#include "corep4_lvgl_ui_sync_internal.h"
#include "ui_function_windows/ui_function_widgets.h"

#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

#if !defined(CONFIG_IDF_TARGET_ESP32P4) || !CONFIG_IDF_TARGET_ESP32P4
#error "The USB_PHY_SEL programming guard is only valid for ESP32-P4"
#endif

#if defined(CONFIG_ESP32P4_SELECTS_REV_LESS_V3) && CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#error "The USB_PHY_SEL programming guard requires the ESP32-P4 V3 eFuse table"
#endif

#if !defined(CONFIG_ESP32P4_REV_MIN_FULL) || CONFIG_ESP32P4_REV_MIN_FULL != 300
#error "The USB_PHY_SEL programming guard requires an ESP32-P4 V3.0 minimum-revision build"
#endif

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_USB_PHY_GUARD";
constexpr uint16_t MIN_CHIP_REVISION = 300; // esp_chip_info MXX format: V3.0.
constexpr uint16_t MAX_CHIP_REVISION = 399; // All V3.x revisions through V3.99.
constexpr esp_efuse_block_t REQUIRED_EFUSE_BLOCK = EFUSE_BLK0;
constexpr uint16_t REQUIRED_EFUSE_BIT = 57;
constexpr uint16_t REQUIRED_EFUSE_WIDTH = 1;
constexpr uint32_t USB_PHY_CHECK_DELAY_MS = 2000;
constexpr uint32_t USB_PHY_CONFIRM_COUNTDOWN_MS = 3000;
constexpr uint32_t USB_PHY_COUNTDOWN_REFRESH_MS = 50;
constexpr uint32_t USB_PHY_RESTART_DELAY_MS = 1000;
constexpr int32_t USB_PHY_COUNTDOWN_BAR_WIDTH = 416;
constexpr int32_t USB_PHY_COUNTDOWN_BAR_HEIGHT = 16;
constexpr int32_t USB_PHY_COUNTDOWN_BAR_BOTTOM_OFFSET = -58;
constexpr uint8_t USB_PHY_BUTTON_NO = 0;
constexpr uint8_t USB_PHY_BUTTON_YES = 1;

struct UsbPhyGuardState {
    CoreP4FunctionDialogHandle dialog = {};
    lv_obj_t* countdown_bar = nullptr;
    lv_timer_t* initial_timer = nullptr;
    lv_timer_t* countdown_timer = nullptr;
    uint32_t countdown_remaining_ms = 0;
    bool started = false;
    bool programming = false;
};

UsbPhyGuardState s_guard = {};
std::atomic<bool> s_burn_task_running{false};

bool read_usb_phy_sel_actual()
{
    return esp_efuse_read_field_bit(ESP_EFUSE_USB_PHY_SEL);
}

bool usb_phy_descriptor_is_expected()
{
    const esp_efuse_desc_t* descriptor = ESP_EFUSE_USB_PHY_SEL[0];
    return descriptor != nullptr &&
           ESP_EFUSE_USB_PHY_SEL[1] == nullptr &&
           descriptor->efuse_block == REQUIRED_EFUSE_BLOCK &&
           descriptor->bit_start == REQUIRED_EFUSE_BIT &&
           descriptor->bit_count == REQUIRED_EFUSE_WIDTH;
}

bool chip_is_esp32p4_v3x(const esp_chip_info_t& chip_info)
{
    return chip_info.model == CHIP_ESP32P4 &&
           chip_info.revision >= MIN_CHIP_REVISION &&
           chip_info.revision <= MAX_CHIP_REVISION;
}

void format_chip_revision(char* buffer, size_t buffer_size, uint16_t revision)
{
    std::snprintf(buffer,
                  buffer_size,
                  "V%u.%u",
                  static_cast<unsigned>(revision / 100U),
                  static_cast<unsigned>(revision % 100U));
}

void delete_timer(lv_timer_t*& timer)
{
    if (!timer) return;
    lv_timer_delete(timer);
    timer = nullptr;
}

void cancel_guard_timers(bool keep_initial = false)
{
    if (!keep_initial) delete_timer(s_guard.initial_timer);
    delete_timer(s_guard.countdown_timer);
}

void guard_dialog_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE ||
        lv_event_get_target(e) != s_guard.dialog.box) {
        return;
    }
    cancel_guard_timers();
    s_guard.dialog = {};
    s_guard.countdown_bar = nullptr;
    s_guard.programming = false;
}

void set_result_bar(bool success)
{
    if (!s_guard.countdown_bar) return;
    lv_bar_set_range(s_guard.countdown_bar, 0, 100);
    lv_bar_set_value(s_guard.countdown_bar, success ? 100 : 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_guard.countdown_bar,
                              lv_color_hex(success ? COLOR_SUCCESS : COLOR_ERROR),
                              LV_PART_INDICATOR);
}

void show_burn_failure(const char* message)
{
    s_guard.programming = false;
    set_result_bar(false);
    corep4_ui_set_function_dialog_text(&s_guard.dialog, message);
    if (s_guard.dialog.buttons[USB_PHY_BUTTON_NO]) {
        set_button_text(s_guard.dialog.buttons[USB_PHY_BUTTON_NO], "Close");
    }
    corep4_ui_set_function_dialog_button_hidden(&s_guard.dialog,
                                                 USB_PHY_BUTTON_NO,
                                                 false);
    corep4_ui_set_function_dialog_button_hidden(&s_guard.dialog,
                                                 USB_PHY_BUTTON_YES,
                                                 true);
}

void show_burn_result(bool success,
                      bool value_before,
                      bool value_after,
                      esp_err_t write_result,
                      const char* failure_reason)
{
    char message[384] = {};

    if (success) {
        std::snprintf(message,
                      sizeof(message),
                      "eFuse operation successful.\n"
                      "USB_PHY_SEL: %u -> %u\n"
                      "Write attempt and readback completed.\n"
                      "Restarting in 1 second...",
                      value_before ? 1U : 0U,
                      value_after ? 1U : 0U);
        set_result_bar(true);
        corep4_ui_set_function_dialog_text(&s_guard.dialog, message);
        return;
    }

    std::snprintf(message,
                  sizeof(message),
                  "eFuse operation failed.\n"
                  "USB_PHY_SEL: %u -> %u\n"
                  "%s (%s)\n"
                  "The device will not restart.",
                  value_before ? 1U : 0U,
                  value_after ? 1U : 0U,
                  failure_reason ? failure_reason : "Unknown error",
                  esp_err_to_name(write_result));
    show_burn_failure(message);
}

void usb_phy_burn_task(void*)
{
    esp_chip_info_t chip_info = {};
    esp_chip_info(&chip_info);

    const bool descriptor_ok = usb_phy_descriptor_is_expected();
    const bool chip_ok = chip_is_esp32p4_v3x(chip_info);
    const bool value_before = read_usb_phy_sel_actual();
    bool value_after = value_before;
    esp_err_t write_result = ESP_ERR_INVALID_VERSION;
    const char* failure_reason = nullptr;
    bool write_attempted = false;

    if (!chip_ok) {
        failure_reason = "Unsupported chip revision";
        ESP_LOGE(TAG,
                 "eFuse write rejected: model=%d revision=%u required_model=%d required_revision=%u..%u",
                 static_cast<int>(chip_info.model),
                 static_cast<unsigned>(chip_info.revision),
                 static_cast<int>(CHIP_ESP32P4),
                 static_cast<unsigned>(MIN_CHIP_REVISION),
                 static_cast<unsigned>(MAX_CHIP_REVISION));
    } else if (!descriptor_ok) {
        write_result = ESP_ERR_INVALID_ARG;
        failure_reason = "USB_PHY_SEL field mismatch";
        ESP_LOGE(TAG, "eFuse write rejected: unexpected USB_PHY_SEL descriptor");
    } else {
        write_attempted = true;
        ESP_LOGW(TAG,
                 "REAL eFuse write attempt: target=ESP32-P4 revision=%u field=USB_PHY_SEL block=0 bit=57 before=%u",
                 static_cast<unsigned>(chip_info.revision),
                 value_before ? 1U : 0U);
        // IDF documents this call as idempotent: an already-set one-bit field
        // returns ESP_OK without attempting repeated physical programming.
        write_result = esp_efuse_write_field_bit(ESP_EFUSE_USB_PHY_SEL);
        value_after = read_usb_phy_sel_actual();
        if (write_result != ESP_OK) {
            failure_reason = "eFuse write failed";
        } else if (!value_after) {
            write_result = ESP_FAIL;
            failure_reason = "Readback failed";
        }
    }

    const bool success = write_attempted && write_result == ESP_OK && value_after;
    ESP_LOGW(TAG,
             "USB_PHY_SEL eFuse result: attempted=%u before=%u after=%u result=%s success=%u",
             write_attempted ? 1U : 0U,
             value_before ? 1U : 0U,
             value_after ? 1U : 0U,
             esp_err_to_name(write_result),
             success ? 1U : 0U);

    if (lvgl_port_lock(2000)) {
        if (s_guard.dialog.box) {
            show_burn_result(success,
                             value_before,
                             value_after,
                             write_result,
                             failure_reason);
        }
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to lock LVGL for USB PHY eFuse result display");
    }

    s_burn_task_running.store(false);
    if (success) {
        vTaskDelay(pdMS_TO_TICKS(USB_PHY_RESTART_DELAY_MS));
        ESP_LOGW(TAG, "USB_PHY_SEL verified; restarting now");
        esp_restart();
    }
    vTaskDelete(nullptr);
}

void begin_real_programming()
{
    if (s_guard.programming || !s_guard.dialog.box || s_burn_task_running.load()) return;

    s_guard.programming = true;
    s_burn_task_running.store(true);
    delete_timer(s_guard.countdown_timer);
    corep4_ui_set_function_dialog_button_hidden(&s_guard.dialog,
                                                 USB_PHY_BUTTON_NO,
                                                 true);
    corep4_ui_set_function_dialog_button_hidden(&s_guard.dialog,
                                                 USB_PHY_BUTTON_YES,
                                                 true);
    corep4_ui_set_function_dialog_text(
        &s_guard.dialog,
        "Programming USB_PHY_SEL = 1...\n"
        "The value will be verified after writing.\n"
        "Do not remove power.");
    if (s_guard.countdown_bar) {
        lv_bar_set_value(s_guard.countdown_bar, 0, LV_ANIM_OFF);
    }

    const BaseType_t task_result = xTaskCreate(usb_phy_burn_task,
                                                "usb_phy_efuse",
                                                4096,
                                                nullptr,
                                                5,
                                                nullptr);
    if (task_result != pdPASS) {
        s_burn_task_running.store(false);
        ESP_LOGE(TAG, "Failed to create USB PHY eFuse programming task");
        show_burn_failure(
            "eFuse operation failed.\n"
            "Could not start programming.\n"
            "The device will not restart.");
    }
}

void no_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_guard.programming) return;
    ESP_LOGI(TAG, "USB PHY programming prompt declined/closed");
    cancel_guard_timers();
    corep4_ui_close_function_dialog(&s_guard.dialog);
    s_guard.countdown_bar = nullptr;
}

void yes_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    begin_real_programming();
}

void countdown_timer_cb(lv_timer_t* timer)
{
    if (timer != s_guard.countdown_timer || s_guard.programming) return;
    if (s_guard.countdown_remaining_ms <= USB_PHY_COUNTDOWN_REFRESH_MS) {
        s_guard.countdown_remaining_ms = 0;
    } else {
        s_guard.countdown_remaining_ms -= USB_PHY_COUNTDOWN_REFRESH_MS;
    }

    if (s_guard.countdown_bar) {
        lv_bar_set_value(s_guard.countdown_bar,
                         static_cast<int32_t>(s_guard.countdown_remaining_ms),
                         LV_ANIM_OFF);
    }
    if (s_guard.countdown_remaining_ms == 0) {
        ESP_LOGW(TAG, "USB PHY confirmation countdown expired; starting protected eFuse flow");
        begin_real_programming();
    }
}

void open_usb_phy_warning_dialog(bool actual_value,
                                 const esp_chip_info_t& chip_info,
                                 bool descriptor_ok)
{
    char revision_text[16] = {};
    char warning_text[448] = {};
    format_chip_revision(revision_text, sizeof(revision_text), chip_info.revision);
    std::snprintf(
        warning_text,
        sizeof(warning_text),
        "Chip: ESP32-P4 %s\n"
        "Current USB_PHY_SEL: %u\n"
        "%s",
        revision_text,
        actual_value ? 1U : 0U,
        descriptor_ok
            ? "USB_PHY_SEL = 1 will be programmed in 3 seconds.\nThis eFuse operation is irreversible."
            : "Safety check failed. Programming is blocked.");

    const CoreP4FunctionDialogButtonConfig buttons[] = {
        {
            "No",
            no_event_cb,
            nullptr,
            COLOR_BUTTON_BG,
            COLOR_TEXT,
            false,
        },
        {
            "Yes",
            yes_event_cb,
            nullptr,
            COLOR_ERROR,
            0xFFFFFF,
            false,
        },
    };

    s_guard.dialog = corep4_ui_open_function_dialog(
        "USB PHY eFuse configuration",
        warning_text,
        CoreP4FunctionDialogSize::MEDIUM,
        buttons,
        sizeof(buttons) / sizeof(buttons[0]));
    if (!s_guard.dialog.box) {
        ESP_LOGE(TAG, "Failed to open USB PHY eFuse warning dialog");
        return;
    }
    lv_obj_add_event_cb(s_guard.dialog.box,
                        guard_dialog_delete_event_cb,
                        LV_EVENT_DELETE,
                        nullptr);

    if (s_guard.dialog.box) {
        // Keep the countdown independent of message text height/scrolling.
        s_guard.countdown_bar = lv_bar_create(s_guard.dialog.box);
        lv_obj_add_flag(s_guard.countdown_bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_add_flag(s_guard.countdown_bar, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(s_guard.countdown_bar,
                        USB_PHY_COUNTDOWN_BAR_WIDTH,
                        USB_PHY_COUNTDOWN_BAR_HEIGHT);
        lv_obj_align(s_guard.countdown_bar,
                     LV_ALIGN_BOTTOM_MID,
                     0,
                     USB_PHY_COUNTDOWN_BAR_BOTTOM_OFFSET);
        lv_bar_set_range(s_guard.countdown_bar,
                         0,
                         USB_PHY_CONFIRM_COUNTDOWN_MS);
        lv_bar_set_value(s_guard.countdown_bar,
                         USB_PHY_CONFIRM_COUNTDOWN_MS,
                         LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_guard.countdown_bar,
                                  lv_color_hex(0xE5E7EB),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_guard.countdown_bar,
                                LV_OPA_COVER,
                                LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_guard.countdown_bar,
                                  lv_color_hex(COLOR_ERROR),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_guard.countdown_bar,
                                LV_OPA_COVER,
                                LV_PART_INDICATOR);
        lv_obj_set_style_border_width(s_guard.countdown_bar, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_guard.countdown_bar,
                                      lv_color_hex(0xCBD5E1),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_opa(s_guard.countdown_bar,
                                    LV_OPA_COVER,
                                    LV_PART_MAIN);
        lv_obj_set_style_radius(s_guard.countdown_bar, 5, LV_PART_MAIN);
        lv_obj_set_style_radius(s_guard.countdown_bar, 5, LV_PART_INDICATOR);
        lv_obj_move_foreground(s_guard.countdown_bar);
    }

    lv_obj_t* footer = lv_msgbox_get_footer(s_guard.dialog.box);
    if (footer) {
        lv_obj_set_width(footer, lv_pct(100));
        lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(footer,
                              LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
    }

    s_guard.countdown_remaining_ms = USB_PHY_CONFIRM_COUNTDOWN_MS;
    s_guard.countdown_timer = lv_timer_create(countdown_timer_cb,
                                               USB_PHY_COUNTDOWN_REFRESH_MS,
                                               nullptr);
    if (!s_guard.countdown_timer) {
        ESP_LOGE(TAG, "Failed to create USB PHY confirmation countdown timer");
    }
}

void initial_check_timer_cb(lv_timer_t* timer)
{
    if (timer != s_guard.initial_timer) return;
    s_guard.initial_timer = nullptr;

    esp_chip_info_t chip_info = {};
    esp_chip_info(&chip_info);
    const bool actual_value = read_usb_phy_sel_actual();
    const bool descriptor_ok = usb_phy_descriptor_is_expected();
    ESP_LOGW(TAG,
             "Startup USB_PHY_SEL prompt: model=%d revision=%u actual=%u descriptor_blk0_bit57_width1=%u",
             static_cast<int>(chip_info.model),
             static_cast<unsigned>(chip_info.revision),
             actual_value ? 1U : 0U,
             descriptor_ok ? 1U : 0U);

    if (actual_value) {
        ESP_LOGI(TAG, "USB_PHY_SEL is already 1; startup prompt skipped");
        return;
    }

    // Only an unprogrammed USB_PHY_SEL needs operator confirmation.
    open_usb_phy_warning_dialog(actual_value, chip_info, descriptor_ok);
}

} // namespace

esp_err_t corep4_ui_usb_phy_guard_start()
{
    if (s_guard.started) return ESP_OK;
    s_guard.started = true;
    s_guard.initial_timer = lv_timer_create(initial_check_timer_cb,
                                             USB_PHY_CHECK_DELAY_MS,
                                             nullptr);
    if (!s_guard.initial_timer) {
        s_guard.started = false;
        ESP_LOGE(TAG, "Failed to create USB PHY startup check timer");
        return ESP_ERR_NO_MEM;
    }
    lv_timer_set_repeat_count(s_guard.initial_timer, 1);
    ESP_LOGW(TAG,
             "REAL USB PHY eFuse guard scheduled: delay=%lums target=ESP32-P4-V3.x(300..399) field=BLK0.bit57 popup_on_zero=1",
             static_cast<unsigned long>(USB_PHY_CHECK_DELAY_MS));
    return ESP_OK;
}
