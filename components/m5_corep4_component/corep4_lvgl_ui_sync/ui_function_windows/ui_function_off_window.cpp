#include "ui_function_widgets.h"

#if __has_include("app_fw_version.h")
#include "app_fw_version.h"
#endif

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>
#include <cstdio>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_OFF";
constexpr uint32_t TERMINAL_ACTION_BACKLIGHT_SETTLE_MS = 120;

#ifndef APP_FW_VERSION
#define APP_FW_VERSION "V0.000000-000000"
#endif

struct OffPageState {
    lv_obj_t* status_label = nullptr;
    lv_obj_t* version_label = nullptr;
    lv_obj_t* action_label = nullptr;
    lv_obj_t* boot_sound_button = nullptr;
    lv_obj_t* poweroff_button = nullptr;
    lv_obj_t* restart_button = nullptr;
    lv_obj_t* countdown_bar = nullptr;
    volatile bool page_alive = false;
    volatile bool task_running = false;
    volatile bool cancel_requested = false;
    int pending_action = 0;  // 1=poweroff, 2=restart
    char action_text[96] = "Ready.";
    uint32_t action_color = COLOR_MUTED;
};

OffPageState s_off_page;

void apply_btn_colors(lv_obj_t* btn, uint32_t bg, uint32_t fg)
{
    if (!btn) return;
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_t* lbl = lv_obj_get_child(btn, 0);
    if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(fg), LV_PART_MAIN);
}

void dim_btn(lv_obj_t* btn)
{
    if (!btn) return;
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(btn, LV_OPA_40, LV_PART_MAIN);
}

void undim_btn(lv_obj_t* btn)
{
    if (!btn) return;
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_state(btn, LV_STATE_DISABLED);
}

void sync_action_label()
{
    set_label_text(s_off_page.action_label, s_off_page.action_text);
    if (s_off_page.action_label) {
        lv_obj_set_style_text_color(s_off_page.action_label,
                                    lv_color_hex(s_off_page.action_color),
                                    LV_PART_MAIN);
    }
}

void set_action_text(const char* text, uint32_t color)
{
    std::snprintf(s_off_page.action_text,
                  sizeof(s_off_page.action_text),
                  "%s",
                  text ? text : "");
    s_off_page.action_color = color;
    sync_action_label();
}

void refresh_boot_sound_button()
{
    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        set_button_text(s_off_page.boot_sound_button, "Sound --");
        set_button_colors(s_off_page.boot_sound_button, COLOR_BUTTON_BG, COLOR_MUTED);
        set_button_enabled(s_off_page.boot_sound_button, false);
        return;
    }

    const bool enabled = tool->get_startup_prompt_audio_enabled();
    set_button_enabled(s_off_page.boot_sound_button, true);
    set_button_text(s_off_page.boot_sound_button, enabled ? "Sound ON" : "Sound OFF");
    set_button_colors(s_off_page.boot_sound_button,
                      enabled ? COLOR_SUCCESS : COLOR_BUTTON_BG,
                      enabled ? 0xFFFFFF : COLOR_TEXT);
}

void refresh_poweroff_ui()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;

    if (!status) {
        set_label_text(s_off_page.status_label,
                       "PM1: --  PYB: --  Task: --");
        set_action_text("Device tool is not ready.", COLOR_ERROR);
        refresh_boot_sound_button();
        dim_btn(s_off_page.poweroff_button);
        dim_btn(s_off_page.restart_button);
        return;
    }

    char status_buf[96];
    std::snprintf(status_buf, sizeof(status_buf),
                  "PM1: %s  PYB: %s  Task: %s",
                  status->power.pm1_initialized ? "Ready" : "Not ready",
                  status->power.pyb_initialized ? "Ready" : "Not ready",
                  s_off_page.task_running ? "Running" : "Idle");
    set_label_text(s_off_page.status_label, status_buf);
    sync_action_label();
    refresh_boot_sound_button();

    const bool pm1_ok = status->power.pm1_initialized;
    const bool idle = !s_off_page.task_running;

    if (!idle) {
        if (s_off_page.pending_action == 1) {
            set_button_text(s_off_page.poweroff_button, "Cancel");
            apply_btn_colors(s_off_page.poweroff_button, 0x4B5563, 0xFFFFFF);
            undim_btn(s_off_page.poweroff_button);
            dim_btn(s_off_page.restart_button);
        } else if (s_off_page.pending_action == 2) {
            set_button_text(s_off_page.restart_button, "Cancel");
            apply_btn_colors(s_off_page.restart_button, 0x4B5563, 0xFFFFFF);
            undim_btn(s_off_page.restart_button);
            dim_btn(s_off_page.poweroff_button);
        }
        return;
    }

    set_button_text(s_off_page.poweroff_button, "Power Off");
    apply_btn_colors(s_off_page.poweroff_button, COLOR_ERROR, 0xFFFFFF);
    if (pm1_ok && idle) {
        undim_btn(s_off_page.poweroff_button);
    } else {
        dim_btn(s_off_page.poweroff_button);
    }

    set_button_text(s_off_page.restart_button, "ESP Restart");
    apply_btn_colors(s_off_page.restart_button, COLOR_WARNING, 0xFFFFFF);
    if (idle) {
        undim_btn(s_off_page.restart_button);
    } else {
        dim_btn(s_off_page.restart_button);
    }

    if (s_off_page.countdown_bar && idle) {
        lv_bar_set_value(s_off_page.countdown_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_off_page.countdown_bar,
                                   lv_color_hex(0xE5E7EB),
                                   LV_PART_INDICATOR);
    }
}

void update_ui_from_task(const char* text, uint32_t color, bool refresh)
{
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock LVGL");
        return;
    }
    if (s_off_page.page_alive) {
        set_action_text(text, color);
        if (refresh) refresh_poweroff_ui();
    }
    lvgl_port_unlock();
}

void update_bar_from_task(int pct)
{
    if (!lvgl_port_lock(50)) return;
    if (s_off_page.page_alive && s_off_page.countdown_bar) {
        lv_bar_set_value(s_off_page.countdown_bar, pct, LV_ANIM_OFF);
    }
    lvgl_port_unlock();
}

esp_err_t prepare_display_before_terminal_power_action(m5_corep4_tool* tool)
{
    if (!tool) {
        ESP_LOGW(TAG, "Cannot turn off display backlight: device tool is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Display: LCD backlight -> 0 before terminal power action");
    esp_err_t ret = tool->set_status(StatusParam::POWER_LCD_BRIGHTNESS,
                                     static_cast<uint8_t>(0),
                                     StatusConfigFlag::APPLY_HARDWARE |
                                         StatusConfigFlag::UPDATE_STATUS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to turn off LCD backlight before terminal action: %s",
                 esp_err_to_name(ret));
    }

    vTaskDelay(pdMS_TO_TICKS(TERMINAL_ACTION_BACKLIGHT_SETTLE_MS));
    return ret;
}

void countdown_task(void* param)
{
    const int action = static_cast<int>(reinterpret_cast<intptr_t>(param));

    for (int i = 30; i >= 0; i--) {
        if (!s_off_page.page_alive || s_off_page.cancel_requested) {
            s_off_page.task_running = false;
            s_off_page.pending_action = 0;
            update_ui_from_task("Cancelled.", COLOR_MUTED, true);
            vTaskDelete(nullptr);
            return;
        }
        update_bar_from_task(i * 100 / 30);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    s_off_page.pending_action = 0;

    if (action == 1) {
        auto* tool = corep4_ui_sync_get_tool();
        if (!tool) {
            s_off_page.task_running = false;
            update_ui_from_task("Power off failed: device not ready.", COLOR_ERROR, true);
            vTaskDelete(nullptr);
            return;
        }

        update_ui_from_task("Turning display off...", COLOR_WARNING, false);
        prepare_display_before_terminal_power_action(tool);
        update_ui_from_task("Sending PM1 shutdown command...", COLOR_WARNING, false);
        ESP_LOGI(TAG, "Power off requested from function window");
        m5pm1_err_t ret = tool->pm1.shutdown();
        s_off_page.task_running = false;
        if (ret == M5PM1_OK) {
            update_ui_from_task("PM1 OFF command sent. Powering down...",
                                COLOR_SUCCESS,
                                true);
        } else {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "Power off failed: PM1 error %d.",
                          static_cast<int>(ret));
            update_ui_from_task(msg, COLOR_ERROR, true);
        }
    } else {
        auto* tool = corep4_ui_sync_get_tool();
        update_ui_from_task("Turning display off...", COLOR_WARNING, false);
        prepare_display_before_terminal_power_action(tool);
        update_ui_from_task("Restarting...", COLOR_WARNING, false);
        ESP_LOGI(TAG, "ESP restart requested from function window");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }

    vTaskDelete(nullptr);
}

void start_countdown(int action)
{
    s_off_page.task_running = true;
    s_off_page.cancel_requested = false;
    s_off_page.pending_action = action;

    const uint32_t bar_color = (action == 1) ? COLOR_ERROR : COLOR_WARNING;

    if (action == 1) {
        set_button_text(s_off_page.poweroff_button, "Cancel");
        apply_btn_colors(s_off_page.poweroff_button, 0x4B5563, 0xFFFFFF);
        undim_btn(s_off_page.poweroff_button);
        dim_btn(s_off_page.restart_button);
    } else {
        set_button_text(s_off_page.restart_button, "Cancel");
        apply_btn_colors(s_off_page.restart_button, 0x4B5563, 0xFFFFFF);
        undim_btn(s_off_page.restart_button);
        dim_btn(s_off_page.poweroff_button);
    }

    if (s_off_page.countdown_bar) {
        lv_bar_set_value(s_off_page.countdown_bar, 100, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_off_page.countdown_bar,
                                   lv_color_hex(bar_color),
                                   LV_PART_INDICATOR);
    }

    set_action_text(action == 1 ? "Power off in 3 s..." : "Restart in 3 s...",
                    COLOR_WARNING);
    refresh_poweroff_ui();

    BaseType_t ret = xTaskCreate(countdown_task,
                                 "func_cd",
                                 3072,
                                 reinterpret_cast<void*>(static_cast<intptr_t>(action)),
                                 configMAX_PRIORITIES - 4,
                                 nullptr);
    if (ret != pdPASS) {
        s_off_page.task_running = false;
        s_off_page.pending_action = 0;
        set_action_text("Error: cannot create countdown task.", COLOR_ERROR);
        refresh_poweroff_ui();
    }
}

void poweroff_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (s_off_page.task_running && s_off_page.pending_action == 1) {
        s_off_page.cancel_requested = true;
        return;
    }
    if (s_off_page.task_running) return;

    start_countdown(1);
}

void restart_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (s_off_page.task_running && s_off_page.pending_action == 2) {
        s_off_page.cancel_requested = true;
        return;
    }
    if (s_off_page.task_running) return;

    start_countdown(2);
}

void boot_sound_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        set_action_text("Boot sound update failed: device not ready.", COLOR_ERROR);
        refresh_boot_sound_button();
        return;
    }

    const bool next = !tool->get_startup_prompt_audio_enabled();
    esp_err_t ret = tool->set_startup_prompt_audio_enabled(next);
    if (ret == ESP_OK) {
        set_action_text(next ? "Startup prompt sound enabled."
                             : "Startup prompt sound disabled.",
                        COLOR_SUCCESS);
    } else {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "Boot sound save failed: %s.",
                      esp_err_to_name(ret));
        set_action_text(msg, COLOR_ERROR);
    }
    refresh_poweroff_ui();
}

void off_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;

    s_off_page.page_alive = false;
    s_off_page.status_label = nullptr;
    s_off_page.version_label = nullptr;
    s_off_page.action_label = nullptr;
    s_off_page.boot_sound_button = nullptr;
    s_off_page.poweroff_button = nullptr;
    s_off_page.restart_button = nullptr;
    s_off_page.countdown_bar = nullptr;
    s_off_page.pending_action = 0;
}

lv_obj_t* add_plain_box(lv_obj_t* parent, int32_t width, int32_t height)
{
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    return box;
}

void build_content(lv_obj_t* parent, const CoreP4FunctionWindowConfig& config)
{
    (void)config;

    prepare_content(parent);
    s_off_page.page_alive = true;
    std::snprintf(s_off_page.action_text, sizeof(s_off_page.action_text), "Ready.");
    s_off_page.action_color = COLOR_MUTED;
    lv_obj_add_event_cb(parent, off_content_delete_event_cb, LV_EVENT_DELETE, nullptr);

    lv_obj_t* card = add_card(parent, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 10, LV_PART_MAIN);

    lv_obj_t* status_row = add_row(card, 94);
    add_label(status_row, "Status", COLOR_MUTED, 110);
    lv_obj_t* status_box = add_plain_box(status_row, 278, 92);
    s_off_page.status_label = add_label(status_box,
                                        "PM1: --  PYB: --  Task: Idle",
                                        COLOR_TEXT,
                                        278);
    lv_label_set_long_mode(s_off_page.status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_off_page.status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    s_off_page.version_label = add_label(status_box, APP_FW_VERSION, COLOR_MUTED, 278);
    lv_label_set_long_mode(s_off_page.version_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_off_page.version_label, &lv_font_montserrat_14, LV_PART_MAIN);
    s_off_page.action_label = add_label(status_box, "Ready.", COLOR_MUTED, 278);
    lv_label_set_long_mode(s_off_page.action_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_off_page.action_label, &lv_font_montserrat_14, LV_PART_MAIN);

    lv_obj_t* sound_row = add_row(card, 46);
    add_label(sound_row, "Boot Sound", COLOR_MUTED, 110);
    s_off_page.boot_sound_button = add_button(sound_row,
                                              "Sound --",
                                              COLOR_BUTTON_BG,
                                              COLOR_TEXT,
                                              282,
                                              46);
    lv_obj_add_event_cb(s_off_page.boot_sound_button,
                        boot_sound_button_event_cb,
                        LV_EVENT_CLICKED,
                        nullptr);

    lv_obj_t* poweroff_row = add_row(card, 58);
    add_label(poweroff_row, "Power Off", COLOR_MUTED, 110);
    s_off_page.poweroff_button = add_button(poweroff_row,
                                            "Power Off",
                                            COLOR_ERROR,
                                            0xFFFFFF,
                                            282,
                                            46);
    lv_obj_add_event_cb(s_off_page.poweroff_button,
                        poweroff_button_event_cb,
                        LV_EVENT_CLICKED,
                        nullptr);

    lv_obj_t* restart_row = add_row(card, 58);
    add_label(restart_row, "Restart", COLOR_MUTED, 110);
    s_off_page.restart_button = add_button(restart_row,
                                           "ESP Restart",
                                           COLOR_WARNING,
                                           0xFFFFFF,
                                           282,
                                           46);
    lv_obj_add_event_cb(s_off_page.restart_button,
                        restart_button_event_cb,
                        LV_EVENT_CLICKED,
                        nullptr);

    s_off_page.countdown_bar = lv_bar_create(card);
    lv_obj_set_size(s_off_page.countdown_bar, lv_pct(100), 14);
    lv_bar_set_range(s_off_page.countdown_bar, 0, 100);
    lv_bar_set_value(s_off_page.countdown_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_off_page.countdown_bar,
                               lv_color_hex(0xE5E7EB),
                               LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_off_page.countdown_bar,
                               lv_color_hex(0xE5E7EB),
                               LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_off_page.countdown_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_off_page.countdown_bar, 4, LV_PART_INDICATOR);

    refresh_poweroff_ui();
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "Off",
    "click on ui_func1off1con",
    build_content,
};

} // namespace

extern "C" void func2off2clicked2func(lv_event_t* e)
{
    corep4_ui_open_function_window(e, WINDOW_CONFIG);
}
