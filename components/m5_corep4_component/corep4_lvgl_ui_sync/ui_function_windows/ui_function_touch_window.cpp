#include "ui_function_widgets.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_touch_cst3530_update.h"
#include "esp_lcd_touch_cst3xx.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#define cst3530_fw_bin cst3530_ui_update_fw_v111_bin
#include "third_party_component/esp_lcd_touch_cst3xx/cst3530_update/CST3530_fw_v1.1.1.h"
#undef cst3530_fw_bin
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_TOUCH";
constexpr int CANVAS_WIDTH = 480;
constexpr int CANVAS_HEIGHT = 480;
constexpr int CANVAS_GRID_DIVS = 5;
constexpr int CANVAS_TOTAL_CELLS = CANVAS_GRID_DIVS * CANVAS_GRID_DIVS;
constexpr uint32_t CANVAS_BG_COLOR = 0xEEF2F7;
constexpr uint32_t CANVAS_CELL_COLOR = 0xFFFFFF;
constexpr uint32_t CANVAS_GRID_COLOR = 0xCBD5E1;
constexpr uint32_t CANVAS_VISITED_COLOR = 0x1E293B;
constexpr uint32_t CANVAS_EXIT_BG = 0xD32F2F;
constexpr uint32_t CANVAS_EXIT_DISABLED_BG = 0x94A3B8;
constexpr uint32_t CANVAS_IDLE_TIMEOUT_MS = 10000;
constexpr uint32_t CANVAS_IDLE_CHECK_MS = 500;
constexpr int CANVAS_GRID_WIDTH = 3;
constexpr int CANVAS_CELL_INSET = CANVAS_GRID_WIDTH;
constexpr int CANVAS_CELL_RADIUS = 8;
constexpr int CANVAS_TRACK_RADIUS = 1;
constexpr int CANVAS_PROGRESS_LABEL_X = 10;
constexpr int CANVAS_PROGRESS_LABEL_Y = 10;
constexpr int CANVAS_PROGRESS_LABEL_W = 72;
constexpr int CANVAS_PROGRESS_LABEL_H = 28;
constexpr int CANVAS_ACTION_BUTTON_GAP = 8;
constexpr int CANVAS_CLEAN_BUTTON_W = 84;
constexpr int CANVAS_EXIT_BUTTON_W = 76;
constexpr int CANVAS_EXIT_BUTTON_H = 36;
constexpr size_t TOUCH_UPGRADE_LOG_SIZE = 4096;
constexpr uint8_t CST3530_UPDATE_RST_PIN = Pin_PYB_TP_RST;
constexpr uint16_t CST3530_UPDATE_RST_MASK = static_cast<uint16_t>(1U << CST3530_UPDATE_RST_PIN);
constexpr size_t CST3530_FW_INFO_OFFSET = 35U * 1024U;
constexpr size_t CST3530_FW_VERSION_OFFSET = CST3530_FW_INFO_OFFSET + 32U;

static_assert(CST3530_UPDATE_RST_PIN == M5IOE1_PIN_8, "CoreP4 v0.7 touch reset must be PYB IO8");
static_assert(CST3530_UPDATE_RST_PIN == 7, "M5IOE1_PIN_8 must map to GPIO_OUT_L bit7");

struct DirtyRect {
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
    bool valid = false;

    void reset() { valid = false; }
    void add_point(int32_t x, int32_t y)
    {
        if (!valid) {
            x1 = x2 = x;
            y1 = y2 = y;
            valid = true;
            return;
        }
        if (x < x1) x1 = x;
        if (x > x2) x2 = x;
        if (y < y1) y1 = y;
        if (y > y2) y2 = y;
    }
    void add_rect(int32_t ax1, int32_t ay1, int32_t ax2, int32_t ay2)
    {
        add_point(ax1, ay1);
        add_point(ax2, ay2);
    }
};

DirtyRect s_canvas_dirty;

inline uint16_t to_rgb565_raw(uint32_t hex)
{
    lv_color16_t c;
    c.red   = static_cast<uint16_t>(((hex >> 16) & 0xFF) >> 3);
    c.green = static_cast<uint16_t>(((hex >> 8) & 0xFF) >> 2);
    c.blue  = static_cast<uint16_t>((hex & 0xFF) >> 3);
    uint16_t raw;
    __builtin_memcpy(&raw, &c, sizeof(raw));
    return raw;
}

inline void canvas_put_px_raw(uint8_t* base, uint32_t stride, int32_t x, int32_t y, uint16_t raw)
{
    *reinterpret_cast<uint16_t*>(base + static_cast<uint32_t>(y) * stride + static_cast<uint32_t>(x) * 2u) = raw;
}

inline uint16_t canvas_get_px_raw(uint8_t* base, uint32_t stride, int32_t x, int32_t y)
{
    return *reinterpret_cast<uint16_t*>(base + static_cast<uint32_t>(y) * stride + static_cast<uint32_t>(x) * 2u);
}

struct TouchPageState {
    lv_obj_t* status_label = nullptr;
    lv_obj_t* indicator_button = nullptr;
    lv_obj_t* upgrade_card = nullptr;
    lv_obj_t* upgrade_button = nullptr;
    lv_obj_t* upgrade_hint_label = nullptr;
    lv_obj_t* upgrade_popup = nullptr;
    lv_obj_t* upgrade_progress_bar = nullptr;
    lv_obj_t* upgrade_progress_label = nullptr;
    lv_obj_t* upgrade_log_box = nullptr;
    lv_obj_t* upgrade_log_label = nullptr;
    lv_obj_t* upgrade_close_button = nullptr;
    cst3xx_info_t info = {};
    esp_err_t info_ret = ESP_FAIL;
    TaskHandle_t upgrade_task = nullptr;
    char upgrade_log[TOUCH_UPGRADE_LOG_SIZE] = {};
    bool info_loaded = false;
    bool page_alive = false;
    bool upgrade_running = false;
    lv_obj_t* window_backdrop = nullptr;
};

struct CanvasPageState {
    lv_obj_t* overlay = nullptr;
    lv_obj_t* canvas = nullptr;
    lv_obj_t* progress_label = nullptr;
    lv_obj_t* clean_button = nullptr;
    lv_obj_t* exit_button = nullptr;
    lv_obj_t* info_button = nullptr;
    lv_timer_t* idle_timer = nullptr;
    void* buffer = nullptr;
    lv_point_t last_point = {-1, -1};
    uint32_t last_activity_ms = 0;
    uint16_t current_track_raw = 0;
    bool visited[CANVAS_TOTAL_CELLS] = {};
    uint8_t visited_count = 0;
    bool is_drawing = false;
    bool closing = false;
};

TouchPageState s_touch_page;
CanvasPageState s_canvas_page;

size_t canvas_buffer_size(int32_t width, int32_t height, lv_color_format_t cf)
{
    return static_cast<size_t>(lv_draw_buf_width_to_stride(width, cf)) *
           static_cast<size_t>(height);
}

inline uint8_t* canvas_buf_data()
{
    if (!s_canvas_page.canvas) return nullptr;
    lv_draw_buf_t* db = lv_canvas_get_draw_buf(s_canvas_page.canvas);
    return db ? db->data : nullptr;
}

inline uint32_t canvas_buf_stride()
{
    if (!s_canvas_page.canvas) return 0;
    lv_draw_buf_t* db = lv_canvas_get_draw_buf(s_canvas_page.canvas);
    return db ? db->header.stride : 0;
}

uint32_t random_canvas_track_color()
{
    static constexpr uint32_t palette[] = {
        0xEF476F, 0xFFD166, 0x06D6A0, 0x118AB2,
        0x9B5DE5, 0xF15BB5, 0x00BBF9, 0xF78C6B,
        0x7DD87D, 0xFF8FAB,
    };
    return palette[esp_random() % (sizeof(palette) / sizeof(palette[0]))];
}

int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

bool canvas_rounded_rect_contains(int32_t x,
                                  int32_t y,
                                  int32_t x1,
                                  int32_t y1,
                                  int32_t x2,
                                  int32_t y2,
                                  int32_t radius)
{
    if (x < x1 || x > x2 || y < y1 || y > y2) {
        return false;
    }

    const int32_t width = x2 - x1 + 1;
    const int32_t height = y2 - y1 + 1;
    int32_t r = radius;
    const int32_t max_r = (width < height ? width : height) / 2;
    if (r > max_r) {
        r = max_r;
    }
    if (r <= 0) {
        return true;
    }

    const int32_t nearest_x = clamp_int(x, x1 + r, x2 - r);
    const int32_t nearest_y = clamp_int(y, y1 + r, y2 - r);
    const int32_t dx = x - nearest_x;
    const int32_t dy = y - nearest_y;
    return dx * dx + dy * dy <= r * r;
}

void canvas_fill_rounded_rect_raw(uint8_t* base,
                                  uint32_t stride,
                                  int32_t x1,
                                  int32_t y1,
                                  int32_t x2,
                                  int32_t y2,
                                  int32_t radius,
                                  uint16_t raw)
{
    if (!base || stride == 0) {
        return;
    }

    x1 = clamp_int(x1, 0, CANVAS_WIDTH - 1);
    y1 = clamp_int(y1, 0, CANVAS_HEIGHT - 1);
    x2 = clamp_int(x2, 0, CANVAS_WIDTH - 1);
    y2 = clamp_int(y2, 0, CANVAS_HEIGHT - 1);
    if (x1 > x2 || y1 > y2) {
        return;
    }

    for (int32_t yy = y1; yy <= y2; ++yy) {
        for (int32_t xx = x1; xx <= x2; ++xx) {
            if (canvas_rounded_rect_contains(xx, yy, x1, y1, x2, y2, radius)) {
                canvas_put_px_raw(base, stride, xx, yy, raw);
            }
        }
    }
}

uint16_t rgb565_raw_luma(uint16_t raw)
{
    lv_color16_t c;
    __builtin_memcpy(&c, &raw, sizeof(c));
    const uint16_t r = static_cast<uint16_t>((c.red << 3) | (c.red >> 2));
    const uint16_t g = static_cast<uint16_t>((c.green << 2) | (c.green >> 4));
    const uint16_t b = static_cast<uint16_t>((c.blue << 3) | (c.blue >> 2));
    return static_cast<uint16_t>((r * 77U + g * 150U + b * 29U) >> 8);
}

void set_manual_fullscreen(lv_obj_t* obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void format_fw_version(uint32_t fw_version, char* out, size_t out_size)
{
    const unsigned major = (fw_version >> 24) & 0xFFU;
    const unsigned minor = (fw_version >> 16) & 0xFFU;
    const unsigned patch = fw_version & 0xFFFFU;

    std::snprintf(out, out_size, "v%u.%u.%u", major, minor, patch);
}

uint32_t make_fw_u32(uint8_t b3, uint8_t b2, uint8_t b1, uint8_t b0)
{
    return (static_cast<uint32_t>(b3) << 24) |
           (static_cast<uint32_t>(b2) << 16) |
           (static_cast<uint32_t>(b1) << 8) |
           static_cast<uint32_t>(b0);
}

uint32_t target_fw_version()
{
    if (sizeof(cst3530_ui_update_fw_v111_bin) <= CST3530_FW_VERSION_OFFSET + 3U) {
        return 0;
    }

    return make_fw_u32(cst3530_ui_update_fw_v111_bin[CST3530_FW_VERSION_OFFSET + 3U],
                       cst3530_ui_update_fw_v111_bin[CST3530_FW_VERSION_OFFSET + 2U],
                       cst3530_ui_update_fw_v111_bin[CST3530_FW_VERSION_OFFSET + 1U],
                       cst3530_ui_update_fw_v111_bin[CST3530_FW_VERSION_OFFSET + 0U]);
}

int compare_fw_version(uint32_t lhs, uint32_t rhs)
{
    const uint32_t lhs_key = ((lhs >> 16) << 16) | (lhs & 0xFFFFU);
    const uint32_t rhs_key = ((rhs >> 16) << 16) | (rhs & 0xFFFFU);
    if (lhs_key < rhs_key) return -1;
    if (lhs_key > rhs_key) return 1;
    return 0;
}

void refresh_indicator_button()
{
    auto* tool = corep4_ui_sync_get_tool();
    const bool enabled = tool && tool->get_lvgl_touch_indicator_enabled();
    set_button_text(s_touch_page.indicator_button, enabled ? "Indicator ON" : "Indicator OFF");
    set_button_colors(s_touch_page.indicator_button,
                      enabled ? COLOR_SUCCESS : COLOR_BUTTON_BG,
                      enabled ? 0xFFFFFF : COLOR_TEXT);
}

bool touch_upgrade_required()
{
    const uint32_t target_version = target_fw_version();
    return s_touch_page.info_loaded &&
           s_touch_page.info_ret == ESP_OK &&
           s_touch_page.info.chip_id == CST3530_UPDATE_MATCH_CHIP_ID &&
           target_version != 0 &&
           compare_fw_version(s_touch_page.info.fw_version, target_version) < 0;
}

void refresh_upgrade_container_visibility()
{
    if (!s_touch_page.upgrade_card) {
        return;
    }

    const bool visible = touch_upgrade_required();
    if (visible) {
        lv_obj_remove_flag(s_touch_page.upgrade_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_touch_page.upgrade_card, LV_OBJ_FLAG_HIDDEN);
    }

    set_button_enabled(s_touch_page.upgrade_button, visible && !s_touch_page.upgrade_running);
    set_button_text(s_touch_page.upgrade_button, s_touch_page.upgrade_running ? "Updating" : "Upgrade");

    if (s_touch_page.upgrade_hint_label) {
        char current_fw_text[32];
        char target_fw_text[32];
        format_fw_version(s_touch_page.info.fw_version, current_fw_text, sizeof(current_fw_text));
        format_fw_version(target_fw_version(), target_fw_text, sizeof(target_fw_text));

        char text[128];
        std::snprintf(text, sizeof(text),
                      "Needs update: %s -> %s, chip 0x%08lX",
                      current_fw_text,
                      target_fw_text,
                      static_cast<unsigned long>(s_touch_page.info.chip_id));
        set_label_text(s_touch_page.upgrade_hint_label, visible ? text : "");
    }
}

void read_touch_info_once()
{
    if (s_touch_page.info_loaded) {
        return;
    }

    auto* tool = corep4_ui_sync_get_tool();
    auto handle = tool ? tool->get_touch_handle() : nullptr;
    if (!handle) {
        s_touch_page.info_ret = ESP_ERR_INVALID_STATE;
        s_touch_page.info_loaded = true;
        return;
    }

    s_touch_page.info_ret = esp_lcd_touch_cst3xx_read_info(handle, &s_touch_page.info);
    s_touch_page.info_loaded = true;
}

void refresh_touch_status()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    auto handle = tool ? tool->get_touch_handle() : nullptr;

    if (!tool || !handle) {
        s_touch_page.info = {};
        s_touch_page.info_ret = ESP_ERR_INVALID_STATE;
        s_touch_page.info_loaded = true;
        set_label_text(s_touch_page.status_label,
                       "Panel: CST3XX --\nChip ID: --\nFW Version: --\nHandle: --");
        refresh_indicator_button();
        refresh_upgrade_container_visibility();
        return;
    }

    read_touch_info_once();
    if (s_touch_page.info_ret != ESP_OK) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Panel: CST3XX %s\nChip ID: read failed\nFW Version: read failed\nError: %s",
                      status && status->touch.touch_initialized ? "OK" : "--",
                      esp_err_to_name(s_touch_page.info_ret));
        set_label_text(s_touch_page.status_label, buf);
        refresh_indicator_button();
        refresh_upgrade_container_visibility();
        return;
    }

    char fw_text[32];
    format_fw_version(s_touch_page.info.fw_version, fw_text, sizeof(fw_text));

    char buf[240];
    std::snprintf(buf, sizeof(buf),
                  "Panel: CST3XX %s  Handle:%s\nChip ID: 0x%08lX\nFW Version: %s (0x%08lX)\nResolution: %ux%u",
                  status && status->touch.touch_initialized ? "OK" : "--",
                  handle ? "ready" : "--",
                  static_cast<unsigned long>(s_touch_page.info.chip_id),
                  fw_text,
                  static_cast<unsigned long>(s_touch_page.info.fw_version),
                  static_cast<unsigned>(s_touch_page.info.resolution_x),
                  static_cast<unsigned>(s_touch_page.info.resolution_y));
    set_label_text(s_touch_page.status_label, buf);
    refresh_indicator_button();
    refresh_upgrade_container_visibility();
}

void reset_canvas_runtime()
{
    s_canvas_page.last_point.x = -1;
    s_canvas_page.last_point.y = -1;
    s_canvas_page.last_activity_ms = lv_tick_get();
    s_canvas_page.current_track_raw = to_rgb565_raw(random_canvas_track_color());
    s_canvas_page.visited_count = 0;
    s_canvas_page.is_drawing = false;
    for (bool& visited : s_canvas_page.visited) {
        visited = false;
    }
}

void reset_canvas_idle_activity()
{
    s_canvas_page.last_activity_ms = lv_tick_get();
}

void refresh_canvas_progress_label_color()
{
    if (!s_canvas_page.progress_label) {
        return;
    }

    uint8_t* base = canvas_buf_data();
    uint32_t stride = canvas_buf_stride();
    if (!base || stride == 0) {
        lv_obj_set_style_bg_color(s_canvas_page.progress_label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
        lv_obj_set_style_border_color(s_canvas_page.progress_label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_color(s_canvas_page.progress_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        return;
    }

    const int32_t x1 = CANVAS_PROGRESS_LABEL_X + 2;
    const int32_t x2 = CANVAS_PROGRESS_LABEL_X + CANVAS_PROGRESS_LABEL_W - 2;
    const int32_t y1 = CANVAS_PROGRESS_LABEL_Y + 2;
    const int32_t y2 = CANVAS_PROGRESS_LABEL_Y + CANVAS_PROGRESS_LABEL_H - 2;
    const int32_t xs[] = {
        clamp_int(x1, 0, CANVAS_WIDTH - 1),
        clamp_int((x1 + x2) / 2, 0, CANVAS_WIDTH - 1),
        clamp_int(x2, 0, CANVAS_WIDTH - 1),
    };
    const int32_t ys[] = {
        clamp_int(y1, 0, CANVAS_HEIGHT - 1),
        clamp_int((y1 + y2) / 2, 0, CANVAS_HEIGHT - 1),
        clamp_int(y2, 0, CANVAS_HEIGHT - 1),
    };

    uint32_t luma_sum = 0;
    uint32_t sample_count = 0;
    for (int32_t y : ys) {
        for (int32_t x : xs) {
            luma_sum += rgb565_raw_luma(canvas_get_px_raw(base, stride, x, y));
            sample_count++;
        }
    }

    const bool dark_bg = sample_count > 0 && (luma_sum / sample_count) < 128U;
    lv_obj_set_style_bg_color(s_canvas_page.progress_label,
                              lv_color_hex(dark_bg ? 0xFFFFFF : COLOR_TEXT),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(s_canvas_page.progress_label,
                                  lv_color_hex(dark_bg ? 0xFFFFFF : COLOR_TEXT),
                                  LV_PART_MAIN);
    lv_obj_set_style_text_color(s_canvas_page.progress_label,
                                lv_color_hex(dark_bg ? COLOR_TEXT : 0xFFFFFF),
                                LV_PART_MAIN);
}

void set_canvas_completion_actions_visible(bool visible)
{
    if (s_canvas_page.clean_button) {
        set_button_enabled(s_canvas_page.clean_button, visible);
        set_button_text(s_canvas_page.clean_button, "Clean");
        set_button_colors(s_canvas_page.clean_button, COLOR_PRIMARY, 0xFFFFFF);
        if (visible) {
            lv_obj_remove_flag(s_canvas_page.clean_button, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_canvas_page.clean_button, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_canvas_page.exit_button) {
        set_button_enabled(s_canvas_page.exit_button, visible);
        set_button_text(s_canvas_page.exit_button, "Exit");
        set_button_colors(s_canvas_page.exit_button,
                          visible ? CANVAS_EXIT_BG : CANVAS_EXIT_DISABLED_BG,
                          0xFFFFFF);
        if (visible) {
            lv_obj_remove_flag(s_canvas_page.exit_button, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_canvas_page.exit_button, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_canvas_page.info_button) {
        set_button_enabled(s_canvas_page.info_button, visible);
        set_button_text(s_canvas_page.info_button, "Info");
        set_button_colors(s_canvas_page.info_button, COLOR_PRIMARY, 0xFFFFFF);
        if (visible) {
            lv_obj_remove_flag(s_canvas_page.info_button, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_canvas_page.info_button, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void update_canvas_progress()
{
    char text[16];
    std::snprintf(text, sizeof(text), "%u/%u",
                  static_cast<unsigned>(s_canvas_page.visited_count),
                  static_cast<unsigned>(CANVAS_TOTAL_CELLS));
    set_label_text(s_canvas_page.progress_label, text);
    refresh_canvas_progress_label_color();
    set_canvas_completion_actions_visible(s_canvas_page.visited_count >= CANVAS_TOTAL_CELLS);
}

void close_canvas_page()
{
    if (!s_canvas_page.overlay || s_canvas_page.closing) {
        return;
    }

    s_canvas_page.closing = true;
    lv_obj_delete_async(s_canvas_page.overlay);
}

void canvas_idle_timer_cb(lv_timer_t* /*timer*/)
{
    if (!s_canvas_page.overlay || s_canvas_page.closing) {
        return;
    }

    if (lv_obj_has_flag(s_canvas_page.overlay, LV_OBJ_FLAG_HIDDEN)) {
        reset_canvas_idle_activity();
        return;
    }

    if (lv_tick_elaps(s_canvas_page.last_activity_ms) >= CANVAS_IDLE_TIMEOUT_MS) {
        if (s_touch_page.window_backdrop) {
            lv_obj_delete(s_touch_page.window_backdrop);
        } else {
            close_canvas_page();
        }
    }
}

void canvas_exit_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    if (s_canvas_page.visited_count >= CANVAS_TOTAL_CELLS) {
        if (s_touch_page.window_backdrop) {
            lv_obj_delete(s_touch_page.window_backdrop);
        } else {
            close_canvas_page();
        }
    }
}

void canvas_overlay_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE ||
        lv_event_get_target(e) != s_canvas_page.overlay) {
        return;
    }

    if (s_canvas_page.idle_timer) {
        lv_timer_delete(s_canvas_page.idle_timer);
        s_canvas_page.idle_timer = nullptr;
    }

    s_canvas_page = CanvasPageState{};
}

void canvas_buffer_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    void* buffer = lv_event_get_user_data(e);
    if (buffer) {
        heap_caps_free(buffer);
    }

    if (lv_event_get_target(e) == s_canvas_page.canvas) {
        s_canvas_page.canvas = nullptr;
        if (s_canvas_page.buffer == buffer) {
            s_canvas_page.buffer = nullptr;
        }
    }
}

void fill_canvas_cell(int row, int col)
{
    if (!s_canvas_page.canvas) {
        return;
    }
    uint8_t* base = canvas_buf_data();
    uint32_t stride = canvas_buf_stride();
    if (!base || stride == 0) {
        return;
    }

    const int32_t cell_w = CANVAS_WIDTH / CANVAS_GRID_DIVS;
    const int32_t cell_h = CANVAS_HEIGHT / CANVAS_GRID_DIVS;
    const int32_t x1 = col * cell_w + CANVAS_CELL_INSET;
    const int32_t y1 = row * cell_h + CANVAS_CELL_INSET;
    const int32_t x2 =
        col == CANVAS_GRID_DIVS - 1 ? CANVAS_WIDTH - CANVAS_CELL_INSET - 1 : (col + 1) * cell_w - 1;
    const int32_t y2 =
        row == CANVAS_GRID_DIVS - 1 ? CANVAS_HEIGHT - CANVAS_CELL_INSET - 1 : (row + 1) * cell_h - 1;
    if (x1 > x2 || y1 > y2) {
        return;
    }

    const uint16_t raw = to_rgb565_raw(CANVAS_VISITED_COLOR);
    canvas_fill_rounded_rect_raw(base, stride, x1, y1, x2, y2, CANVAS_CELL_RADIUS, raw);
    s_canvas_dirty.add_rect(x1, y1, x2, y2);
}

void mark_canvas_cell(int32_t x, int32_t y)
{
    const int32_t clamped_x = clamp_int(x, 0, CANVAS_WIDTH - 1);
    const int32_t clamped_y = clamp_int(y, 0, CANVAS_HEIGHT - 1);
    const int col = clamp_int((clamped_x * CANVAS_GRID_DIVS) / CANVAS_WIDTH, 0, CANVAS_GRID_DIVS - 1);
    const int row = clamp_int((clamped_y * CANVAS_GRID_DIVS) / CANVAS_HEIGHT, 0, CANVAS_GRID_DIVS - 1);
    const int index = row * CANVAS_GRID_DIVS + col;

    if (!s_canvas_page.visited[index]) {
        s_canvas_page.visited[index] = true;
        fill_canvas_cell(row, col);
        if (s_canvas_page.visited_count < CANVAS_TOTAL_CELLS) {
            s_canvas_page.visited_count++;
        }
        update_canvas_progress();
    }
}

void draw_canvas_brush_stamp(int32_t cx, int32_t cy, uint16_t raw)
{
    if (!s_canvas_page.canvas ||
        cx < 0 || cx >= CANVAS_WIDTH ||
        cy < 0 || cy >= CANVAS_HEIGHT) {
        return;
    }

    mark_canvas_cell(cx, cy);
    uint8_t* base = canvas_buf_data();
    uint32_t stride = canvas_buf_stride();
    if (!base || stride == 0) {
        return;
    }

    constexpr int32_t r = CANVAS_TRACK_RADIUS;
    for (int32_t dy = -r; dy <= r; ++dy) {
        for (int32_t dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy > r * r) {
                continue;
            }
            const int32_t x = cx + dx;
            const int32_t y = cy + dy;
            if (x < 0 || x >= CANVAS_WIDTH || y < 0 || y >= CANVAS_HEIGHT) {
                continue;
            }
            canvas_put_px_raw(base, stride, x, y, raw);
            s_canvas_dirty.add_point(x, y);
        }
    }
}

void draw_canvas_brush_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t raw)
{
    x0 = clamp_int(x0, 0, CANVAS_WIDTH - 1);
    y0 = clamp_int(y0, 0, CANVAS_HEIGHT - 1);
    x1 = clamp_int(x1, 0, CANVAS_WIDTH - 1);
    y1 = clamp_int(y1, 0, CANVAS_HEIGHT - 1);

    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy;

    while (true) {
        draw_canvas_brush_stamp(x0, y0, raw);
        if (x0 == x1 && y0 == y1) {
            break;
        }

        int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void canvas_touch_event_cb(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE) {
        lv_event_stop_bubbling(e);
        lv_event_stop_processing(e);
        return;
    }

    if (!s_canvas_page.canvas) {
        return;
    }

    if (code != LV_EVENT_PRESSED &&
        code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED &&
        code != LV_EVENT_PRESS_LOST) {
        return;
    }

    reset_canvas_idle_activity();

    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    point.x = clamp_int(point.x, 0, CANVAS_WIDTH - 1);
    point.y = clamp_int(point.y, 0, CANVAS_HEIGHT - 1);

    if (code == LV_EVENT_PRESSED) {
        s_canvas_dirty.reset();
        s_canvas_page.is_drawing = true;
        s_canvas_page.last_point = point;
        s_canvas_page.current_track_raw = to_rgb565_raw(random_canvas_track_color());
        draw_canvas_brush_stamp(point.x, point.y, s_canvas_page.current_track_raw);
    } else if (code == LV_EVENT_PRESSING) {
        s_canvas_dirty.reset();
        if (s_canvas_page.is_drawing &&
            s_canvas_page.last_point.x >= 0 &&
            s_canvas_page.last_point.y >= 0) {
            draw_canvas_brush_line(s_canvas_page.last_point.x,
                                   s_canvas_page.last_point.y,
                                   point.x,
                                   point.y,
                                   s_canvas_page.current_track_raw);
        } else {
            s_canvas_page.is_drawing = true;
            s_canvas_page.current_track_raw = to_rgb565_raw(random_canvas_track_color());
            draw_canvas_brush_stamp(point.x, point.y, s_canvas_page.current_track_raw);
        }
        s_canvas_page.last_point = point;
    } else {
        s_canvas_page.is_drawing = false;
        s_canvas_page.last_point.x = -1;
        s_canvas_page.last_point.y = -1;
    }

    refresh_canvas_progress_label_color();

    if (s_canvas_dirty.valid && s_canvas_page.canvas) {
        lv_area_t a;
        a.x1 = s_canvas_dirty.x1;
        a.y1 = s_canvas_dirty.y1;
        a.x2 = s_canvas_dirty.x2;
        a.y2 = s_canvas_dirty.y2;
        lv_obj_invalidate_area(s_canvas_page.canvas, &a);
        s_canvas_dirty.reset();
    }
}

void draw_canvas_grid_into_buffer()
{
    uint8_t* base = canvas_buf_data();
    uint32_t stride = canvas_buf_stride();
    if (!base || stride == 0) {
        return;
    }

    const uint16_t grid_raw = to_rgb565_raw(CANVAS_GRID_COLOR);
    const int32_t cell_w = CANVAS_WIDTH / CANVAS_GRID_DIVS;
    const int32_t cell_h = CANVAS_HEIGHT / CANVAS_GRID_DIVS;

    for (int i = 0; i <= CANVAS_GRID_DIVS; ++i) {
        const int32_t x = i == CANVAS_GRID_DIVS ? CANVAS_WIDTH - CANVAS_GRID_WIDTH : i * cell_w;
        const int32_t y = i == CANVAS_GRID_DIVS ? CANVAS_HEIGHT - CANVAS_GRID_WIDTH : i * cell_h;

        for (int32_t k = 0; k < CANVAS_GRID_WIDTH; ++k) {
            const int32_t line_y = y + k;
            const int32_t line_x = x + k;
            if (line_y >= 0 && line_y < CANVAS_HEIGHT) {
                for (int32_t xx = 0; xx < CANVAS_WIDTH; ++xx) {
                    canvas_put_px_raw(base, stride, xx, line_y, grid_raw);
                }
            }
            if (line_x >= 0 && line_x < CANVAS_WIDTH) {
                for (int32_t yy = 0; yy < CANVAS_HEIGHT; ++yy) {
                    canvas_put_px_raw(base, stride, line_x, yy, grid_raw);
                }
            }
        }
    }
}

void canvas_clean_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_event_stop_bubbling(e);

    if (!s_canvas_page.canvas) {
        return;
    }

    reset_canvas_runtime();
    s_canvas_dirty.reset();
    lv_canvas_fill_bg(s_canvas_page.canvas, lv_color_hex(CANVAS_CELL_COLOR), LV_OPA_COVER);
    draw_canvas_grid_into_buffer();
    update_canvas_progress();
    reset_canvas_idle_activity();
    lv_obj_invalidate(s_canvas_page.canvas);
}

lv_obj_t* create_canvas_label(lv_obj_t* parent)
{
    lv_obj_t* label = lv_label_create(parent);
    set_manual_fullscreen(label);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(label, CANVAS_PROGRESS_LABEL_X, CANVAS_PROGRESS_LABEL_Y);
    lv_obj_set_size(label, CANVAS_PROGRESS_LABEL_W, CANVAS_PROGRESS_LABEL_H);
    lv_obj_set_style_bg_color(label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(label, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_radius(label, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(label, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(label, 4, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(label, "0/25");
    return label;
}

void canvas_info_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_canvas_page.overlay) {
        lv_obj_add_flag(s_canvas_page.overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t* create_canvas_info_button(lv_obj_t* parent)
{
    lv_obj_t* button = add_button(parent, "Info", COLOR_PRIMARY, 0xFFFFFF, CANVAS_CLEAN_BUTTON_W, CANVAS_EXIT_BUTTON_H);
    set_manual_fullscreen(button);
    lv_obj_set_pos(button, 202, 8);
    lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(button, canvas_info_event_cb, LV_EVENT_CLICKED, nullptr);
    return button;
}

lv_obj_t* create_canvas_clean_button(lv_obj_t* parent)
{
    lv_obj_t* button = add_button(parent, "Clean", COLOR_PRIMARY, 0xFFFFFF,
                                  CANVAS_CLEAN_BUTTON_W, CANVAS_EXIT_BUTTON_H);
    set_manual_fullscreen(button);
    lv_obj_set_pos(button, 294, 8);
    lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(button, canvas_clean_event_cb, LV_EVENT_CLICKED, nullptr);
    return button;
}

lv_obj_t* create_canvas_exit_button(lv_obj_t* parent)
{
    lv_obj_t* button = add_button(parent, "Exit", CANVAS_EXIT_BG, 0xFFFFFF, CANVAS_CLEAN_BUTTON_W, CANVAS_EXIT_BUTTON_H);
    set_manual_fullscreen(button);
    lv_obj_set_pos(button, 386, 8);
    lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(button, canvas_exit_event_cb, LV_EVENT_CLICKED, nullptr);
    return button;
}

void open_canvas_page()
{
    if (s_canvas_page.overlay) {
        reset_canvas_idle_activity();
        lv_obj_remove_flag(s_canvas_page.overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_canvas_page.overlay);
        return;
    }

    reset_canvas_runtime();

    lv_obj_t* overlay = lv_obj_create(lv_layer_top());
    s_canvas_page.overlay = overlay;
    lv_obj_remove_style_all(overlay);
    set_manual_fullscreen(overlay);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, CANVAS_WIDTH, CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(CANVAS_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(overlay, canvas_touch_event_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(overlay, canvas_overlay_delete_event_cb, LV_EVENT_DELETE, nullptr);

    s_canvas_page.canvas = lv_canvas_create(overlay);
    set_manual_fullscreen(s_canvas_page.canvas);
    lv_obj_remove_flag(s_canvas_page.canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_canvas_page.canvas, 0, 0);
    lv_obj_set_size(s_canvas_page.canvas, CANVAS_WIDTH, CANVAS_HEIGHT);

    const lv_color_format_t cf = LV_COLOR_FORMAT_RGB565;
    const size_t buf_size = canvas_buffer_size(CANVAS_WIDTH, CANVAS_HEIGHT, cf);
    s_canvas_page.buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_canvas_page.buffer) {
        s_canvas_page.buffer = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
    }

    if (!s_canvas_page.buffer) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer, size=%u", static_cast<unsigned>(buf_size));
        close_canvas_page();
        return;
    }

    lv_canvas_set_buffer(s_canvas_page.canvas,
                         s_canvas_page.buffer,
                         CANVAS_WIDTH,
                         CANVAS_HEIGHT,
                         cf);
    lv_obj_add_event_cb(s_canvas_page.canvas,
                        canvas_buffer_delete_event_cb,
                        LV_EVENT_DELETE,
                        s_canvas_page.buffer);
    lv_canvas_fill_bg(s_canvas_page.canvas, lv_color_hex(CANVAS_CELL_COLOR), LV_OPA_COVER);

    draw_canvas_grid_into_buffer();
    s_canvas_page.progress_label = create_canvas_label(overlay);
    s_canvas_page.info_button = create_canvas_info_button(overlay);
    s_canvas_page.clean_button = create_canvas_clean_button(overlay);
    s_canvas_page.exit_button = create_canvas_exit_button(overlay);
    s_canvas_page.idle_timer = lv_timer_create(canvas_idle_timer_cb, CANVAS_IDLE_CHECK_MS, nullptr);
    update_canvas_progress();
    lv_obj_move_foreground(overlay);
}

void canvas_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    open_canvas_page();
}

void upgrade_popup_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE ||
        lv_event_get_target(e) != s_touch_page.upgrade_popup) {
        return;
    }

    s_touch_page.upgrade_popup = nullptr;
    s_touch_page.upgrade_progress_bar = nullptr;
    s_touch_page.upgrade_progress_label = nullptr;
    s_touch_page.upgrade_log_box = nullptr;
    s_touch_page.upgrade_log_label = nullptr;
    s_touch_page.upgrade_close_button = nullptr;
}

void upgrade_popup_close_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_touch_page.upgrade_running) {
        return;
    }

    if (s_touch_page.upgrade_popup) {
        lv_obj_delete(s_touch_page.upgrade_popup);
    }
}

void append_upgrade_log_locked(const char* line)
{
    if (!line || !line[0]) {
        return;
    }

    size_t len = std::strlen(s_touch_page.upgrade_log);
    const size_t line_len = std::strlen(line);
    if (len + line_len + 2 >= sizeof(s_touch_page.upgrade_log)) {
        const size_t keep = sizeof(s_touch_page.upgrade_log) / 2;
        const size_t old_len = len;
        const size_t start = old_len > keep ? old_len - keep : 0;
        std::memmove(s_touch_page.upgrade_log,
                     s_touch_page.upgrade_log + start,
                     old_len - start + 1);
        len = std::strlen(s_touch_page.upgrade_log);
    }

    std::snprintf(s_touch_page.upgrade_log + len,
                  sizeof(s_touch_page.upgrade_log) - len,
                  "%s\n",
                  line);
}

void update_upgrade_popup_locked(int progress, const char* line, bool finished, bool failed)
{
    append_upgrade_log_locked(line);

    if (s_touch_page.upgrade_progress_bar) {
        lv_bar_set_value(s_touch_page.upgrade_progress_bar, progress, LV_ANIM_OFF);
    }
    if (s_touch_page.upgrade_progress_label) {
        char text[64];
        std::snprintf(text, sizeof(text), "%d%%  %s",
                      progress,
                      failed ? "Failed" : (finished ? "Done" : "Updating"));
        set_label_text(s_touch_page.upgrade_progress_label, text);
        lv_obj_set_style_text_color(s_touch_page.upgrade_progress_label,
                                    lv_color_hex(failed ? COLOR_ERROR : (finished ? COLOR_SUCCESS : COLOR_TEXT)),
                                    LV_PART_MAIN);
    }
    if (s_touch_page.upgrade_log_label) {
        set_label_text(s_touch_page.upgrade_log_label, s_touch_page.upgrade_log);
    }
    if (s_touch_page.upgrade_log_box) {
        lv_obj_scroll_to_y(s_touch_page.upgrade_log_box, LV_COORD_MAX, LV_ANIM_OFF);
    }
    if (s_touch_page.upgrade_close_button) {
        if (finished || failed) {
            lv_obj_remove_flag(s_touch_page.upgrade_close_button, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_touch_page.upgrade_close_button, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void open_upgrade_popup()
{
    if (s_touch_page.upgrade_popup) {
        lv_obj_move_foreground(s_touch_page.upgrade_popup);
        return;
    }

    s_touch_page.upgrade_log[0] = '\0';

    lv_obj_t* overlay = lv_obj_create(lv_layer_top());
    s_touch_page.upgrade_popup = overlay;
    lv_obj_remove_style_all(overlay);
    set_manual_fullscreen(overlay);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, CANVAS_WIDTH, CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_event_cb(overlay, upgrade_popup_delete_event_cb, LV_EVENT_DELETE, nullptr);

    lv_obj_t* panel = lv_obj_create(overlay);
    lv_obj_set_size(panel, 420, 330);
    lv_obj_center(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* title = add_label(panel, "CST3530 Firmware Update", COLOR_TEXT);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);

    s_touch_page.upgrade_progress_label = add_label(panel, "0%  Waiting", COLOR_TEXT);

    s_touch_page.upgrade_progress_bar = lv_bar_create(panel);
    lv_obj_set_width(s_touch_page.upgrade_progress_bar, lv_pct(100));
    lv_obj_set_height(s_touch_page.upgrade_progress_bar, 14);
    lv_bar_set_range(s_touch_page.upgrade_progress_bar, 0, 100);
    lv_bar_set_value(s_touch_page.upgrade_progress_bar, 0, LV_ANIM_OFF);

    s_touch_page.upgrade_log_box = lv_obj_create(panel);
    lv_obj_set_width(s_touch_page.upgrade_log_box, lv_pct(100));
    lv_obj_set_height(s_touch_page.upgrade_log_box, 190);
    lv_obj_set_style_bg_color(s_touch_page.upgrade_log_box, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_touch_page.upgrade_log_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_touch_page.upgrade_log_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_touch_page.upgrade_log_box, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(s_touch_page.upgrade_log_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_touch_page.upgrade_log_box, 8, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_touch_page.upgrade_log_box, LV_SCROLLBAR_MODE_AUTO);

    s_touch_page.upgrade_log_label = lv_label_create(s_touch_page.upgrade_log_box);
    lv_obj_set_width(s_touch_page.upgrade_log_label, lv_pct(100));
    lv_label_set_long_mode(s_touch_page.upgrade_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_touch_page.upgrade_log_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_touch_page.upgrade_log_label, &lv_font_montserrat_16, LV_PART_MAIN);

    s_touch_page.upgrade_close_button = add_button(panel, "Close", COLOR_BUTTON_BG, COLOR_TEXT, 96, 34);
    lv_obj_align(s_touch_page.upgrade_close_button, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(s_touch_page.upgrade_close_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_touch_page.upgrade_close_button, upgrade_popup_close_event_cb, LV_EVENT_CLICKED, nullptr);

    update_upgrade_popup_locked(0, "Ready to update.", false, false);
}

void log_cst3530_reset_regs(m5_corep4_tool* tool, const char* phase)
{
    uint16_t mode = 0;
    uint16_t out = 0;
    uint16_t in = 0;
    uint16_t drv = 0;

    const bool ok = tool &&
                    tool->pyb.getModeReg(&mode) == M5IOE1_OK &&
                    tool->pyb.getOutputReg(&out) == M5IOE1_OK &&
                    tool->pyb.getInputReg(&in) == M5IOE1_OK &&
                    tool->pyb.getDriveReg(&drv) == M5IOE1_OK;
    if (!ok) {
        ESP_LOGW(TAG, "CST3530 reset %s: failed to read PYB GPIO registers", phase ? phase : "--");
        return;
    }

    ESP_LOGI(TAG,
             "CST3530 reset %s: IOE P8 enum=%u mask=0x%04X MODE=0x%04X OUT=0x%04X IN=0x%04X DRV=0x%04X bit=%u/%u/%u/%u",
             phase ? phase : "--",
             static_cast<unsigned>(CST3530_UPDATE_RST_PIN),
             static_cast<unsigned>(CST3530_UPDATE_RST_MASK),
             static_cast<unsigned>(mode),
             static_cast<unsigned>(out),
             static_cast<unsigned>(in),
             static_cast<unsigned>(drv),
             (mode & CST3530_UPDATE_RST_MASK) ? 1U : 0U,
             (out & CST3530_UPDATE_RST_MASK) ? 1U : 0U,
             (in & CST3530_UPDATE_RST_MASK) ? 1U : 0U,
             (drv & CST3530_UPDATE_RST_MASK) ? 1U : 0U);
}

esp_err_t cst3530_check_pyb_result(const char* op, m5ioe1_err_t err)
{
    if (err == M5IOE1_OK) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "CST3530 reset %s failed: M5IOE1 err=%d", op ? op : "operation", static_cast<int>(err));
    return ESP_FAIL;
}

esp_err_t cst3530_update_reset_cb(void* user_ctx)
{
    auto* tool = static_cast<m5_corep4_tool*>(user_ctx);
    if (!tool) {
        return ESP_ERR_INVALID_ARG;
    }

    m5ioe1_err_t pyb_ret = M5IOE1_OK;
    log_cst3530_reset_regs(tool, "before");

    tool->pyb.pinModeWithRes(CST3530_UPDATE_RST_PIN, OUTPUT, &pyb_ret);
    ESP_RETURN_ON_ERROR(cst3530_check_pyb_result("pinMode(P8 output)", pyb_ret), TAG, "PYB pinMode failed");

    pyb_ret = tool->pyb.setDriveMode(CST3530_UPDATE_RST_PIN, M5IOE1_DRIVE_OPENDRAIN);
    ESP_RETURN_ON_ERROR(cst3530_check_pyb_result("driveMode(P8 open-drain)", pyb_ret),
                        TAG,
                        "PYB drive mode failed");

    tool->pyb.digitalWriteWithRes(CST3530_UPDATE_RST_PIN, LOW, &pyb_ret);
    ESP_RETURN_ON_ERROR(cst3530_check_pyb_result("write P8 low", pyb_ret), TAG, "PYB write low failed");
    log_cst3530_reset_regs(tool, "low");

    vTaskDelay(pdMS_TO_TICKS(8));

    tool->pyb.digitalWriteWithRes(CST3530_UPDATE_RST_PIN, HIGH, &pyb_ret);
    ESP_RETURN_ON_ERROR(cst3530_check_pyb_result("write P8 high", pyb_ret), TAG, "PYB write high failed");
    log_cst3530_reset_regs(tool, "high");

    return ESP_OK;
}

void cst3530_update_status_cb(const cst3530_update_status_t* status, void* /*user_ctx*/)
{
    if (!status || !lvgl_port_lock(1000)) {
        return;
    }

    update_upgrade_popup_locked(status->progress,
                                status->message,
                                status->event == CST3530_UPDATE_EVENT_DONE,
                                status->event == CST3530_UPDATE_EVENT_ERROR);
    lvgl_port_unlock();
}

void set_lvgl_touch_input_enabled(bool enabled)
{
    auto* tool = corep4_ui_sync_get_tool();
    lv_indev_t* indev = tool ? tool->get_lvgl_touch_indev() : nullptr;
    if (!indev) {
        return;
    }

    if (lvgl_port_lock(1000)) {
        lv_indev_enable(indev, enabled);
        lvgl_port_unlock();
    }
}

void finish_upgrade_task_ui(esp_err_t ret)
{
    s_touch_page.upgrade_running = false;
    s_touch_page.upgrade_task = nullptr;

    if (!lvgl_port_lock(1000)) {
        return;
    }

    if (s_touch_page.page_alive) {
        s_touch_page.info_loaded = false;
        refresh_touch_status();
    }
    refresh_upgrade_container_visibility();

    update_upgrade_popup_locked(100,
                                ret == ESP_OK ? "Upgrade finished successfully." : esp_err_to_name(ret),
                                ret == ESP_OK,
                                ret != ESP_OK);
    lvgl_port_unlock();
}

void touch_upgrade_task(void* /*param*/)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    auto* tool = corep4_ui_sync_get_tool();

    if (!tool) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    set_lvgl_touch_input_enabled(false);

    {
        cst3530_update_config_t config = {};
        config.bus = tool->otool_tools.i2c_tools.get_i2c_master_bus_handle();
        config.i2c_speed_hz = 200000;
        config.reset_cb = cst3530_update_reset_cb;
        config.reset_user_ctx = tool;
        config.status_cb = cst3530_update_status_cb;
        ret = esp_lcd_touch_cst3530_update_firmware(&config,
                                                    cst3530_ui_update_fw_v111_bin,
                                                    sizeof(cst3530_ui_update_fw_v111_bin));
    }

    if (tool->get_touch_handle()) {
        (void)esp_lcd_touch_cst3xx_set_workmode(tool->get_touch_handle(), CST3XX_MODE_NORMAL);
    }

    set_lvgl_touch_input_enabled(true);

done:
    finish_upgrade_task_ui(ret);
    vTaskDelete(nullptr);
}

void upgrade_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED ||
        s_touch_page.upgrade_running ||
        !touch_upgrade_required()) {
        return;
    }

    open_upgrade_popup();
    s_touch_page.upgrade_running = true;
    refresh_upgrade_container_visibility();
    update_upgrade_popup_locked(0, "Starting firmware update task.", false, false);

    BaseType_t task_ret = xTaskCreate(touch_upgrade_task,
                                      "cst3530_update",
                                      8192,
                                      nullptr,
                                      5,
                                      &s_touch_page.upgrade_task);
    if (task_ret != pdPASS) {
        s_touch_page.upgrade_running = false;
        s_touch_page.upgrade_task = nullptr;
        refresh_upgrade_container_visibility();
        update_upgrade_popup_locked(0, "Failed to create update task.", false, true);
    }
}

void indicator_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        return;
    }
    const bool next_enabled = !tool->get_lvgl_touch_indicator_enabled();
    esp_err_t ret = tool->set_lvgl_touch_indicator_preference(next_enabled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save touch indicator preference: %s", esp_err_to_name(ret));
    }
    refresh_indicator_button();
}

void touch_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    if (s_canvas_page.overlay && !s_canvas_page.closing) {
        lv_obj_delete(s_canvas_page.overlay);
    }

    s_touch_page.page_alive = false;
    s_touch_page.status_label = nullptr;
    s_touch_page.indicator_button = nullptr;
    s_touch_page.upgrade_card = nullptr;
    s_touch_page.upgrade_button = nullptr;
    s_touch_page.upgrade_hint_label = nullptr;
    if (s_touch_page.upgrade_popup && !s_touch_page.upgrade_running) {
        lv_obj_delete(s_touch_page.upgrade_popup);
    }
    s_touch_page.info = {};
    s_touch_page.info_ret = ESP_FAIL;
    s_touch_page.info_loaded = false;
    s_touch_page.window_backdrop = nullptr;
}

void build_content(lv_obj_t* parent, const CoreP4FunctionWindowConfig& config)
{
    (void)config;

    s_touch_page.window_backdrop = lv_obj_get_parent(parent);

    prepare_content(parent);
    s_touch_page.page_alive = true;
    lv_obj_add_event_cb(parent, touch_content_delete_event_cb, LV_EVENT_DELETE, nullptr);

    lv_obj_t* status_card = add_card(parent, 120);
    add_label(status_card, "Touch Panel", COLOR_TEXT);
    s_touch_page.status_label = add_label(status_card, "Panel: CST3XX --\nChip ID: --\nFW Version: --", COLOR_MUTED);
    lv_label_set_long_mode(s_touch_page.status_label, LV_LABEL_LONG_WRAP);

    lv_obj_t* control_card = add_card(parent, 52);
    lv_obj_t* indicator_row = add_row(control_card, 34);
    add_label(indicator_row, "Touch indicator", COLOR_MUTED, 176);
    s_touch_page.indicator_button = add_button(indicator_row, "Indicator ON", COLOR_SUCCESS, 0xFFFFFF, 146, 32);
    lv_obj_add_event_cb(s_touch_page.indicator_button, indicator_button_event_cb, LV_EVENT_CLICKED, nullptr);

    s_touch_page.upgrade_card = add_card(parent, 92);
    lv_obj_add_flag(s_touch_page.upgrade_card, LV_OBJ_FLAG_HIDDEN);
    add_label(s_touch_page.upgrade_card, "CST3530 Firmware", COLOR_TEXT);
    lv_obj_t* upgrade_row = add_row(s_touch_page.upgrade_card, 36);
    s_touch_page.upgrade_button = add_button(upgrade_row, "Upgrade", COLOR_WARNING, 0xFFFFFF, 104, 32);
    s_touch_page.upgrade_hint_label = add_label(upgrade_row, "", COLOR_MUTED, 300);
    lv_obj_add_event_cb(s_touch_page.upgrade_button, upgrade_button_event_cb, LV_EVENT_CLICKED, nullptr);

    refresh_touch_status();
    open_canvas_page();
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "Touch",
    "click on ui_func1touch1con",
    build_content,
    "Canvas",
    canvas_button_event_cb,
    nullptr,
    COLOR_PRIMARY,
    0xFFFFFF,
};

} // namespace

extern "C" void func2touch2clicked2func(lv_event_t* e)
{
    corep4_ui_open_function_window(e, WINDOW_CONFIG);
}
