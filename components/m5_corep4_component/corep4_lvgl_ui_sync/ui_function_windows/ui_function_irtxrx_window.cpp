#include "ui_function_widgets.h"

#include "ir_codec_types.h"
#include "ir_tools.h"
#include "m5_corep4_pinmap.h"
#include "m5_corep4_status_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_IRTXRX";
constexpr uint32_t RX_REFRESH_MS = 250;

constexpr int32_t TX_PANEL_H = 260;
constexpr int32_t RX_PANEL_H = 122;
constexpr int32_t PANEL_RADIUS = 8;
constexpr int32_t TILE_W = 99;
constexpr int32_t TILE_H = 132;
constexpr int32_t TILE_GAP = 6;
constexpr int32_t TILE_Y = 62;
constexpr int32_t DRAG_STEP_PX = 12;
constexpr int32_t SEND_Y = 206;
constexpr int32_t SEND_H = 40;

constexpr uint32_t COLOR_PANEL_TX_BG = 0xF8FAFC;
constexpr uint32_t COLOR_PANEL_RX_BG = 0xF0FDF4;
constexpr uint32_t COLOR_TILE_BG = 0xFFFFFF;
constexpr uint32_t COLOR_TX_ACCENT = 0x2563EB;
constexpr uint32_t COLOR_RX_ACCENT = 0x16A34A;

enum class NecByteField : uint8_t {
    ADDR_HI,
    ADDR_LO,
    CMD_HI,
    CMD_LO,
};

struct NecByteEditor {
    NecByteField field = NecByteField::ADDR_HI;
    lv_obj_t* tile = nullptr;
    lv_obj_t* name_label = nullptr;
    lv_obj_t* value_label = nullptr;
    int32_t drag_accum = 0;
};

struct IrRxCache {
    ir_decode_result_t newest;
    ir_decode_result_t last;
    bool has_new = false;
    bool has_last = false;
    uint32_t count = 0;
};

struct IrPageState {
    lv_obj_t* status_label = nullptr;
    lv_obj_t* result_label = nullptr;
    lv_obj_t* send_button = nullptr;
    lv_obj_t* accum_button = nullptr;
    lv_obj_t* repeat_button = nullptr;
    lv_obj_t* rx_count_label = nullptr;
    lv_obj_t* rx_format_label = nullptr;
    lv_obj_t* rx_addr_label = nullptr;
    lv_obj_t* rx_cmd_label = nullptr;
    lv_obj_t* rx_repeat_label = nullptr;
    lv_obj_t* rx_raw_label = nullptr;
    lv_obj_t* rx_clear_button = nullptr;
    NecByteEditor byte_editors[4];
    lv_timer_t* timer = nullptr;
    volatile bool page_alive = false;
    volatile bool init_task_running = false;
    volatile bool tx_task_running = false;
    bool ir_init_done = false;
    bool tx_repeat = false;
    bool auto_increment = false;
    uint32_t tx_address = 0x0004;
    uint32_t tx_command = 0x0008;
    ir_send_request_t pending_tx = {};
};

static IrRxCache s_ir_rx_cache;
static portMUX_TYPE s_ir_rx_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_ir_callback_registered = false;

IrPageState s_ir_page;

void start_ir_init_task(const char* text);

uint32_t compose_tx_address()
{
    return s_ir_page.tx_address & 0xFFFFu;
}

uint32_t compose_tx_command()
{
    return s_ir_page.tx_command & 0xFFFFu;
}

uint8_t get_nec_byte(NecByteField field)
{
    switch (field) {
        case NecByteField::ADDR_HI:
            return static_cast<uint8_t>((compose_tx_address() >> 8) & 0xFFu);
        case NecByteField::ADDR_LO:
            return static_cast<uint8_t>(compose_tx_address() & 0xFFu);
        case NecByteField::CMD_HI:
            return static_cast<uint8_t>((compose_tx_command() >> 8) & 0xFFu);
        case NecByteField::CMD_LO:
            return static_cast<uint8_t>(compose_tx_command() & 0xFFu);
        default:
            return 0;
    }
}

void set_nec_byte(NecByteField field, uint8_t value)
{
    switch (field) {
        case NecByteField::ADDR_HI:
            s_ir_page.tx_address = (compose_tx_address() & 0x00FFu) |
                                   (static_cast<uint32_t>(value) << 8);
            break;
        case NecByteField::ADDR_LO:
            s_ir_page.tx_address = (compose_tx_address() & 0xFF00u) |
                                   static_cast<uint32_t>(value);
            break;
        case NecByteField::CMD_HI:
            s_ir_page.tx_command = (compose_tx_command() & 0x00FFu) |
                                   (static_cast<uint32_t>(value) << 8);
            break;
        case NecByteField::CMD_LO:
            s_ir_page.tx_command = (compose_tx_command() & 0xFF00u) |
                                   static_cast<uint32_t>(value);
            break;
        default:
            break;
    }
}

void adjust_nec_byte(NecByteField field, int delta)
{
    const int value = (static_cast<int>(get_nec_byte(field)) + delta) & 0xFF;
    set_nec_byte(field, static_cast<uint8_t>(value));
}

void set_label_color(lv_obj_t* label, uint32_t color)
{
    if (label) {
        lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    }
}

void set_result_text(const char* text, uint32_t color)
{
    set_label_text(s_ir_page.result_label, text);
    set_label_color(s_ir_page.result_label, color);
}

void set_field_label(lv_obj_t* label, const char* text, uint32_t color)
{
    set_label_text(label, text);
    set_label_color(label, color);
}

void style_label(lv_obj_t* label,
                 const lv_font_t* font,
                 uint32_t color,
                 lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    if (!label) {
        return;
    }
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
}

lv_obj_t* create_panel(lv_obj_t* parent, int32_t height, uint32_t bg_color)
{
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, height);
    lv_obj_set_style_bg_color(panel, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, PANEL_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    return panel;
}

lv_obj_t* create_abs_label(lv_obj_t* parent,
                           const char* text,
                           int32_t x,
                           int32_t y,
                           int32_t width,
                           int32_t height,
                           const lv_font_t* font,
                           uint32_t color,
                           lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    style_label(label, font, color, align);
    return label;
}

lv_obj_t* create_abs_button(lv_obj_t* parent,
                            const char* text,
                            int32_t x,
                            int32_t y,
                            int32_t width,
                            int32_t height,
                            uint32_t bg_color,
                            uint32_t text_color,
                            const lv_font_t* font = &lv_font_montserrat_16,
                            uint32_t border_color = COLOR_BORDER)
{
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(border_color), LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text ? text : "");
    style_label(label, font, text_color, LV_TEXT_ALIGN_CENTER);
    lv_obj_center(label);
    return button;
}

void refresh_tx_value_labels(bool update_result = true)
{
    for (auto& editor : s_ir_page.byte_editors) {
        if (!editor.value_label) {
            continue;
        }
        char value[8];
        std::snprintf(value, sizeof(value), "%02X", get_nec_byte(editor.field));
        lv_label_set_text(editor.value_label, value);
    }

    char result[96];
    std::snprintf(result, sizeof(result),
                  "NEC addr 0x%04lX  cmd 0x%04lX",
                  static_cast<unsigned long>(compose_tx_address()),
                  static_cast<unsigned long>(compose_tx_command()));
    if (update_result) {
        set_result_text(result, s_ir_page.ir_init_done ? COLOR_MUTED : COLOR_WARNING);
    }
}

void update_repeat_button()
{
    set_button_text(s_ir_page.repeat_button,
                    s_ir_page.tx_repeat ? "Repeat x2" : "Repeat x1");
    set_button_colors(s_ir_page.repeat_button,
                      s_ir_page.tx_repeat ? 0xFEF3C7 : COLOR_BUTTON_BG,
                      s_ir_page.tx_repeat ? COLOR_WARNING : COLOR_TEXT);
}

void update_auto_increment_button()
{
    set_button_text(s_ir_page.accum_button,
                    s_ir_page.auto_increment ? "Auto+1 ON" : "Auto+1");
    set_button_colors(s_ir_page.accum_button,
                      s_ir_page.auto_increment ? 0xDBEAFE : COLOR_BUTTON_BG,
                      s_ir_page.auto_increment ? COLOR_TX_ACCENT : COLOR_TEXT);
}

void set_tx_send_enabled(bool enabled)
{
    set_button_enabled(s_ir_page.send_button, enabled);
}

void refresh_ir_status()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    const bool tool_ready = tool && tool->otool_tools.ir.is_initialized();
    if (tool_ready) {
        s_ir_page.ir_init_done = true;
    }

    char text[128];
    std::snprintf(text, sizeof(text), "Init:%s  L3B:%s  RX:%s",
                  s_ir_page.ir_init_done ? "OK" :
                      (s_ir_page.init_task_running ? "WORK" : "NO"),
                  status && status->power.pyb_l3b_power ? "ON" : "OFF",
                  s_ir_page.ir_init_done ? "NEC" : "--");
    set_label_text(s_ir_page.status_label, text);
    set_label_color(s_ir_page.status_label,
                    s_ir_page.ir_init_done ? COLOR_SUCCESS :
                    (s_ir_page.init_task_running ? COLOR_WARNING : COLOR_ERROR));

    set_tx_send_enabled(s_ir_page.ir_init_done &&
                        !s_ir_page.init_task_running &&
                        !s_ir_page.tx_task_running);

    set_button_enabled(s_ir_page.accum_button, !s_ir_page.tx_task_running);
    update_repeat_button();
    update_auto_increment_button();
}

void update_ir_ui_from_task(const char* text, uint32_t color)
{
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock LVGL for IR update");
        return;
    }
    if (s_ir_page.page_alive) {
        set_result_text(text, color);
        refresh_ir_status();
    }
    lvgl_port_unlock();
}

void update_tx_ui_from_task(const char* text, uint32_t color)
{
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock LVGL for IR TX update");
        return;
    }
    if (s_ir_page.page_alive) {
        refresh_tx_value_labels(false);
        set_result_text(text, color);
        refresh_ir_status();
    }
    lvgl_port_unlock();
}

void ir_rx_on_decoded(const ir_decode_result_t& result)
{
    portENTER_CRITICAL(&s_ir_rx_mux);
    s_ir_rx_cache.newest = result;
    s_ir_rx_cache.last = result;
    s_ir_rx_cache.has_new = true;
    s_ir_rx_cache.has_last = true;
    ++s_ir_rx_cache.count;
    portEXIT_CRITICAL(&s_ir_rx_mux);
}

void set_rx_empty_fields()
{
    set_field_label(s_ir_page.rx_format_label, "Protocol: --", COLOR_MUTED);
    set_field_label(s_ir_page.rx_addr_label, "Address: --", COLOR_MUTED);
    set_field_label(s_ir_page.rx_cmd_label, "Command: --", COLOR_MUTED);
    set_field_label(s_ir_page.rx_repeat_label, "Repeat: --", COLOR_MUTED);
    set_field_label(s_ir_page.rx_raw_label, "Raw frame: --", COLOR_MUTED);
}

void set_rx_decoded_fields(const ir_decode_result_t& rx)
{
    char text[48];

    std::snprintf(text, sizeof(text), "Protocol: %s", ir_format_name(rx.format));
    set_field_label(s_ir_page.rx_format_label, text, COLOR_SUCCESS);

    std::snprintf(text, sizeof(text), "Address: 0x%04lX",
                  static_cast<unsigned long>(rx.address & 0xFFFFu));
    set_field_label(s_ir_page.rx_addr_label, text, COLOR_TEXT);

    std::snprintf(text, sizeof(text), "Command: 0x%04lX",
                  static_cast<unsigned long>(rx.command & 0xFFFFu));
    set_field_label(s_ir_page.rx_cmd_label, text, COLOR_TEXT);

    std::snprintf(text, sizeof(text), "Repeat: %s", rx.is_repeat ? "YES" : "NO");
    set_field_label(s_ir_page.rx_repeat_label, text,
                    rx.is_repeat ? COLOR_WARNING : COLOR_MUTED);

    std::snprintf(text, sizeof(text), "Raw frame: 0x%08lX",
                  static_cast<unsigned long>(rx.raw_value));
    set_field_label(s_ir_page.rx_raw_label, text, COLOR_MUTED);
}

void ir_timer_cb(lv_timer_t* /*timer*/)
{
    if (!s_ir_page.page_alive) {
        return;
    }

    refresh_ir_status();

    bool has_new = false;
    bool has_last = false;
    uint32_t count = 0;
    ir_decode_result_t rx{};
    portENTER_CRITICAL(&s_ir_rx_mux);
    if (s_ir_rx_cache.has_new) {
        rx = s_ir_rx_cache.newest;
        has_new = true;
        s_ir_rx_cache.has_new = false;
    }
    has_last = s_ir_rx_cache.has_last;
    count = s_ir_rx_cache.count;
    portEXIT_CRITICAL(&s_ir_rx_mux);

    char count_text[40];
    std::snprintf(count_text, sizeof(count_text), "Frames:%lu",
                  static_cast<unsigned long>(count));
    set_label_text(s_ir_page.rx_count_label, count_text);

    if (!has_last) {
        return;
    }

    if (has_new) {
        set_rx_decoded_fields(rx);
    }
}

void ir_init_task(void* /*param*/)
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;

    if (!tool) {
        s_ir_page.init_task_running = false;
        update_ir_ui_from_task("Tool not available.", COLOR_ERROR);
        vTaskDelete(nullptr);
        return;
    }

    tool->set_status(StatusParam::POWER_PYB_L3B, true,
                     StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::APPLY_HARDWARE);
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t ret = ESP_OK;
    if (!tool->otool_tools.ir.is_initialized()) {
        ret = tool->otool_tools.ir.init(Pin_P4_IR_TX, Pin_P4_IR_RX, Pin_P4_IR_EN);
    }

    if (ret == ESP_OK) {
        if (!s_ir_callback_registered) {
            tool->otool_tools.ir.subscribe(ir_format_t::NEC, ir_rx_on_decoded);
            s_ir_callback_registered = true;
        }
        ret = tool->otool_tools.ir.start_receive();
    }

    if (ret == ESP_OK) {
        s_ir_page.ir_init_done = true;
        if (status) {
            status->ir.ir_initialized = true;
            status->ir.current_format = ir_format_t::NEC;
        }
    }

    char msg[128];
    std::snprintf(msg, sizeof(msg), "IR TX/RX init %s: %s",
                  ret == ESP_OK ? "OK" : "failed",
                  esp_err_to_name(ret));
    s_ir_page.init_task_running = false;
    update_ir_ui_from_task(msg, ret == ESP_OK ? COLOR_SUCCESS : COLOR_ERROR);
    vTaskDelete(nullptr);
}

void ir_send_task(void* /*param*/)
{
    auto* tool = corep4_ui_sync_get_tool();
    const ir_send_request_t request = s_ir_page.pending_tx;
    uint32_t color = COLOR_ERROR;
    char msg[128];
    bool attempted_send = false;

    if (!tool || !s_ir_page.ir_init_done) {
        std::snprintf(msg, sizeof(msg), "IR is not initialized yet.");
        color = COLOR_WARNING;
    } else {
        attempted_send = true;
        esp_err_t ret = tool->otool_tools.ir.send(request);
        color = ret == ESP_OK ? COLOR_SUCCESS : COLOR_ERROR;
        if (s_ir_page.auto_increment) {
            s_ir_page.tx_command = (request.command + 1u) & 0xFFFFu;
            std::snprintf(msg, sizeof(msg),
                          "TX 0x%04lX/0x%04lX -> %s  next cmd 0x%04lX",
                          static_cast<unsigned long>(request.address & 0xFFFFu),
                          static_cast<unsigned long>(request.command & 0xFFFFu),
                          esp_err_to_name(ret),
                          static_cast<unsigned long>(compose_tx_command()));
        } else {
            std::snprintf(msg, sizeof(msg),
                          "TX NEC addr 0x%04lX cmd 0x%04lX -> %s",
                          static_cast<unsigned long>(request.address & 0xFFFFu),
                          static_cast<unsigned long>(request.command & 0xFFFFu),
                          esp_err_to_name(ret));
        }
    }

    s_ir_page.tx_task_running = false;
    if (attempted_send) {
        update_tx_ui_from_task(msg, color);
    } else {
        update_ir_ui_from_task(msg, color);
    }
    vTaskDelete(nullptr);
}

void start_ir_init_task(const char* text)
{
    if (s_ir_page.init_task_running) {
        return;
    }

    s_ir_page.init_task_running = true;
    set_result_text(text ? text : "Initializing IR TX/RX...", COLOR_WARNING);
    refresh_ir_status();

    BaseType_t ret = xTaskCreate(ir_init_task, "func_ir_init", 4096, nullptr,
                                 configMAX_PRIORITIES - 4, nullptr);
    if (ret != pdPASS) {
        s_ir_page.init_task_running = false;
        set_result_text("Cannot create IR init task.", COLOR_ERROR);
        refresh_ir_status();
    }
}

void ir_init_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    start_ir_init_task("Reinitializing IR TX/RX...");
}

void ir_send_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED ||
        s_ir_page.init_task_running ||
        s_ir_page.tx_task_running) {
        return;
    }

    s_ir_page.pending_tx = {};
    s_ir_page.pending_tx.format = ir_format_t::NEC;
    s_ir_page.pending_tx.address = compose_tx_address();
    s_ir_page.pending_tx.command = compose_tx_command();
    s_ir_page.pending_tx.is_repeat = s_ir_page.tx_repeat;

    s_ir_page.tx_task_running = true;
    set_result_text("Sending NEC frame...", COLOR_WARNING);
    refresh_ir_status();

    BaseType_t ret = xTaskCreate(ir_send_task, "func_ir_tx", 4096, nullptr,
                                 configMAX_PRIORITIES - 4, nullptr);
    if (ret != pdPASS) {
        s_ir_page.tx_task_running = false;
        set_result_text("Cannot create IR send task.", COLOR_ERROR);
        refresh_ir_status();
    }
}

void style_byte_tile(NecByteEditor* editor, bool active)
{
    if (!editor || !editor->tile) {
        return;
    }

    lv_obj_set_style_border_color(editor->tile,
                                  lv_color_hex(active ? COLOR_TX_ACCENT : COLOR_BORDER),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(editor->tile, active ? 2 : 1, LV_PART_MAIN);
    set_label_color(editor->value_label, active ? COLOR_TX_ACCENT : COLOR_TEXT);
}

void byte_drag_event_cb(lv_event_t* e)
{
    auto* editor = static_cast<NecByteEditor*>(lv_event_get_user_data(e));
    if (!editor) {
        return;
    }

    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        editor->drag_accum = 0;
        style_byte_tile(editor, true);
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        editor->drag_accum = 0;
        style_byte_tile(editor, false);
        return;
    }

    if (code != LV_EVENT_PRESSING) {
        return;
    }

    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }

    lv_point_t vect{};
    lv_indev_get_vect(indev, &vect);
    editor->drag_accum -= vect.y;

    int steps = 0;
    while (editor->drag_accum >= DRAG_STEP_PX) {
        ++steps;
        editor->drag_accum -= DRAG_STEP_PX;
    }
    while (editor->drag_accum <= -DRAG_STEP_PX) {
        --steps;
        editor->drag_accum += DRAG_STEP_PX;
    }

    if (steps != 0) {
        adjust_nec_byte(editor->field, steps);
        refresh_tx_value_labels();
        refresh_ir_status();
    }
}

void auto_increment_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    s_ir_page.auto_increment = !s_ir_page.auto_increment;
    update_auto_increment_button();
    set_result_text(s_ir_page.auto_increment ?
                    "Auto+1 ON: command advances after Send." :
                    "Auto+1 OFF: command stays fixed.",
                    s_ir_page.auto_increment ? COLOR_PRIMARY : COLOR_MUTED);
    refresh_ir_status();
}

void repeat_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_ir_page.tx_repeat = !s_ir_page.tx_repeat;
    update_repeat_button();
}

void clear_rx_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    portENTER_CRITICAL(&s_ir_rx_mux);
    s_ir_rx_cache = {};
    portEXIT_CRITICAL(&s_ir_rx_mux);

    set_label_text(s_ir_page.rx_count_label, "Frames:0");
    set_rx_empty_fields();
    refresh_ir_status();
}

void ir_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    if (s_ir_page.timer) {
        lv_timer_delete(s_ir_page.timer);
        s_ir_page.timer = nullptr;
    }

    s_ir_page.page_alive = false;
    s_ir_page.status_label = nullptr;
    s_ir_page.result_label = nullptr;
    s_ir_page.send_button = nullptr;
    s_ir_page.accum_button = nullptr;
    s_ir_page.repeat_button = nullptr;
    s_ir_page.rx_count_label = nullptr;
    s_ir_page.rx_format_label = nullptr;
    s_ir_page.rx_addr_label = nullptr;
    s_ir_page.rx_cmd_label = nullptr;
    s_ir_page.rx_repeat_label = nullptr;
    s_ir_page.rx_raw_label = nullptr;
    s_ir_page.rx_clear_button = nullptr;
    for (auto& editor : s_ir_page.byte_editors) {
        editor = {};
    }
}

void create_byte_editor(lv_obj_t* parent,
                        NecByteEditor& editor,
                        NecByteField field,
                        const char* title,
                        int32_t x)
{
    editor.field = field;

    lv_obj_t* tile = lv_obj_create(parent);
    editor.tile = tile;
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(tile, x, TILE_Y);
    lv_obj_set_size(tile, TILE_W, TILE_H);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_TILE_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(tile, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(tile, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);

    editor.name_label = create_abs_label(tile, title, 6, 9, TILE_W - 12, 18,
                                         &lv_font_montserrat_14, COLOR_MUTED,
                                         LV_TEXT_ALIGN_CENTER);
    create_abs_label(tile, "+", 6, 31, TILE_W - 12, 18,
                     &lv_font_montserrat_16, COLOR_TX_ACCENT,
                     LV_TEXT_ALIGN_CENTER);
    editor.value_label = create_abs_label(tile, "00", 6, 56, TILE_W - 12, 32,
                                          &lv_font_montserrat_24, COLOR_TEXT,
                                          LV_TEXT_ALIGN_CENTER);
    create_abs_label(tile, "-", 6, 94, TILE_W - 12, 18,
                     &lv_font_montserrat_16, COLOR_MUTED,
                     LV_TEXT_ALIGN_CENTER);

    lv_obj_add_event_cb(tile, byte_drag_event_cb, LV_EVENT_PRESSED, &editor);
    lv_obj_add_event_cb(tile, byte_drag_event_cb, LV_EVENT_PRESSING, &editor);
    lv_obj_add_event_cb(tile, byte_drag_event_cb, LV_EVENT_RELEASED, &editor);
    lv_obj_add_event_cb(tile, byte_drag_event_cb, LV_EVENT_PRESS_LOST, &editor);
}

void build_tx_panel(lv_obj_t* parent)
{
    lv_obj_t* tx_panel = create_panel(parent, TX_PANEL_H, COLOR_PANEL_TX_BG);

    create_abs_label(tx_panel, "TX - NEC Encode", 10, 8, 178, 22,
                     &lv_font_montserrat_16, COLOR_TEXT);
    s_ir_page.status_label = create_abs_label(tx_panel, "Init:NO  L3B:--  RX:--",
                                              188, 8, 232, 22,
                                              &lv_font_montserrat_14,
                                              COLOR_MUTED,
                                              LV_TEXT_ALIGN_RIGHT);
    s_ir_page.result_label = create_abs_label(tx_panel,
                                              "Auto initializing IR TX/RX...",
                                              10, 34, 410, 22,
                                              &lv_font_montserrat_14,
                                              COLOR_WARNING);

    const int32_t first_x = 10;
    create_byte_editor(tx_panel, s_ir_page.byte_editors[0],
                       NecByteField::ADDR_HI, "ADDR H", first_x);
    create_byte_editor(tx_panel, s_ir_page.byte_editors[1],
                       NecByteField::ADDR_LO, "ADDR L",
                       first_x + (TILE_W + TILE_GAP));
    create_byte_editor(tx_panel, s_ir_page.byte_editors[2],
                       NecByteField::CMD_HI, "CMD H",
                       first_x + (TILE_W + TILE_GAP) * 2);
    create_byte_editor(tx_panel, s_ir_page.byte_editors[3],
                       NecByteField::CMD_LO, "CMD L",
                       first_x + (TILE_W + TILE_GAP) * 3);

    s_ir_page.accum_button = create_abs_button(tx_panel, "Auto+1",
                                               10, SEND_Y, 100, SEND_H,
                                               COLOR_BUTTON_BG, COLOR_TEXT);
    s_ir_page.repeat_button = create_abs_button(tx_panel, "Repeat x1",
                                                120, SEND_Y, 100, SEND_H,
                                                COLOR_BUTTON_BG, COLOR_TEXT);
    s_ir_page.send_button = create_abs_button(tx_panel, "Send NEC",
                                              242, SEND_Y, 178, SEND_H,
                                              COLOR_TX_ACCENT, 0xFFFFFF);

    lv_obj_add_event_cb(s_ir_page.accum_button, auto_increment_event_cb,
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_ir_page.repeat_button, repeat_event_cb,
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_ir_page.send_button, ir_send_event_cb,
                        LV_EVENT_CLICKED, nullptr);

    refresh_tx_value_labels();
}

void build_rx_panel(lv_obj_t* parent)
{
    lv_obj_t* rx_panel = create_panel(parent, RX_PANEL_H, COLOR_PANEL_RX_BG);

    create_abs_label(rx_panel, "RX - NEC Decode", 10, 8, 180, 22,
                     &lv_font_montserrat_16, COLOR_TEXT);
    s_ir_page.rx_count_label = create_abs_label(rx_panel, "Frames:0",
                                                210, 10, 100, 20,
                                                &lv_font_montserrat_14,
                                                COLOR_RX_ACCENT,
                                                LV_TEXT_ALIGN_RIGHT);
    s_ir_page.rx_clear_button = create_abs_button(rx_panel, "Clear",
                                                  324, 8, 88, 28,
                                                  COLOR_BUTTON_BG, COLOR_TEXT,
                                                  &lv_font_montserrat_14);

    s_ir_page.rx_format_label = create_abs_label(rx_panel, "Protocol: --",
                                                 10, 42, 150, 18,
                                                 &lv_font_montserrat_14,
                                                 COLOR_MUTED);
    s_ir_page.rx_repeat_label = create_abs_label(rx_panel, "Repeat: --",
                                                 250, 42, 162, 18,
                                                 &lv_font_montserrat_14,
                                                 COLOR_MUTED);
    s_ir_page.rx_addr_label = create_abs_label(rx_panel, "Address: --",
                                               10, 64, 186, 18,
                                               &lv_font_montserrat_14,
                                               COLOR_MUTED);
    s_ir_page.rx_cmd_label = create_abs_label(rx_panel, "Command: --",
                                              220, 64, 192, 18,
                                              &lv_font_montserrat_14,
                                              COLOR_MUTED);
    s_ir_page.rx_raw_label = create_abs_label(rx_panel, "Raw frame: --",
                                              10, 88, 402, 18,
                                              &lv_font_montserrat_14,
                                              COLOR_MUTED);

    lv_obj_add_event_cb(s_ir_page.rx_clear_button, clear_rx_event_cb,
                        LV_EVENT_CLICKED, nullptr);
}

void build_content(lv_obj_t* parent, const CoreP4FunctionWindowConfig& /*config*/)
{
    prepare_content(parent);
    lv_obj_set_style_pad_row(parent, 8, LV_PART_MAIN);
    s_ir_page.page_alive = true;
    s_ir_page.ir_init_done = false;
    s_ir_page.init_task_running = false;
    s_ir_page.tx_task_running = false;
    lv_obj_add_event_cb(parent, ir_content_delete_event_cb, LV_EVENT_DELETE, nullptr);

    build_tx_panel(parent);
    build_rx_panel(parent);

    auto* tool = corep4_ui_sync_get_tool();
    if (tool && tool->otool_tools.ir.is_initialized()) {
        s_ir_page.ir_init_done = true;
        if (!s_ir_callback_registered) {
            tool->otool_tools.ir.subscribe(ir_format_t::NEC, ir_rx_on_decoded);
            s_ir_callback_registered = true;
        }
        tool->otool_tools.ir.start_receive();
        set_result_text("IR TX/RX is ready. NEC RX active.", COLOR_SUCCESS);
    } else {
        start_ir_init_task("Auto initializing IR TX/RX...");
    }

    refresh_ir_status();
    s_ir_page.timer = lv_timer_create(ir_timer_cb, RX_REFRESH_MS, nullptr);
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "IR TX/RX",
    "click on ui_func1irtxrx1con",
    build_content,
    "Reinit",
    ir_init_event_cb,
    nullptr,
    COLOR_TX_ACCENT,
    0xFFFFFF,
};

} // namespace

extern "C" void func2irtxrx2clicked2func(lv_event_t* e)
{
    corep4_ui_open_function_window(e, WINDOW_CONFIG);
}
