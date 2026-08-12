#include "ui_function_widgets.h"

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "uvc_app_task.h"
#include "m5_corep4_status_config.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_USB";

constexpr uint32_t COLOR_SURFACE = 0xFFFFFF;
constexpr uint32_t COLOR_CHIP_IDLE = 0xE5E7EB;
constexpr uint32_t COLOR_ACCENT_CAM = 0x0EA5E9;
constexpr uint32_t COLOR_ACCENT_MOUSE = 0x2563EB;
constexpr uint32_t COLOR_ACCENT_KEYBOARD = 0x7C3AED;
constexpr uint32_t COLOR_ACCENT_OTHER = 0x64748B;
constexpr uint32_t COLOR_MOUSE_AREA = 0xEEF6FF;
constexpr uint32_t COLOR_CAMERA_AREA = 0x050505;
constexpr uint32_t COLOR_MOUSE_STROKE_LEFT = 0x60A5FA;
constexpr uint32_t COLOR_MOUSE_STROKE_RIGHT = 0xEF4444;
constexpr uint32_t AUTO_CHECK_FIRST_DELAY_MS = 1000;
constexpr uint32_t AUTO_CHECK_INTERVAL_MS = 5000;
constexpr uint32_t USB_TEST_REFRESH_MS = 16;
constexpr uint32_t CAMERA_FRAME_INTERVAL_MS = 66;
constexpr uint32_t CAMERA_SELECT_TIMEOUT_MS = 5000;
constexpr uint32_t KEYBOARD_CURSOR_BLINK_MS = 500;
constexpr int CAMERA_DEVICE_INDEX = 0;
constexpr uint8_t CAMERA_FPS_WINDOW_SIZE = 20;
constexpr int16_t SUBPAGE_EXIT_FALLBACK_X = 388;
constexpr int16_t SUBPAGE_EXIT_FALLBACK_Y = 9;
constexpr int16_t SUBPAGE_EXIT_FALLBACK_W = 76;
constexpr int16_t SUBPAGE_EXIT_FALLBACK_H = 38;
constexpr int16_t USB_SUBPAGE_HEIGHT = 398;
constexpr int16_t MOUSE_BRUSH_RADIUS = 2;

enum class UsbAction : uint8_t {
    CHECK,
};

enum class UsbView : uint8_t {
    HUB,
    CAMERA_TEST,
    MOUSE_TEST,
    KEYBOARD_TEST,
};

enum class UsbDeviceOpen : uint8_t {
    CAMERA,
    MOUSE,
    KEYBOARD,
};

struct UsbPageState {
    lv_obj_t* content_root       = nullptr;
    lv_obj_t* cam_chip           = nullptr;
    lv_obj_t* cam_chip_label     = nullptr;
    lv_obj_t* hid_chip           = nullptr;
    lv_obj_t* hid_chip_label     = nullptr;
    lv_obj_t* other_chip         = nullptr;
    lv_obj_t* other_chip_label   = nullptr;
    lv_obj_t* cam_row            = nullptr;
    lv_obj_t* cam_title_label    = nullptr;
    lv_obj_t* cam_info_label     = nullptr;
    lv_obj_t* mouse_row          = nullptr;
    lv_obj_t* mouse_title_label  = nullptr;
    lv_obj_t* mouse_info_label   = nullptr;
    lv_obj_t* keyboard_row       = nullptr;
    lv_obj_t* keyboard_title_label = nullptr;
    lv_obj_t* keyboard_info_label  = nullptr;
    lv_obj_t* other_row          = nullptr;
    lv_obj_t* other_title_label  = nullptr;
    lv_obj_t* other_info_label   = nullptr;
    lv_obj_t* result_label       = nullptr;
    lv_obj_t* header_check_button = nullptr;
    lv_obj_t* header_exit_button = nullptr;
    lv_obj_t* header_subpage_exit_button = nullptr;
    lv_obj_t* mouse_area         = nullptr;
    lv_obj_t* mouse_canvas       = nullptr;
    void* mouse_canvas_buffer    = nullptr;
    int16_t mouse_canvas_w       = 0;
    int16_t mouse_canvas_h       = 0;
    lv_obj_t* mouse_xy_label     = nullptr;
    lv_obj_t* mouse_button_label = nullptr;
    lv_obj_t* keyboard_text_label = nullptr;
    lv_obj_t* keyboard_status_label = nullptr;
    lv_obj_t* camera_area       = nullptr;
    lv_obj_t* camera_image      = nullptr;
    lv_obj_t* camera_status_label = nullptr;
    lv_obj_t* camera_select_overlay = nullptr;
    lv_obj_t* camera_select_progress = nullptr;
    lv_timer_t* timer            = nullptr;
    uint32_t next_auto_check_ms   = 0;
    uint32_t next_status_refresh_ms = 0;
    volatile bool page_alive     = false;
    volatile bool task_running   = false;
    UsbView active_view          = UsbView::HUB;
    bool uvc_started             = false;
    bool hid_started             = false;
    bool mouse_support_destroy_pending = false;
    bool mouse_connected         = false;
    bool keyboard_connected      = false;
    int camera_count             = 0;
    int other_count              = 0;
    bool mouse_draw_active       = false;
    uint32_t mouse_draw_color    = 0;
    int16_t mouse_last_x         = 0;
    int16_t mouse_last_y         = 0;
    uint8_t keyboard_last_key    = 0;
    uint8_t keyboard_prev_keys[6] = {};
    char keyboard_text[512]      = {};
    size_t keyboard_text_len     = 0;
    bool keyboard_caps_lock      = false;
    bool keyboard_cursor_visible = true;
    uint32_t keyboard_cursor_next_toggle_ms = 0;
    uint8_t* camera_rgb_buffer   = nullptr;
    size_t camera_rgb_capacity   = 0;
    jpeg_decoder_handle_t camera_jpeg_decoder = nullptr;
    bool camera_streaming        = false;
    bool camera_select_countdown_active = false;
    int camera_resolution_index  = 0;
    int camera_pending_resolution_index = 0;
    int camera_frame_w           = 0;
    int camera_frame_h           = 0;
    int camera_canvas_w          = 0;
    int camera_canvas_h          = 0;
    uint32_t camera_frame_count  = 0;
    uint32_t camera_frame_ticks[CAMERA_FPS_WINDOW_SIZE] = {};
    uint8_t camera_frame_tick_count = 0;
    uint8_t camera_frame_tick_index = 0;
    uint32_t camera_next_frame_ms = 0;
    uint32_t camera_next_status_ms = 0;
    uint32_t camera_select_deadline_ms = 0;
};

struct MouseDirtyRect {
    int16_t x1 = 0;
    int16_t y1 = 0;
    int16_t x2 = 0;
    int16_t y2 = 0;
    bool valid = false;

    void reset()
    {
        x1 = 0;
        y1 = 0;
        x2 = 0;
        y2 = 0;
        valid = false;
    }

    void add_point(int16_t x, int16_t y)
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
};

UsbPageState s_usb_page;
MouseDirtyRect s_mouse_dirty;
bool s_uvc_host_installed = false;

void show_hub_page();
void show_camera_mode_select();
void close_camera_mode_select();
void update_camera_mode_select(uint32_t now_ms);
void open_camera_test_page(int resolution_index);
void refresh_usb_status();
void destroy_mouse_support_async_cb(void* user_data);

void set_visible(lv_obj_t* obj, bool visible)
{
    if (!obj) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

uint32_t usb_tick_ms()
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

bool time_reached(uint32_t now_ms, uint32_t due_ms)
{
    return static_cast<int32_t>(now_ms - due_ms) >= 0;
}

void set_result_text(const char* text, uint32_t color)
{
    if (!s_usb_page.result_label) {
        return;
    }
    set_label_text(s_usb_page.result_label, text);
    lv_obj_set_style_text_color(s_usb_page.result_label, lv_color_hex(color), LV_PART_MAIN);
    set_visible(s_usb_page.result_label, text && text[0] != '\0');
}

const char* get_button_label_text(lv_obj_t* button)
{
    if (!button || !lv_obj_check_type(button, &lv_button_class) ||
        lv_obj_get_child_count(button) == 0) {
        return nullptr;
    }

    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (!label || !lv_obj_check_type(label, &lv_label_class)) {
        return nullptr;
    }
    return lv_label_get_text(label);
}

void set_button_label_font(lv_obj_t* button, const lv_font_t* font)
{
    if (!button || !font || lv_obj_get_child_count(button) == 0) {
        return;
    }
    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (!label || !lv_obj_check_type(label, &lv_label_class)) {
        return;
    }
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_center(label);
}

void discover_header_buttons_in(lv_obj_t* root)
{
    if (!root || (s_usb_page.header_check_button && s_usb_page.header_exit_button)) {
        return;
    }

    const uint32_t child_count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t* child = lv_obj_get_child(root, i);
        const char* text = get_button_label_text(child);
        if (text) {
            if (std::strcmp(text, "Check") == 0) {
                s_usb_page.header_check_button = child;
            } else if (std::strcmp(text, "Exit") == 0) {
                s_usb_page.header_exit_button = child;
            }
        }
        discover_header_buttons_in(child);
    }
}

void usb_header_subpage_exit_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    show_hub_page();
}

void ensure_header_subpage_exit_button()
{
    if (s_usb_page.header_subpage_exit_button) {
        return;
    }

    int16_t x = SUBPAGE_EXIT_FALLBACK_X;
    int16_t y = SUBPAGE_EXIT_FALLBACK_Y;
    int16_t w = SUBPAGE_EXIT_FALLBACK_W;
    int16_t h = SUBPAGE_EXIT_FALLBACK_H;
    if (s_usb_page.header_exit_button) {
        lv_obj_t* header = lv_obj_get_parent(s_usb_page.header_exit_button);
        if (header) {
            x = lv_obj_get_x(header) + lv_obj_get_x(s_usb_page.header_exit_button);
            y = lv_obj_get_y(header) + lv_obj_get_y(s_usb_page.header_exit_button);
            w = lv_obj_get_width(s_usb_page.header_exit_button);
            h = lv_obj_get_height(s_usb_page.header_exit_button);
        }
    }

    lv_obj_t* button = lv_button_create(lv_layer_top());
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(button, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, h);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xDC2626), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(button, usb_header_subpage_exit_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Exit");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(label);

    s_usb_page.header_subpage_exit_button = button;
}

void destroy_header_subpage_exit_button()
{
    if (s_usb_page.header_subpage_exit_button) {
        lv_obj_delete_async(s_usb_page.header_subpage_exit_button);
        s_usb_page.header_subpage_exit_button = nullptr;
    }
}

void set_header_subpage_exit_mode(bool enabled)
{
    set_visible(s_usb_page.header_check_button, !enabled);
    set_visible(s_usb_page.header_exit_button, !enabled);
    if (enabled) {
        ensure_header_subpage_exit_button();
        set_visible(s_usb_page.header_subpage_exit_button, true);
        if (s_usb_page.header_subpage_exit_button) {
            lv_obj_move_foreground(s_usb_page.header_subpage_exit_button);
        }
    } else {
        destroy_header_subpage_exit_button();
    }
}

void discover_header_buttons()
{
    if (!s_usb_page.content_root) {
        return;
    }
    lv_obj_t* window = lv_obj_get_parent(s_usb_page.content_root);
    discover_header_buttons_in(window);
}

void cancel_pending_mouse_support_destroy()
{
    if (!s_usb_page.mouse_support_destroy_pending) {
        return;
    }

    lv_async_call_cancel(destroy_mouse_support_async_cb, nullptr);
    s_usb_page.mouse_support_destroy_pending = false;
}

void destroy_mouse_support_now()
{
    auto* tool = corep4_ui_sync_get_tool();
    if (tool) {
        tool->destroy_lvgl_mouse_support();
    }
}

void destroy_mouse_support_async_cb(void* /*user_data*/)
{
    s_usb_page.mouse_support_destroy_pending = false;
    destroy_mouse_support_now();
}

void schedule_mouse_support_destroy()
{
    if (s_usb_page.mouse_support_destroy_pending) {
        return;
    }

    s_usb_page.mouse_support_destroy_pending = true;
    if (lv_async_call(destroy_mouse_support_async_cb, nullptr) != LV_RESULT_OK) {
        s_usb_page.mouse_support_destroy_pending = false;
        ESP_LOGW(TAG, "Failed to schedule LVGL mouse support destroy");
    }
}

void style_chip(lv_obj_t* chip)
{
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(chip, 132, 42);
    lv_obj_set_style_border_width(chip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(chip, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(chip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);
}

lv_obj_t* create_chip(lv_obj_t* parent, const char* text, lv_obj_t** label_out)
{
    lv_obj_t* chip = lv_obj_create(parent);
    style_chip(chip);

    lv_obj_t* label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(label);

    if (label_out) {
        *label_out = label;
    }
    return chip;
}

void set_chip(lv_obj_t* chip, lv_obj_t* label, const char* text,
              uint32_t bg_color, uint32_t text_color)
{
    if (!chip || !label) {
        return;
    }
    lv_obj_set_style_bg_color(chip, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), LV_PART_MAIN);
    lv_obj_center(label);
}

void style_device_row(lv_obj_t* row, bool clickable)
{
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 64);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(clickable ? COLOR_PRIMARY : COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 7, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 7, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (clickable) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
}

lv_obj_t* create_badge(lv_obj_t* parent, const char* text, uint32_t color)
{
    lv_obj_t* badge = lv_obj_create(parent);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(badge, 54, 40);
    lv_obj_set_style_bg_color(badge, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(badge, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(badge, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(badge, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(badge, 0, LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(badge);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(label);
    return badge;
}

lv_obj_t* create_text_stack(lv_obj_t* parent, lv_obj_t** title_label, lv_obj_t** info_label)
{
    lv_obj_t* stack = lv_obj_create(parent);
    lv_obj_remove_flag(stack, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(stack, 48);
    lv_obj_set_width(stack, 160);
    lv_obj_set_flex_grow(stack, 1);
    lv_obj_set_style_bg_opa(stack, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(stack, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(stack, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(stack, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(stack, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stack, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    *title_label = add_label(stack, "", COLOR_TEXT);
    lv_label_set_long_mode(*title_label, LV_LABEL_LONG_DOT);
    *info_label = add_label(stack, "", COLOR_MUTED);
    lv_label_set_long_mode(*info_label, LV_LABEL_LONG_DOT);
    return stack;
}

void add_open_hint(lv_obj_t* parent)
{
    lv_obj_t* label = add_label(parent, "Open", COLOR_PRIMARY, 54);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

int query_camera_info(uvc_dev_info_t* out_info)
{
    int dev_num = 0;
    if (!s_usb_page.uvc_started || app_uvc_get_connect_dev_num(&dev_num) != ESP_OK) {
        return 0;
    }
    if (dev_num > 0 && out_info) {
        std::memset(out_info, 0, sizeof(*out_info));
        app_uvc_get_dev_frame_info(0, out_info);
    }
    return dev_num;
}

const char* camera_format_name(int format)
{
    switch (format) {
        case UVC_VS_FORMAT_MJPEG:
            return "MJPEG";
        case UVC_VS_FORMAT_YUY2:
            return "YUY2";
        case UVC_VS_FORMAT_H264:
            return "H264";
        case UVC_VS_FORMAT_H265:
            return "H265";
        default:
            return "UVC";
    }
}

int clamp_camera_resolution_index(const uvc_dev_info_t& info, int requested_index)
{
    if (info.resolution_count <= 0) {
        return 0;
    }
    if (requested_index < 0 || requested_index >= info.resolution_count) {
        return 0;
    }
    return requested_index;
}

void format_camera_mode(const uvc_dev_info_t& info,
                        int index,
                        char* title,
                        size_t title_len,
                        char* detail,
                        size_t detail_len)
{
    if (!title || title_len == 0 || !detail || detail_len == 0 ||
        index < 0 || index >= info.resolution_count) {
        return;
    }

    const auto& mode = info.resolution[index];
    std::snprintf(title,
                  title_len,
                  "%s %dx%d",
                  camera_format_name(mode.format),
                  mode.width,
                  mode.height);
    if (mode.fps > 0.05f) {
        std::snprintf(detail, detail_len, "%.1f fps", static_cast<double>(mode.fps));
    } else {
        std::snprintf(detail, detail_len, "-- fps");
    }
}

void set_camera_status(const char* text, uint32_t color = 0xFFFFFF)
{
    if (!s_usb_page.camera_status_label) {
        return;
    }
    set_label_text(s_usb_page.camera_status_label, text ? text : "");
    lv_obj_set_style_text_color(s_usb_page.camera_status_label, lv_color_hex(color), LV_PART_MAIN);
    set_visible(s_usb_page.camera_status_label, text && text[0] != '\0');
}

void close_camera_mode_select()
{
    if (s_usb_page.camera_select_overlay) {
        lv_obj_t* overlay = s_usb_page.camera_select_overlay;
        s_usb_page.camera_select_overlay = nullptr;
        s_usb_page.camera_select_progress = nullptr;
        s_usb_page.camera_select_deadline_ms = 0;
        s_usb_page.camera_select_countdown_active = false;
        lv_obj_delete(overlay);
    } else {
        s_usb_page.camera_select_progress = nullptr;
        s_usb_page.camera_select_deadline_ms = 0;
        s_usb_page.camera_select_countdown_active = false;
    }
}

void cancel_camera_mode_select_countdown()
{
    if (!s_usb_page.camera_select_countdown_active) {
        return;
    }
    s_usb_page.camera_select_countdown_active = false;
    s_usb_page.camera_select_deadline_ms = 0;
    if (s_usb_page.camera_select_progress) {
        lv_obj_add_flag(s_usb_page.camera_select_progress, LV_OBJ_FLAG_HIDDEN);
    }
}

void camera_mode_select_touch_event_cb(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING ||
        code == LV_EVENT_SCROLL_BEGIN) {
        cancel_camera_mode_select_countdown();
    }
}

void camera_mode_select_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const int resolution_index = static_cast<int>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    close_camera_mode_select();
    open_camera_test_page(resolution_index);
}

lv_obj_t* create_camera_mode_row(lv_obj_t* parent,
                                 const uvc_dev_info_t& info,
                                 int index)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 46);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(row,
                        camera_mode_select_event_cb,
                        LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(index)));
    lv_obj_add_event_cb(row,
                        camera_mode_select_touch_event_cb,
                        LV_EVENT_PRESSED,
                        nullptr);

    char title[48] = {};
    char detail[24] = {};
    format_camera_mode(info, index, title, sizeof(title), detail, sizeof(detail));

    lv_obj_t* title_label = add_label(row, title, COLOR_TEXT, 238);
    lv_obj_set_flex_grow(title_label, 1);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, LV_PART_MAIN);

    lv_obj_t* detail_label = add_label(row, detail, COLOR_MUTED, 84);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_16, LV_PART_MAIN);
    return row;
}

void show_camera_mode_select()
{
    if (!s_usb_page.content_root) {
        return;
    }

    close_camera_mode_select();

    uvc_dev_info_t info = {};
    const int camera_count = query_camera_info(&info);
    if (camera_count <= 0) {
        set_result_text("Camera disconnected.", COLOR_WARNING);
        refresh_usb_status();
        return;
    }
    if (info.resolution_count <= 0) {
        set_result_text("Only MJPEG UVC cameras are supported.", COLOR_WARNING);
        return;
    }

    s_usb_page.camera_pending_resolution_index = 0;
    s_usb_page.camera_select_countdown_active = true;
    const uint32_t now_ms = usb_tick_ms();
    s_usb_page.camera_select_deadline_ms = now_ms + CAMERA_SELECT_TIMEOUT_MS;

    lv_obj_t* overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(overlay,
                        camera_mode_select_touch_event_cb,
                        LV_EVENT_PRESSED,
                        nullptr);

    lv_obj_t* panel = lv_obj_create(overlay);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(panel, 408, 330);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(panel, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(panel);
    lv_obj_add_event_cb(panel,
                        camera_mode_select_touch_event_cb,
                        LV_EVENT_PRESSED,
                        nullptr);

    lv_obj_t* title_label = add_label(panel, "Camera Mode", COLOR_TEXT);
    lv_obj_set_width(title_label, lv_pct(100));
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, LV_PART_MAIN);

    lv_obj_t* list = lv_obj_create(panel);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_height(list, 246);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(list, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(list,
                        camera_mode_select_touch_event_cb,
                        LV_EVENT_PRESSED,
                        nullptr);
    lv_obj_add_event_cb(list,
                        camera_mode_select_touch_event_cb,
                        LV_EVENT_SCROLL_BEGIN,
                        nullptr);

    for (int i = 0; i < info.resolution_count; ++i) {
        create_camera_mode_row(list, info, i);
    }

    lv_obj_t* track = lv_obj_create(panel);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(track, lv_pct(100));
    lv_obj_set_height(track, 8);
    lv_obj_set_style_bg_color(track, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(track, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(track, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(track, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(track, 0, LV_PART_MAIN);

    lv_obj_t* fill = lv_obj_create(track);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(fill, lv_pct(100));
    lv_obj_set_height(fill, lv_pct(100));
    lv_obj_set_style_bg_color(fill, lv_color_hex(COLOR_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(fill, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(fill, 0, LV_PART_MAIN);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);

    s_usb_page.camera_select_overlay = overlay;
    s_usb_page.camera_select_progress = fill;
    lv_obj_move_foreground(overlay);
    update_camera_mode_select(now_ms);
}

void update_camera_mode_select(uint32_t now_ms)
{
    if (!s_usb_page.camera_select_overlay) {
        return;
    }

    if (!s_usb_page.camera_select_countdown_active) {
        return;
    }

    if (time_reached(now_ms, s_usb_page.camera_select_deadline_ms)) {
        const int resolution_index = s_usb_page.camera_pending_resolution_index;
        close_camera_mode_select();
        open_camera_test_page(resolution_index);
        return;
    }

    if (!s_usb_page.camera_select_progress) {
        return;
    }

    lv_obj_t* track = lv_obj_get_parent(s_usb_page.camera_select_progress);
    if (!track) {
        return;
    }
    lv_obj_update_layout(track);
    const int32_t track_w = lv_obj_get_width(track);
    if (track_w <= 0) {
        return;
    }

    const uint32_t remaining_ms = s_usb_page.camera_select_deadline_ms - now_ms;
    int32_t fill_w = static_cast<int32_t>(
        (static_cast<uint64_t>(track_w) * remaining_ms) / CAMERA_SELECT_TIMEOUT_MS);
    if (fill_w < 0) {
        fill_w = 0;
    } else if (fill_w > track_w) {
        fill_w = track_w;
    }
    lv_obj_set_width(s_usb_page.camera_select_progress, fill_w);
    lv_obj_align(s_usb_page.camera_select_progress, LV_ALIGN_LEFT_MID, 0, 0);
}

void free_camera_frame_buffer()
{
    if (s_usb_page.camera_image) {
        lv_image_set_src(s_usb_page.camera_image, nullptr);
    }
    if (s_usb_page.camera_jpeg_decoder) {
        jpeg_del_decoder_engine(s_usb_page.camera_jpeg_decoder);
        s_usb_page.camera_jpeg_decoder = nullptr;
    }
    if (s_usb_page.camera_rgb_buffer) {
        heap_caps_free(s_usb_page.camera_rgb_buffer);
        s_usb_page.camera_rgb_buffer = nullptr;
    }
    s_usb_page.camera_rgb_capacity = 0;
    s_usb_page.camera_frame_w = 0;
    s_usb_page.camera_frame_h = 0;
    s_usb_page.camera_canvas_w = 0;
    s_usb_page.camera_canvas_h = 0;
    s_usb_page.camera_frame_count = 0;
    std::memset(s_usb_page.camera_frame_ticks, 0, sizeof(s_usb_page.camera_frame_ticks));
    s_usb_page.camera_frame_tick_count = 0;
    s_usb_page.camera_frame_tick_index = 0;
}

void stop_camera_stream()
{
    if (s_usb_page.camera_streaming) {
        app_uvc_control_dev_by_index(CAMERA_DEVICE_INDEX, false, s_usb_page.camera_resolution_index);
        s_usb_page.camera_streaming = false;
    }
    s_usb_page.camera_next_frame_ms = 0;
    s_usb_page.camera_next_status_ms = 0;
}

uint32_t align_up_u32(uint32_t value, uint32_t alignment)
{
    if (alignment == 0) {
        return value;
    }
    return (value + alignment - 1u) & ~(alignment - 1u);
}

float update_camera_fps(uint32_t now_ms)
{
    s_usb_page.camera_frame_ticks[s_usb_page.camera_frame_tick_index] = now_ms;
    s_usb_page.camera_frame_tick_index =
        static_cast<uint8_t>((s_usb_page.camera_frame_tick_index + 1) % CAMERA_FPS_WINDOW_SIZE);
    if (s_usb_page.camera_frame_tick_count < CAMERA_FPS_WINDOW_SIZE) {
        ++s_usb_page.camera_frame_tick_count;
    }

    const uint8_t count = s_usb_page.camera_frame_tick_count;
    if (count < 2) {
        return 0.0f;
    }

    const uint8_t oldest_index =
        (count < CAMERA_FPS_WINDOW_SIZE)
            ? 0
            : s_usb_page.camera_frame_tick_index;
    const uint8_t newest_index =
        static_cast<uint8_t>((s_usb_page.camera_frame_tick_index +
                              CAMERA_FPS_WINDOW_SIZE - 1) % CAMERA_FPS_WINDOW_SIZE);
    const uint32_t oldest_ms = s_usb_page.camera_frame_ticks[oldest_index];
    const uint32_t newest_ms = s_usb_page.camera_frame_ticks[newest_index];
    const uint32_t elapsed_ms = newest_ms - oldest_ms;
    if (elapsed_ms == 0) {
        return 0.0f;
    }
    return static_cast<float>(count - 1) * 1000.0f / static_cast<float>(elapsed_ms);
}

bool ensure_camera_jpeg_decoder()
{
    if (s_usb_page.camera_jpeg_decoder) {
        return true;
    }

    jpeg_decode_engine_cfg_t cfg = {};
    cfg.intr_priority = 0;
    cfg.timeout_ms = 180;
    esp_err_t ret = jpeg_new_decoder_engine(&cfg, &s_usb_page.camera_jpeg_decoder);
    if (ret != ESP_OK) {
        char line[96];
        std::snprintf(line, sizeof(line), "JPEG decoder failed: %s", esp_err_to_name(ret));
        set_camera_status(line, COLOR_ERROR);
        ESP_LOGW(TAG, "%s", line);
        return false;
    }
    return true;
}

bool ensure_camera_rgb_buffer(size_t required, int canvas_w, int canvas_h)
{
    if (required == 0 || canvas_w <= 0 || canvas_h <= 0 || !s_usb_page.camera_image) {
        return false;
    }

    if (required > s_usb_page.camera_rgb_capacity) {
        lv_image_set_src(s_usb_page.camera_image, nullptr);

        jpeg_decode_memory_alloc_cfg_t alloc_cfg = {};
        alloc_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
        size_t allocated = 0;
        uint8_t* buffer = static_cast<uint8_t*>(
            jpeg_alloc_decoder_mem(required, &alloc_cfg, &allocated));
        if (!buffer) {
            ESP_LOGW(TAG, "Camera RGB buffer alloc failed, required=%u",
                     static_cast<unsigned>(required));
            return false;
        }

        if (s_usb_page.camera_rgb_buffer) {
            heap_caps_free(s_usb_page.camera_rgb_buffer);
        }
        s_usb_page.camera_rgb_buffer = buffer;
        s_usb_page.camera_rgb_capacity = allocated;
        s_usb_page.camera_canvas_w = 0;
        s_usb_page.camera_canvas_h = 0;
    }

    if (s_usb_page.camera_canvas_w != canvas_w ||
        s_usb_page.camera_canvas_h != canvas_h) {
        lv_canvas_set_buffer(s_usb_page.camera_image,
                             s_usb_page.camera_rgb_buffer,
                             canvas_w,
                             canvas_h,
                             LV_COLOR_FORMAT_RGB565);
        s_usb_page.camera_canvas_w = canvas_w;
        s_usb_page.camera_canvas_h = canvas_h;
    }
    return true;
}

bool find_jpeg_bounds(const uint8_t* data, size_t len, size_t* offset, size_t* size)
{
    if (!data || len < 4 || !offset || !size) {
        return false;
    }

    size_t start = len;
    for (size_t i = 0; i + 2 < len; ++i) {
        if (data[i] == 0xFF && data[i + 1] == 0xD8 && data[i + 2] == 0xFF) {
            start = i;
            break;
        }
    }
    if (start == len) {
        return false;
    }

    size_t end = len;
    for (size_t i = start + 2; i + 1 < len; ++i) {
        if (data[i] == 0xFF && data[i + 1] == 0xD9) {
            end = i + 2;
            break;
        }
    }

    *offset = start;
    *size = end - start;
    return *size > 4;
}

void get_camera_area_size(int16_t* width, int16_t* height)
{
    int16_t area_w = 432;
    int16_t area_h = USB_SUBPAGE_HEIGHT;
    if (s_usb_page.camera_area) {
        lv_obj_update_layout(s_usb_page.camera_area);
        area_w = lv_obj_get_width(s_usb_page.camera_area);
        area_h = lv_obj_get_height(s_usb_page.camera_area);
        if (area_w <= 0) {
            area_w = 432;
        }
        if (area_h <= 0) {
            area_h = USB_SUBPAGE_HEIGHT;
        }
    }
    if (width) {
        *width = area_w;
    }
    if (height) {
        *height = area_h;
    }
}

void update_camera_image_fit(int frame_w, int frame_h)
{
    if (!s_usb_page.camera_image || frame_w <= 0 || frame_h <= 0) {
        return;
    }

    int16_t area_w = 0;
    int16_t area_h = 0;
    get_camera_area_size(&area_w, &area_h);
    uint32_t zoom_w = static_cast<uint32_t>(area_w) * LV_SCALE_NONE /
                      static_cast<uint32_t>(frame_w);
    uint32_t zoom_h = static_cast<uint32_t>(area_h) * LV_SCALE_NONE /
                      static_cast<uint32_t>(frame_h);
    uint32_t zoom = zoom_w < zoom_h ? zoom_w : zoom_h;
    if (zoom == 0) {
        zoom = 1;
    } else if (zoom > LV_SCALE_NONE * 4) {
        zoom = LV_SCALE_NONE * 4;
    }

    lv_image_set_scale(s_usb_page.camera_image, zoom);
    lv_obj_center(s_usb_page.camera_image);
}

bool start_camera_stream_if_needed(const uvc_dev_info_t& info)
{
    if (s_usb_page.camera_streaming) {
        return true;
    }

    const int resolution_index =
        clamp_camera_resolution_index(info, s_usb_page.camera_resolution_index);
    s_usb_page.camera_resolution_index = resolution_index;

    if (info.if_streaming) {
        if (info.active_resolution >= 0 && info.active_resolution < info.resolution_count &&
            info.active_resolution != resolution_index) {
            app_uvc_control_dev_by_index(CAMERA_DEVICE_INDEX, false, info.active_resolution);
            set_camera_status("Switching camera mode...", 0xFFFFFF);
            return false;
        }

        s_usb_page.camera_streaming = true;
        s_usb_page.camera_resolution_index =
            (info.active_resolution >= 0 && info.active_resolution < info.resolution_count)
                ? info.active_resolution
                : 0;
        return true;
    }

    esp_err_t ret = app_uvc_control_dev_by_index(CAMERA_DEVICE_INDEX, true, resolution_index);
    if (ret != ESP_OK) {
        char line[96];
        std::snprintf(line, sizeof(line), "Camera start failed: %s", esp_err_to_name(ret));
        set_camera_status(line, COLOR_ERROR);
        return false;
    }

    s_usb_page.camera_streaming = true;
    s_usb_page.camera_resolution_index = resolution_index;
    s_usb_page.camera_next_frame_ms = usb_tick_ms() + 150;
    s_usb_page.camera_next_status_ms = 0;
    set_camera_status("Starting camera...", 0xFFFFFF);
    return true;
}

void refresh_camera_test(uint32_t now_ms)
{
    if (!s_usb_page.camera_area || !s_usb_page.camera_image) {
        return;
    }
    if (!s_usb_page.uvc_started) {
        set_camera_status("Camera host is not initialized.", COLOR_WARNING);
        return;
    }

    uvc_dev_info_t info = {};
    const int camera_count = query_camera_info(&info);
    if (camera_count <= 0) {
        stop_camera_stream();
        free_camera_frame_buffer();
        set_camera_status("Camera disconnected.", COLOR_WARNING);
        return;
    }
    if (info.resolution_count <= 0) {
        set_camera_status("Only MJPEG UVC cameras are supported.", COLOR_WARNING);
        return;
    }
    if (!start_camera_stream_if_needed(info)) {
        return;
    }

    if (s_usb_page.camera_next_frame_ms != 0 &&
        !time_reached(now_ms, s_usb_page.camera_next_frame_ms)) {
        return;
    }
    s_usb_page.camera_next_frame_ms = now_ms + CAMERA_FRAME_INTERVAL_MS;

    uvc_host_frame_t* frame = app_uvc_get_frame_by_index_timeout(CAMERA_DEVICE_INDEX, 0);
    if (!frame) {
        if (s_usb_page.camera_frame_count == 0 &&
            (s_usb_page.camera_next_status_ms == 0 ||
             time_reached(now_ms, s_usb_page.camera_next_status_ms))) {
            set_camera_status("Waiting for camera frame...", 0xFFFFFF);
            ESP_LOGW(TAG, "Waiting for camera frame: streaming=%d resolution_index=%d",
                     s_usb_page.camera_streaming,
                     s_usb_page.camera_resolution_index);
            s_usb_page.camera_next_status_ms = now_ms + 1000;
        }
        return;
    }

    const uint8_t* frame_data = static_cast<const uint8_t*>(frame->data);
    const size_t frame_len = frame->data_len;
    const uint8_t first0 = frame_len > 0 ? frame_data[0] : 0;
    const uint8_t first1 = frame_len > 1 ? frame_data[1] : 0;
    const uint8_t last0 = frame_len > 1 ? frame_data[frame_len - 2] : 0;
    const uint8_t last1 = frame_len > 0 ? frame_data[frame_len - 1] : 0;
    size_t jpeg_offset = 0;
    size_t jpeg_size = 0;
    int frame_w = static_cast<int>(frame->vs_format.h_res);
    int frame_h = static_cast<int>(frame->vs_format.v_res);
    int canvas_w = frame_w;
    int canvas_h = frame_h;
    bool decoded = false;
    const char* error_text = nullptr;
    esp_err_t decode_ret = ESP_OK;

    if (frame->vs_format.format != UVC_VS_FORMAT_MJPEG) {
        error_text = "Only MJPEG UVC cameras are supported.";
    } else if (!find_jpeg_bounds(frame_data, frame_len, &jpeg_offset, &jpeg_size)) {
        error_text = "Invalid camera JPEG frame.";
    } else if (!ensure_camera_jpeg_decoder()) {
        error_text = "JPEG decoder is not ready.";
    } else {
        const uint8_t* jpeg_data = frame_data + jpeg_offset;
        jpeg_decode_picture_info_t pic_info = {};
        decode_ret = jpeg_decoder_get_info(jpeg_data,
                                           static_cast<uint32_t>(jpeg_size),
                                           &pic_info);
        if (decode_ret != ESP_OK || pic_info.width == 0 || pic_info.height == 0) {
            error_text = "Invalid JPEG info.";
        } else {
            frame_w = static_cast<int>(pic_info.width);
            frame_h = static_cast<int>(pic_info.height);
            canvas_w = static_cast<int>(align_up_u32(pic_info.width, 16));
            canvas_h = static_cast<int>(align_up_u32(pic_info.height, 16));
            const size_t out_required = static_cast<size_t>(canvas_w) *
                                        static_cast<size_t>(canvas_h) * 2u;
            if (!ensure_camera_rgb_buffer(out_required, canvas_w, canvas_h)) {
                error_text = "Not enough memory for RGB frame.";
            } else {
                jpeg_decode_cfg_t decode_cfg = {};
                decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
                decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
                decode_cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
                uint32_t out_size = 0;
                decode_ret = jpeg_decoder_process(s_usb_page.camera_jpeg_decoder,
                                                  &decode_cfg,
                                                  jpeg_data,
                                                  static_cast<uint32_t>(jpeg_size),
                                                  s_usb_page.camera_rgb_buffer,
                                                  static_cast<uint32_t>(s_usb_page.camera_rgb_capacity),
                                                  &out_size);
                if (decode_ret == ESP_OK) {
                    decoded = true;
                } else {
                    error_text = "JPEG decode failed.";
                }
            }
        }
    }

    app_uvc_return_frame_by_index(CAMERA_DEVICE_INDEX, frame);

    if (!decoded) {
        set_camera_status(error_text ? error_text : "Camera frame error.", COLOR_WARNING);
        if (s_usb_page.camera_next_status_ms == 0 ||
            time_reached(now_ms, s_usb_page.camera_next_status_ms)) {
            ESP_LOGW(TAG, "Camera frame failed: %s ret=%s len=%u first=%02x%02x last=%02x%02x",
                     error_text ? error_text : "unknown",
                     esp_err_to_name(decode_ret),
                     static_cast<unsigned>(frame_len),
                     first0,
                     first1,
                     last0,
                     last1);
            s_usb_page.camera_next_status_ms = now_ms + 1000;
        }
        return;
    }

    s_usb_page.camera_frame_w = frame_w;
    s_usb_page.camera_frame_h = frame_h;
    ++s_usb_page.camera_frame_count;
    const float camera_fps = update_camera_fps(now_ms);

    update_camera_image_fit(canvas_w, canvas_h);
    lv_obj_invalidate(s_usb_page.camera_image);

    if (s_usb_page.camera_frame_count == 1) {
        ESP_LOGI(TAG, "Camera frame decoded: jpeg=%u src=%dx%d canvas=%dx%d",
                 static_cast<unsigned>(jpeg_size),
                 frame_w,
                 frame_h,
                 canvas_w,
                 canvas_h);
    }
    if (s_usb_page.camera_frame_count == 1 ||
        s_usb_page.camera_next_status_ms == 0 ||
        time_reached(now_ms, s_usb_page.camera_next_status_ms)) {
        char line[96];
        std::snprintf(line, sizeof(line), "%dx%d  %.1f fps",
                      frame_w,
                      frame_h,
                      static_cast<double>(camera_fps));
        set_camera_status(line, 0xFFFFFF);
        s_usb_page.camera_next_status_ms = now_ms + 500;
    }
}

void refresh_usb_status()
{
    auto* tool   = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;

    if (status && status->usb_mouse.usb_host_initialized) {
        s_usb_page.hid_started = true;
    }
    s_usb_page.uvc_started = s_uvc_host_installed;

    uvc_dev_info_t cam_info = {};
    s_usb_page.camera_count = query_camera_info(&cam_info);
    s_usb_page.mouse_connected = s_usb_page.hid_started && tool && tool->is_usb_mouse_connected();
    s_usb_page.keyboard_connected = s_usb_page.hid_started && tool && tool->is_usb_keyboard_connected();
    char other_name[48] = {};
    uint16_t other_vid = 0;
    uint16_t other_pid = 0;
    int other_total = 0;
    const bool other_info_ok = tool && tool->get_usb_other_device_info(other_name,
                                                                       sizeof(other_name),
                                                                       &other_vid,
                                                                       &other_pid,
                                                                       &other_total);
    s_usb_page.other_count = other_info_ok ? other_total : (tool ? tool->get_usb_other_device_count() : 0);

    const int hid_count = (s_usb_page.mouse_connected ? 1 : 0) +
                          (s_usb_page.keyboard_connected ? 1 : 0);

    char chip_text[24];
    std::snprintf(chip_text, sizeof(chip_text), "CAM %d", s_usb_page.camera_count);
    set_chip(s_usb_page.cam_chip, s_usb_page.cam_chip_label, chip_text,
             s_usb_page.camera_count > 0 ? COLOR_SUCCESS : COLOR_CHIP_IDLE,
             s_usb_page.camera_count > 0 ? 0xFFFFFF : COLOR_TEXT);

    std::snprintf(chip_text, sizeof(chip_text), "HID %d", hid_count);
    set_chip(s_usb_page.hid_chip, s_usb_page.hid_chip_label, chip_text,
             hid_count > 0 ? COLOR_SUCCESS : COLOR_CHIP_IDLE,
             hid_count > 0 ? 0xFFFFFF : COLOR_TEXT);

    std::snprintf(chip_text, sizeof(chip_text), "OTHER %d", s_usb_page.other_count);
    set_chip(s_usb_page.other_chip, s_usb_page.other_chip_label, chip_text,
             s_usb_page.other_count > 0 ? COLOR_WARNING : COLOR_CHIP_IDLE,
             s_usb_page.other_count > 0 ? 0xFFFFFF : COLOR_TEXT);

    const bool show_cam = s_usb_page.camera_count > 0;
    set_visible(s_usb_page.cam_row, show_cam);
    if (show_cam) {
        set_label_text(s_usb_page.cam_title_label, "UVC Camera");
        char detail[96];
        if (cam_info.resolution_count > 0) {
            std::snprintf(detail, sizeof(detail), "%d modes | tap to select",
                          cam_info.resolution_count);
        } else {
            std::snprintf(detail, sizeof(detail), "Ready | tap to select");
        }
        set_label_text(s_usb_page.cam_info_label, detail);
    }

    set_visible(s_usb_page.mouse_row, s_usb_page.mouse_connected);
    if (s_usb_page.mouse_connected) {
        set_label_text(s_usb_page.mouse_title_label, "USB Mouse");
        set_label_text(s_usb_page.mouse_info_label, "Pointer test ready");
    }

    set_visible(s_usb_page.keyboard_row, s_usb_page.keyboard_connected);
    if (s_usb_page.keyboard_connected) {
        set_label_text(s_usb_page.keyboard_title_label, "USB Keyboard");
        set_label_text(s_usb_page.keyboard_info_label, "Text input test ready");
    }

    set_visible(s_usb_page.other_row, s_usb_page.other_count > 0);
    if (s_usb_page.other_count > 0) {
        set_label_text(s_usb_page.other_title_label,
                       other_name[0] ? other_name : "Other USB");
        char detail[96];
        std::snprintf(detail, sizeof(detail), "vid=0x%04X pid=0x%04X%s",
                      static_cast<unsigned>(other_vid),
                      static_cast<unsigned>(other_pid),
                      s_usb_page.other_count > 1 ? " +" : "");
        set_label_text(s_usb_page.other_info_label, detail);
    }
}

void action_check(char* out, size_t len, uint32_t* col)
{
    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        std::snprintf(out, len, "Tool not available.");
        if (col) *col = COLOR_ERROR;
        return;
    }

    esp_err_t hid_ret = ESP_OK;
    auto* status = tool->get_device_status();
    if (!status || !status->power.pyb_usba_power) {
        tool->set_status(StatusParam::POWER_PYB_USBA, true,
                         StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::APPLY_HARDWARE);
        vTaskDelay(pdMS_TO_TICKS(200));
        status = tool->get_device_status();
    }

    if (status && status->usb_mouse.usb_host_initialized) {
        s_usb_page.hid_started = true;
    } else {
        hid_ret = tool->init_usb_mouse_support();
        if (hid_ret == ESP_OK) {
            s_usb_page.hid_started = true;
            if (status) {
                status->usb_mouse.usb_host_initialized = true;
            }
        }
    }

    esp_err_t uvc_ret = ESP_OK;
    if (!s_uvc_host_installed) {
        uvc_ret = tool->uvc_usb_init();
        if (uvc_ret == ESP_OK) {
            s_uvc_host_installed = true;
        }
    }
    s_usb_page.uvc_started = s_uvc_host_installed;

    esp_err_t monitor_ret = tool->init_usb_device_monitor();

    int dev_num = 0;
    if (s_usb_page.uvc_started) {
        app_uvc_get_connect_dev_num(&dev_num);
    }
    const bool mouse_ok = s_usb_page.hid_started && tool->is_usb_mouse_connected();
    const bool keyboard_ok = s_usb_page.hid_started && tool->is_usb_keyboard_connected();
    const int other_count = tool->get_usb_other_device_count();

    if (hid_ret != ESP_OK) {
        std::snprintf(out, len, "Check warning: HID %s", esp_err_to_name(hid_ret));
        if (col) *col = COLOR_WARNING;
    } else if (uvc_ret != ESP_OK) {
        std::snprintf(out, len, "Check warning: UVC %s", esp_err_to_name(uvc_ret));
        if (col) *col = COLOR_WARNING;
    } else if (monitor_ret != ESP_OK) {
        std::snprintf(out, len, "Check warning: monitor %s", esp_err_to_name(monitor_ret));
        if (col) *col = COLOR_WARNING;
    } else if (dev_num > 0 || mouse_ok || keyboard_ok || other_count > 0) {
        std::snprintf(out, len, "Check OK: devices ready.");
        if (col) *col = COLOR_SUCCESS;
    } else {
        std::snprintf(out, len, "Hub ready.");
        if (col) *col = COLOR_SUCCESS;
    }
}

void usb_action_task(void* param)
{
    UsbAction action = static_cast<UsbAction>(reinterpret_cast<uintptr_t>(param));
    char result[160];
    uint32_t color = COLOR_ERROR;
    result[0] = '\0';

    switch (action) {
        case UsbAction::CHECK:
            action_check(result, sizeof(result), &color);
            break;
    }

    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock LVGL for USB update");
        s_usb_page.task_running = false;
        vTaskDelete(nullptr);
        return;
    }
    s_usb_page.task_running = false;
    if (s_usb_page.page_alive) {
        set_result_text(result, color);
        if (s_usb_page.active_view == UsbView::HUB) {
            refresh_usb_status();
        }
    }
    lvgl_port_unlock();
    vTaskDelete(nullptr);
}

void start_usb_action(UsbAction action)
{
    if (s_usb_page.task_running) {
        return;
    }
    const uint32_t now_ms = usb_tick_ms();
    s_usb_page.task_running = true;
    set_result_text("Checking USB...", COLOR_WARNING);
    BaseType_t ret = xTaskCreate(usb_action_task,
                                 "func_usb",
                                 8192,
                                 reinterpret_cast<void*>(static_cast<uintptr_t>(action)),
                                 configMAX_PRIORITIES - 4,
                                 nullptr);
    if (ret != pdPASS) {
        s_usb_page.task_running = false;
        set_result_text("USB action failed: cannot create worker task.", COLOR_ERROR);
        if (s_usb_page.active_view == UsbView::HUB) {
            refresh_usb_status();
        }
    } else if (action == UsbAction::CHECK) {
        s_usb_page.next_auto_check_ms = now_ms + AUTO_CHECK_INTERVAL_MS;
    }
}

void cleanup_active_view()
{
    if (s_usb_page.active_view == UsbView::MOUSE_TEST) {
        schedule_mouse_support_destroy();
        s_usb_page.mouse_draw_active = false;
        s_mouse_dirty.reset();
        set_header_subpage_exit_mode(false);
    } else if (s_usb_page.active_view == UsbView::CAMERA_TEST) {
        stop_camera_stream();
        free_camera_frame_buffer();
        set_header_subpage_exit_mode(false);
    }
}

void reset_hub_widget_refs()
{
    s_usb_page.cam_chip = nullptr;
    s_usb_page.cam_chip_label = nullptr;
    s_usb_page.hid_chip = nullptr;
    s_usb_page.hid_chip_label = nullptr;
    s_usb_page.other_chip = nullptr;
    s_usb_page.other_chip_label = nullptr;
    s_usb_page.cam_row = nullptr;
    s_usb_page.cam_title_label = nullptr;
    s_usb_page.cam_info_label = nullptr;
    s_usb_page.mouse_row = nullptr;
    s_usb_page.mouse_title_label = nullptr;
    s_usb_page.mouse_info_label = nullptr;
    s_usb_page.keyboard_row = nullptr;
    s_usb_page.keyboard_title_label = nullptr;
    s_usb_page.keyboard_info_label = nullptr;
    s_usb_page.other_row = nullptr;
    s_usb_page.other_title_label = nullptr;
    s_usb_page.other_info_label = nullptr;
    s_usb_page.result_label = nullptr;
    s_usb_page.mouse_area = nullptr;
    s_usb_page.mouse_canvas = nullptr;
    s_usb_page.mouse_canvas_w = 0;
    s_usb_page.mouse_canvas_h = 0;
    s_usb_page.mouse_xy_label = nullptr;
    s_usb_page.mouse_button_label = nullptr;
    s_usb_page.camera_area = nullptr;
    s_usb_page.camera_image = nullptr;
    s_usb_page.camera_status_label = nullptr;
}

void set_keyboard_text_label()
{
    char display[sizeof(s_usb_page.keyboard_text) + 2];
    const size_t copy_len = s_usb_page.keyboard_text_len < sizeof(s_usb_page.keyboard_text)
                                ? s_usb_page.keyboard_text_len
                                : sizeof(s_usb_page.keyboard_text) - 1;
    if (copy_len > 0) {
        std::memcpy(display, s_usb_page.keyboard_text, copy_len);
    }
    display[copy_len] = s_usb_page.keyboard_cursor_visible ? '|' : ' ';
    display[copy_len + 1] = '\0';
    set_label_text(s_usb_page.keyboard_text_label, display);
}

void restart_keyboard_cursor_blink()
{
    s_usb_page.keyboard_cursor_visible = true;
    s_usb_page.keyboard_cursor_next_toggle_ms = usb_tick_ms() + KEYBOARD_CURSOR_BLINK_MS;
}

void update_keyboard_cursor_blink(uint32_t now_ms)
{
    if (!s_usb_page.keyboard_text_label) {
        return;
    }
    if (s_usb_page.keyboard_cursor_next_toggle_ms == 0) {
        s_usb_page.keyboard_cursor_next_toggle_ms = now_ms + KEYBOARD_CURSOR_BLINK_MS;
        return;
    }
    if (!time_reached(now_ms, s_usb_page.keyboard_cursor_next_toggle_ms)) {
        return;
    }
    s_usb_page.keyboard_cursor_visible = !s_usb_page.keyboard_cursor_visible;
    s_usb_page.keyboard_cursor_next_toggle_ms = now_ms + KEYBOARD_CURSOR_BLINK_MS;
    set_keyboard_text_label();
}

void append_keyboard_text(const char* text)
{
    if (!text || text[0] == '\0') {
        return;
    }
    const size_t add_len = std::strlen(text);
    if (add_len >= sizeof(s_usb_page.keyboard_text)) {
        return;
    }
    if (s_usb_page.keyboard_text_len + add_len >= sizeof(s_usb_page.keyboard_text)) {
        s_usb_page.keyboard_text_len = 0;
        s_usb_page.keyboard_text[0] = '\0';
    }
    std::memcpy(&s_usb_page.keyboard_text[s_usb_page.keyboard_text_len], text, add_len + 1);
    s_usb_page.keyboard_text_len += add_len;
    restart_keyboard_cursor_blink();
    set_keyboard_text_label();
}

void keyboard_backspace()
{
    if (s_usb_page.keyboard_text_len == 0) {
        return;
    }
    --s_usb_page.keyboard_text_len;
    s_usb_page.keyboard_text[s_usb_page.keyboard_text_len] = '\0';
    restart_keyboard_cursor_blink();
    set_keyboard_text_label();
}

bool keycode_to_text(uint8_t key, uint8_t modifier, bool caps_lock, char* out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    const bool shift = (modifier & 0x22) != 0;
    const bool ctrl = (modifier & 0x11) != 0;
    const bool alt = (modifier & 0x44) != 0;
    const bool gui = (modifier & 0x88) != 0;
    if (ctrl || alt || gui) {
        return false;
    }
    if (key >= 0x04 && key <= 0x1D) {
        char c = static_cast<char>(((shift ^ caps_lock) ? 'A' : 'a') + key - 0x04);
        std::snprintf(out, out_len, "%c", c);
        return true;
    }
    if (key >= 0x1E && key <= 0x26) {
        static const char normal[] = "123456789";
        static const char shifted[] = "!@#$%^&*(";
        std::snprintf(out, out_len, "%c", shift ? shifted[key - 0x1E] : normal[key - 0x1E]);
        return true;
    }
    switch (key) {
        case 0x27: std::snprintf(out, out_len, "%c", shift ? ')' : '0'); return true;
        case 0x28: std::snprintf(out, out_len, "\n"); return true;
        case 0x2B: std::snprintf(out, out_len, "    "); return true;
        case 0x2C: std::snprintf(out, out_len, " "); return true;
        case 0x2D: std::snprintf(out, out_len, "%c", shift ? '_' : '-'); return true;
        case 0x2E: std::snprintf(out, out_len, "%c", shift ? '+' : '='); return true;
        case 0x2F: std::snprintf(out, out_len, "%c", shift ? '{' : '['); return true;
        case 0x30: std::snprintf(out, out_len, "%c", shift ? '}' : ']'); return true;
        case 0x31: std::snprintf(out, out_len, "%c", shift ? '|' : '\\'); return true;
        case 0x32: std::snprintf(out, out_len, "%c", shift ? '~' : '#'); return true;
        case 0x33: std::snprintf(out, out_len, "%c", shift ? ':' : ';'); return true;
        case 0x34: std::snprintf(out, out_len, "%c", shift ? '"' : '\''); return true;
        case 0x35: std::snprintf(out, out_len, "%c", shift ? '~' : '`'); return true;
        case 0x36: std::snprintf(out, out_len, "%c", shift ? '<' : ','); return true;
        case 0x37: std::snprintf(out, out_len, "%c", shift ? '>' : '.'); return true;
        case 0x38: std::snprintf(out, out_len, "%c", shift ? '?' : '/'); return true;
        case 0x54: std::snprintf(out, out_len, "/"); return true;
        case 0x55: std::snprintf(out, out_len, "*"); return true;
        case 0x56: std::snprintf(out, out_len, "-"); return true;
        case 0x57: std::snprintf(out, out_len, "+"); return true;
        case 0x58: std::snprintf(out, out_len, "\n"); return true;
        case 0x59: std::snprintf(out, out_len, "1"); return true;
        case 0x5A: std::snprintf(out, out_len, "2"); return true;
        case 0x5B: std::snprintf(out, out_len, "3"); return true;
        case 0x5C: std::snprintf(out, out_len, "4"); return true;
        case 0x5D: std::snprintf(out, out_len, "5"); return true;
        case 0x5E: std::snprintf(out, out_len, "6"); return true;
        case 0x5F: std::snprintf(out, out_len, "7"); return true;
        case 0x60: std::snprintf(out, out_len, "8"); return true;
        case 0x61: std::snprintf(out, out_len, "9"); return true;
        case 0x62: std::snprintf(out, out_len, "0"); return true;
        case 0x63: std::snprintf(out, out_len, "."); return true;
        case 0x64: std::snprintf(out, out_len, "%c", shift ? '|' : '\\'); return true;
        case 0x67: std::snprintf(out, out_len, "="); return true;
        default: break;
    }
    return false;
}

bool key_was_down(uint8_t key, const uint8_t* keys, size_t key_count)
{
    if (key == 0 || !keys) {
        return false;
    }
    for (size_t i = 0; i < key_count; ++i) {
        if (keys[i] == key) {
            return true;
        }
    }
    return false;
}

int16_t clamp_coord(int32_t value, int16_t min_value, int16_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return static_cast<int16_t>(value);
}

uint32_t canvas_buffer_size_px(int32_t w, int32_t h, lv_color_format_t cf)
{
    if (w <= 0 || h <= 0) {
        return 0;
    }

    const uint8_t bpp = lv_color_format_get_bpp(cf);
    if (bpp == 0) {
        return 0;
    }

    uint32_t stride = lv_draw_buf_width_to_stride(static_cast<uint32_t>(w), cf);
    if (stride == 0) {
        stride = (static_cast<uint32_t>(w) * bpp + 7u) / 8u;
    }

    return stride * static_cast<uint32_t>(h) + LV_DRAW_BUF_ALIGN +
           LV_COLOR_INDEXED_PALETTE_SIZE(cf) * sizeof(lv_color32_t);
}

uint16_t to_rgb565_raw(uint32_t hex)
{
    lv_color16_t c;
    c.red = static_cast<uint16_t>(((hex >> 16) & 0xFF) >> 3);
    c.green = static_cast<uint16_t>(((hex >> 8) & 0xFF) >> 2);
    c.blue = static_cast<uint16_t>((hex & 0xFF) >> 3);
    uint16_t raw;
    __builtin_memcpy(&raw, &c, sizeof(raw));
    return raw;
}

uint8_t* mouse_canvas_data()
{
    if (!s_usb_page.mouse_canvas) {
        return nullptr;
    }
    lv_draw_buf_t* db = lv_canvas_get_draw_buf(s_usb_page.mouse_canvas);
    return db ? db->data : nullptr;
}

uint32_t mouse_canvas_stride()
{
    if (!s_usb_page.mouse_canvas) {
        return 0;
    }
    lv_draw_buf_t* db = lv_canvas_get_draw_buf(s_usb_page.mouse_canvas);
    return db ? db->header.stride : 0;
}

void mouse_canvas_put_px(uint8_t* base, uint32_t stride, int16_t x, int16_t y, uint16_t raw)
{
    if (!base || stride == 0 ||
        x < 0 || y < 0 ||
        x >= s_usb_page.mouse_canvas_w ||
        y >= s_usb_page.mouse_canvas_h) {
        return;
    }
    *reinterpret_cast<uint16_t*>(base + static_cast<uint32_t>(y) * stride +
                                 static_cast<uint32_t>(x) * 2u) = raw;
    s_mouse_dirty.add_point(x, y);
}

void invalidate_mouse_canvas()
{
    if (!s_usb_page.mouse_canvas || !s_mouse_dirty.valid) {
        return;
    }

    lv_area_t canvas_area;
    lv_obj_get_coords(s_usb_page.mouse_canvas, &canvas_area);

    lv_area_t area;
    area.x1 = canvas_area.x1 + s_mouse_dirty.x1;
    area.y1 = canvas_area.y1 + s_mouse_dirty.y1;
    area.x2 = canvas_area.x1 + s_mouse_dirty.x2;
    area.y2 = canvas_area.y1 + s_mouse_dirty.y2;
    lv_obj_invalidate_area(s_usb_page.mouse_canvas, &area);
    s_mouse_dirty.reset();
}

bool mouse_point_to_area(const usb_mouse_data_t& data, int16_t* out_x, int16_t* out_y)
{
    if (!s_usb_page.mouse_area || !out_x || !out_y) {
        return false;
    }

    lv_area_t area;
    lv_obj_get_coords(s_usb_page.mouse_area, &area);

    if (data.x < area.x1 || data.x > area.x2 ||
        data.y < area.y1 || data.y > area.y2) {
        return false;
    }

    *out_x = clamp_coord(static_cast<int32_t>(data.x) - area.x1, 0,
                         lv_obj_get_width(s_usb_page.mouse_area) - 1);
    *out_y = clamp_coord(static_cast<int32_t>(data.y) - area.y1, 0,
                         lv_obj_get_height(s_usb_page.mouse_area) - 1);
    return true;
}

void clear_mouse_strokes()
{
    if (s_usb_page.mouse_canvas) {
        s_mouse_dirty.reset();
        lv_canvas_fill_bg(s_usb_page.mouse_canvas, lv_color_hex(COLOR_MOUSE_AREA), LV_OPA_COVER);
        lv_obj_invalidate(s_usb_page.mouse_canvas);
    }
    s_usb_page.mouse_draw_active = false;
}

void draw_mouse_stamp(int16_t cx, int16_t cy, uint16_t raw)
{
    uint8_t* base = mouse_canvas_data();
    const uint32_t stride = mouse_canvas_stride();
    if (!base || stride == 0) {
        return;
    }

    for (int16_t dy = -MOUSE_BRUSH_RADIUS; dy <= MOUSE_BRUSH_RADIUS; ++dy) {
        for (int16_t dx = -MOUSE_BRUSH_RADIUS; dx <= MOUSE_BRUSH_RADIUS; ++dx) {
            if (dx * dx + dy * dy > MOUSE_BRUSH_RADIUS * MOUSE_BRUSH_RADIUS) {
                continue;
            }
            mouse_canvas_put_px(base, stride, cx + dx, cy + dy, raw);
        }
    }
}

void draw_mouse_stroke(int16_t x, int16_t y, uint32_t color)
{
    const uint16_t raw = to_rgb565_raw(color);
    if (!s_usb_page.mouse_draw_active || s_usb_page.mouse_draw_color != color) {
        s_usb_page.mouse_draw_active = true;
        s_usb_page.mouse_draw_color = color;
        s_usb_page.mouse_last_x = x;
        s_usb_page.mouse_last_y = y;
        s_mouse_dirty.reset();
        draw_mouse_stamp(x, y, raw);
        invalidate_mouse_canvas();
        return;
    }

    if (s_usb_page.mouse_last_x == x && s_usb_page.mouse_last_y == y) {
        return;
    }

    int16_t x0 = s_usb_page.mouse_last_x;
    int16_t y0 = s_usb_page.mouse_last_y;
    int16_t x1 = x;
    int16_t y1 = y;
    int16_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int16_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    const int16_t sx = x0 < x1 ? 1 : -1;
    const int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = dx - dy;

    s_mouse_dirty.reset();
    while (true) {
        draw_mouse_stamp(x0, y0, raw);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int16_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }

    s_usb_page.mouse_last_x = x;
    s_usb_page.mouse_last_y = y;
    invalidate_mouse_canvas();
}

void update_mouse_drawing(const usb_mouse_data_t& data)
{
    const bool draw_left = data.left_button;
    const bool draw_right = data.right_button;
    if (!draw_left && !draw_right) {
        s_usb_page.mouse_draw_active = false;
        return;
    }

    int16_t x = 0;
    int16_t y = 0;
    if (!mouse_point_to_area(data, &x, &y)) {
        s_usb_page.mouse_draw_active = false;
        return;
    }

    const uint32_t color = draw_right ? COLOR_MOUSE_STROKE_RIGHT : COLOR_MOUSE_STROKE_LEFT;
    draw_mouse_stroke(x, y, color);
}

void refresh_mouse_test()
{
    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        return;
    }

    usb_mouse_data_t data = {};
    esp_err_t ret = tool->get_usb_mouse_data(&data);
    if (ret != ESP_OK) {
        tool->destroy_lvgl_mouse_support();
        s_usb_page.mouse_draw_active = false;
        set_label_text(s_usb_page.mouse_xy_label, "Mouse disconnected");
        set_label_text(s_usb_page.mouse_button_label, "Waiting for mouse data");
        return;
    }

    if (!tool->get_lvgl_mouse_indev()) {
        tool->create_lvgl_mouse_input_device();
    }

    char line[80];
    std::snprintf(line, sizeof(line), "X %d   Y %d   Wheel %d",
                  static_cast<int>(data.x),
                  static_cast<int>(data.y),
                  static_cast<int>(data.wheel));
    set_label_text(s_usb_page.mouse_xy_label, line);
    std::snprintf(line, sizeof(line), "L %s   R %s   M %s",
                  data.left_button ? "ON" : "OFF",
                  data.right_button ? "ON" : "OFF",
                  data.middle_button ? "ON" : "OFF");
    set_label_text(s_usb_page.mouse_button_label, line);
    update_mouse_drawing(data);
}

void refresh_keyboard_test()
{
    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        return;
    }

    usb_keyboard_data_t data = {};
    esp_err_t ret = tool->get_usb_keyboard_data(&data);
    if (ret != ESP_OK) {
        s_usb_page.keyboard_last_key = 0;
        std::memset(s_usb_page.keyboard_prev_keys, 0, sizeof(s_usb_page.keyboard_prev_keys));
        set_label_text(s_usb_page.keyboard_status_label, "Keyboard disconnected");
        return;
    }

    uint8_t current_key = 0;
    uint8_t pressed_keys[sizeof(data.keycodes)] = {};
    for (size_t i = 0; i < sizeof(data.keycodes); ++i) {
        if (data.keycodes[i] != 0) {
            if (current_key == 0) {
                current_key = data.keycodes[i];
            }
            pressed_keys[i] = data.keycodes[i];
        }
    }

    for (size_t i = 0; i < sizeof(pressed_keys); ++i) {
        const uint8_t key = pressed_keys[i];
        if (key == 0 || key_was_down(key,
                                     s_usb_page.keyboard_prev_keys,
                                     sizeof(s_usb_page.keyboard_prev_keys))) {
            continue;
        }

        if (key == 0x2A) {
            keyboard_backspace();
        } else if (key == 0x39) {
            s_usb_page.keyboard_caps_lock = !s_usb_page.keyboard_caps_lock;
        } else {
            char text[8];
            if (keycode_to_text(key,
                                data.modifier,
                                s_usb_page.keyboard_caps_lock,
                                text,
                                sizeof(text))) {
                append_keyboard_text(text);
            }
        }
    }
    s_usb_page.keyboard_last_key = current_key;
    std::memcpy(s_usb_page.keyboard_prev_keys, pressed_keys, sizeof(s_usb_page.keyboard_prev_keys));

    char line[80];
    std::snprintf(line, sizeof(line), "Last key: %s", data.last_key_name[0] ? data.last_key_name : "--");
    set_label_text(s_usb_page.keyboard_status_label, line);
}

void usb_keyboard_clear_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_usb_page.keyboard_text_len = 0;
    s_usb_page.keyboard_text[0] = '\0';
    restart_keyboard_cursor_blink();
    set_keyboard_text_label();
}

void usb_mouse_clean_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    clear_mouse_strokes();
}

void usb_mouse_canvas_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    void* buffer = lv_event_get_user_data(e);
    if (buffer && buffer == s_usb_page.mouse_canvas_buffer) {
        s_usb_page.mouse_canvas_buffer = nullptr;
        heap_caps_free(buffer);
    }
    if (lv_event_get_target(e) == s_usb_page.mouse_canvas) {
        s_usb_page.mouse_canvas = nullptr;
        s_usb_page.mouse_canvas_w = 0;
        s_usb_page.mouse_canvas_h = 0;
    }
}

void open_camera_test_page(int resolution_index)
{
    if (!s_usb_page.content_root) {
        return;
    }
    cleanup_active_view();
    s_usb_page.active_view = UsbView::CAMERA_TEST;
    s_usb_page.camera_streaming = false;
    s_usb_page.camera_resolution_index = resolution_index;
    s_usb_page.camera_frame_count = 0;
    std::memset(s_usb_page.camera_frame_ticks, 0, sizeof(s_usb_page.camera_frame_ticks));
    s_usb_page.camera_frame_tick_count = 0;
    s_usb_page.camera_frame_tick_index = 0;
    s_usb_page.camera_next_frame_ms = 0;
    s_usb_page.camera_next_status_ms = 0;
    lv_obj_clean(s_usb_page.content_root);
    reset_hub_widget_refs();
    set_header_subpage_exit_mode(true);

    s_usb_page.camera_area = lv_obj_create(s_usb_page.content_root);
    lv_obj_remove_flag(s_usb_page.camera_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(s_usb_page.camera_area, lv_pct(100));
    lv_obj_set_height(s_usb_page.camera_area, USB_SUBPAGE_HEIGHT);
    lv_obj_set_style_bg_color(s_usb_page.camera_area, lv_color_hex(COLOR_CAMERA_AREA), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_usb_page.camera_area, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_usb_page.camera_area, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_usb_page.camera_area, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_radius(s_usb_page.camera_area, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_usb_page.camera_area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_usb_page.camera_area, 0, LV_PART_MAIN);

    s_usb_page.camera_image = lv_canvas_create(s_usb_page.camera_area);
    lv_obj_remove_flag(s_usb_page.camera_image, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_usb_page.camera_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_usb_page.camera_image);

    s_usb_page.camera_status_label = lv_label_create(s_usb_page.camera_area);
    lv_obj_set_width(s_usb_page.camera_status_label, lv_pct(100));
    lv_label_set_long_mode(s_usb_page.camera_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_usb_page.camera_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_usb_page.camera_status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_usb_page.camera_status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(s_usb_page.camera_status_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    set_camera_status("Starting camera...", 0xFFFFFF);

    refresh_camera_test(usb_tick_ms());
}

void open_mouse_test_page()
{
    if (!s_usb_page.content_root) {
        return;
    }
    cancel_pending_mouse_support_destroy();
    cleanup_active_view();
    s_usb_page.active_view = UsbView::MOUSE_TEST;
    lv_obj_clean(s_usb_page.content_root);
    reset_hub_widget_refs();
    s_usb_page.mouse_draw_active = false;
    set_header_subpage_exit_mode(true);

    s_usb_page.mouse_area = lv_obj_create(s_usb_page.content_root);
    lv_obj_remove_flag(s_usb_page.mouse_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(s_usb_page.mouse_area, lv_pct(100));
    lv_obj_set_height(s_usb_page.mouse_area, USB_SUBPAGE_HEIGHT);
    lv_obj_set_style_bg_color(s_usb_page.mouse_area, lv_color_hex(COLOR_MOUSE_AREA), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_usb_page.mouse_area, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_usb_page.mouse_area, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_usb_page.mouse_area, lv_color_hex(0x93C5FD), LV_PART_MAIN);
    lv_obj_set_style_radius(s_usb_page.mouse_area, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_usb_page.mouse_area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_usb_page.mouse_area, 0, LV_PART_MAIN);

    lv_obj_update_layout(s_usb_page.mouse_area);
    s_usb_page.mouse_canvas_w = lv_obj_get_width(s_usb_page.mouse_area);
    s_usb_page.mouse_canvas_h = lv_obj_get_height(s_usb_page.mouse_area);
    if (s_usb_page.mouse_canvas_w <= 0) {
        s_usb_page.mouse_canvas_w = 432;
    }
    if (s_usb_page.mouse_canvas_h <= 0) {
        s_usb_page.mouse_canvas_h = USB_SUBPAGE_HEIGHT;
    }

    s_usb_page.mouse_canvas = lv_canvas_create(s_usb_page.mouse_area);
    lv_obj_remove_flag(s_usb_page.mouse_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_usb_page.mouse_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_usb_page.mouse_canvas, 0, 0);
    lv_obj_set_size(s_usb_page.mouse_canvas,
                    s_usb_page.mouse_canvas_w,
                    s_usb_page.mouse_canvas_h);

    const lv_color_format_t cf = LV_COLOR_FORMAT_RGB565;
    const uint32_t buf_size = canvas_buffer_size_px(s_usb_page.mouse_canvas_w,
                                                    s_usb_page.mouse_canvas_h,
                                                    cf);
    s_usb_page.mouse_canvas_buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_usb_page.mouse_canvas_buffer) {
        s_usb_page.mouse_canvas_buffer = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
    }
    if (s_usb_page.mouse_canvas_buffer) {
        lv_canvas_set_buffer(s_usb_page.mouse_canvas,
                             s_usb_page.mouse_canvas_buffer,
                             s_usb_page.mouse_canvas_w,
                             s_usb_page.mouse_canvas_h,
                             cf);
        lv_obj_add_event_cb(s_usb_page.mouse_canvas,
                            usb_mouse_canvas_delete_event_cb,
                            LV_EVENT_DELETE,
                            s_usb_page.mouse_canvas_buffer);
        lv_canvas_fill_bg(s_usb_page.mouse_canvas, lv_color_hex(COLOR_MOUSE_AREA), LV_OPA_COVER);
    }

    s_usb_page.mouse_xy_label = lv_label_create(s_usb_page.mouse_area);
    lv_obj_set_pos(s_usb_page.mouse_xy_label, 12, 12);
    lv_obj_set_width(s_usb_page.mouse_xy_label, 246);
    lv_label_set_text(s_usb_page.mouse_xy_label, "X --   Y --   Wheel --");
    lv_label_set_long_mode(s_usb_page.mouse_xy_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_usb_page.mouse_xy_label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_usb_page.mouse_xy_label, &lv_font_montserrat_16, LV_PART_MAIN);

    s_usb_page.mouse_button_label = lv_label_create(s_usb_page.mouse_area);
    lv_obj_set_pos(s_usb_page.mouse_button_label, 12, 36);
    lv_obj_set_width(s_usb_page.mouse_button_label, 246);
    lv_label_set_text(s_usb_page.mouse_button_label, "L --   R --   M --");
    lv_label_set_long_mode(s_usb_page.mouse_button_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_usb_page.mouse_button_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_usb_page.mouse_button_label, &lv_font_montserrat_16, LV_PART_MAIN);

    lv_obj_t* clean_btn = add_button(s_usb_page.mouse_area, "Clean", 0xDBEAFE, COLOR_TEXT, 92, 36);
    lv_obj_align(clean_btn, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_add_event_cb(clean_btn, usb_mouse_clean_event_cb, LV_EVENT_CLICKED, nullptr);

    auto* tool = corep4_ui_sync_get_tool();
    if (tool) {
        tool->create_lvgl_mouse_input_device();
    }
    refresh_mouse_test();
}

void open_keyboard_test_page()
{
    if (!s_usb_page.content_root) {
        return;
    }
    cleanup_active_view();
    s_usb_page.active_view = UsbView::KEYBOARD_TEST;
    s_usb_page.keyboard_last_key = 0;
    std::memset(s_usb_page.keyboard_prev_keys, 0, sizeof(s_usb_page.keyboard_prev_keys));
    s_usb_page.keyboard_caps_lock = false;
    restart_keyboard_cursor_blink();
    set_header_subpage_exit_mode(true);
    lv_obj_clean(s_usb_page.content_root);
    reset_hub_widget_refs();

    lv_obj_t* card = add_card(s_usb_page.content_root, USB_SUBPAGE_HEIGHT);
    lv_obj_t* header = add_row(card, 42);
    lv_obj_t* title_label = add_label(header, "USB Keyboard", COLOR_TEXT, 300);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_t* clear_btn = add_button(header, "Clear", COLOR_BUTTON_BG, COLOR_TEXT, 96, 36);
    set_button_label_font(clear_btn, &lv_font_montserrat_20);
    lv_obj_add_event_cb(clear_btn, usb_keyboard_clear_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* text_box = lv_obj_create(card);
    lv_obj_set_width(text_box, lv_pct(100));
    lv_obj_set_height(text_box, 270);
    lv_obj_set_style_bg_color(text_box, lv_color_hex(COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(text_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(text_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(text_box, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(text_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(text_box, 10, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(text_box, LV_SCROLLBAR_MODE_AUTO);

    s_usb_page.keyboard_text_label = add_label(text_box, "", COLOR_TEXT);
    lv_label_set_long_mode(s_usb_page.keyboard_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_usb_page.keyboard_text_label, lv_pct(100));
    lv_obj_set_style_text_font(s_usb_page.keyboard_text_label, &lv_font_montserrat_24, LV_PART_MAIN);
    set_keyboard_text_label();

    s_usb_page.keyboard_status_label = add_label(card, "Last key: --", COLOR_MUTED);
    lv_label_set_long_mode(s_usb_page.keyboard_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_usb_page.keyboard_status_label, &lv_font_montserrat_20, LV_PART_MAIN);
    refresh_keyboard_test();
}

void usb_device_open_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    UsbDeviceOpen target = static_cast<UsbDeviceOpen>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    switch (target) {
        case UsbDeviceOpen::CAMERA:
            show_camera_mode_select();
            break;
        case UsbDeviceOpen::MOUSE:
            open_mouse_test_page();
            break;
        case UsbDeviceOpen::KEYBOARD:
            open_keyboard_test_page();
            break;
    }
}

void usb_check_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    start_usb_action(UsbAction::CHECK);
}

void run_auto_check_if_due(uint32_t now_ms)
{
    if (s_usb_page.active_view != UsbView::HUB) {
        return;
    }
    if (s_usb_page.next_auto_check_ms == 0 ||
        !time_reached(now_ms, s_usb_page.next_auto_check_ms)) {
        return;
    }

    if (s_usb_page.task_running) {
        s_usb_page.next_auto_check_ms = now_ms + 250;
        return;
    }
    start_usb_action(UsbAction::CHECK);
}

void usb_timer_cb(lv_timer_t* /*timer*/)
{
    if (!s_usb_page.page_alive) {
        return;
    }
    const uint32_t now_ms = usb_tick_ms();

    update_camera_mode_select(now_ms);

    if (s_usb_page.active_view == UsbView::HUB) {
        run_auto_check_if_due(now_ms);
        if (s_usb_page.next_status_refresh_ms == 0 ||
            time_reached(now_ms, s_usb_page.next_status_refresh_ms)) {
            refresh_usb_status();
            s_usb_page.next_status_refresh_ms = now_ms + 500;
        }
    } else if (s_usb_page.active_view == UsbView::MOUSE_TEST) {
        refresh_mouse_test();
    } else if (s_usb_page.active_view == UsbView::KEYBOARD_TEST) {
        refresh_keyboard_test();
        update_keyboard_cursor_blink(now_ms);
    } else if (s_usb_page.active_view == UsbView::CAMERA_TEST) {
        refresh_camera_test(now_ms);
    }
}

void usb_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    close_camera_mode_select();
    cleanup_active_view();
    if (s_usb_page.timer) {
        lv_timer_delete(s_usb_page.timer);
        s_usb_page.timer = nullptr;
    }
    s_usb_page.page_alive = false;
    s_usb_page.content_root = nullptr;
    reset_hub_widget_refs();
    s_usb_page.mouse_xy_label = nullptr;
    s_usb_page.mouse_button_label = nullptr;
    s_usb_page.mouse_area = nullptr;
    s_usb_page.mouse_canvas = nullptr;
    s_usb_page.mouse_canvas_w = 0;
    s_usb_page.mouse_canvas_h = 0;
    s_usb_page.keyboard_text_label = nullptr;
    s_usb_page.keyboard_status_label = nullptr;
    s_usb_page.camera_area = nullptr;
    s_usb_page.camera_image = nullptr;
    s_usb_page.camera_status_label = nullptr;
    destroy_header_subpage_exit_button();
    s_usb_page.header_check_button = nullptr;
    s_usb_page.header_exit_button = nullptr;
}

void show_hub_page()
{
    if (!s_usb_page.content_root) {
        return;
    }
    close_camera_mode_select();
    cleanup_active_view();
    s_usb_page.active_view = UsbView::HUB;
    set_header_subpage_exit_mode(false);
    lv_obj_clean(s_usb_page.content_root);
    reset_hub_widget_refs();
    s_usb_page.mouse_xy_label = nullptr;
    s_usb_page.mouse_button_label = nullptr;
    s_usb_page.mouse_area = nullptr;
    s_usb_page.mouse_canvas = nullptr;
    s_usb_page.mouse_canvas_w = 0;
    s_usb_page.mouse_canvas_h = 0;
    s_usb_page.keyboard_text_label = nullptr;
    s_usb_page.keyboard_status_label = nullptr;
    s_usb_page.camera_area = nullptr;
    s_usb_page.camera_image = nullptr;
    s_usb_page.camera_status_label = nullptr;
    s_usb_page.next_status_refresh_ms = 0;

    lv_obj_t* hub_card = add_card(s_usb_page.content_root, USB_SUBPAGE_HEIGHT);

    lv_obj_t* title_row = add_row(hub_card, 42);
    lv_obj_t* title_label = add_label(title_row, "USB 2.0 Hub", COLOR_TEXT, 172);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_t* status_label = add_label(title_row, "Check to refresh", COLOR_MUTED, 232);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);

    lv_obj_t* chip_row = add_row(hub_card, 46);
    s_usb_page.cam_chip = create_chip(chip_row, "CAM 0", &s_usb_page.cam_chip_label);
    s_usb_page.hid_chip = create_chip(chip_row, "HID 0", &s_usb_page.hid_chip_label);
    s_usb_page.other_chip = create_chip(chip_row, "OTHER 0", &s_usb_page.other_chip_label);

    lv_obj_t* devices = lv_obj_create(hub_card);
    lv_obj_set_width(devices, lv_pct(100));
    lv_obj_set_height(devices, 238);
    lv_obj_set_style_bg_opa(devices, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(devices, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(devices, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(devices, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(devices, 7, LV_PART_MAIN);
    lv_obj_set_flex_flow(devices, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(devices, LV_SCROLLBAR_MODE_AUTO);

    s_usb_page.cam_row = lv_obj_create(devices);
    style_device_row(s_usb_page.cam_row, true);
    create_badge(s_usb_page.cam_row, "CAM", COLOR_ACCENT_CAM);
    create_text_stack(s_usb_page.cam_row, &s_usb_page.cam_title_label, &s_usb_page.cam_info_label);
    add_open_hint(s_usb_page.cam_row);
    lv_obj_add_event_cb(s_usb_page.cam_row, usb_device_open_event_cb, LV_EVENT_CLICKED,
        reinterpret_cast<void*>(static_cast<uintptr_t>(UsbDeviceOpen::CAMERA)));

    s_usb_page.mouse_row = lv_obj_create(devices);
    style_device_row(s_usb_page.mouse_row, true);
    create_badge(s_usb_page.mouse_row, "MOU", COLOR_ACCENT_MOUSE);
    create_text_stack(s_usb_page.mouse_row, &s_usb_page.mouse_title_label, &s_usb_page.mouse_info_label);
    add_open_hint(s_usb_page.mouse_row);
    lv_obj_add_event_cb(s_usb_page.mouse_row, usb_device_open_event_cb, LV_EVENT_CLICKED,
        reinterpret_cast<void*>(static_cast<uintptr_t>(UsbDeviceOpen::MOUSE)));

    s_usb_page.keyboard_row = lv_obj_create(devices);
    style_device_row(s_usb_page.keyboard_row, true);
    create_badge(s_usb_page.keyboard_row, "KEY", COLOR_ACCENT_KEYBOARD);
    create_text_stack(s_usb_page.keyboard_row, &s_usb_page.keyboard_title_label, &s_usb_page.keyboard_info_label);
    add_open_hint(s_usb_page.keyboard_row);
    lv_obj_add_event_cb(s_usb_page.keyboard_row, usb_device_open_event_cb, LV_EVENT_CLICKED,
        reinterpret_cast<void*>(static_cast<uintptr_t>(UsbDeviceOpen::KEYBOARD)));

    s_usb_page.other_row = lv_obj_create(devices);
    style_device_row(s_usb_page.other_row, false);
    create_badge(s_usb_page.other_row, "OTH", COLOR_ACCENT_OTHER);
    create_text_stack(s_usb_page.other_row, &s_usb_page.other_title_label, &s_usb_page.other_info_label);

    s_usb_page.result_label = add_label(hub_card, "", COLOR_MUTED);
    lv_label_set_long_mode(s_usb_page.result_label, LV_LABEL_LONG_DOT);
    set_visible(s_usb_page.result_label, false);

    refresh_usb_status();
}

void build_content(lv_obj_t* parent, const CoreP4FunctionWindowConfig& /*config*/)
{
    prepare_content(parent);
    s_usb_page = UsbPageState{};
    s_usb_page.content_root = parent;
    s_usb_page.page_alive = true;
    s_usb_page.uvc_started = s_uvc_host_installed;
    const uint32_t now_ms = usb_tick_ms();
    s_usb_page.next_auto_check_ms = now_ms + AUTO_CHECK_FIRST_DELAY_MS;
    s_usb_page.next_status_refresh_ms = now_ms;
    lv_obj_add_event_cb(parent, usb_content_delete_event_cb, LV_EVENT_DELETE, nullptr);

    discover_header_buttons();
    show_hub_page();
    s_usb_page.timer = lv_timer_create(usb_timer_cb, USB_TEST_REFRESH_MS, nullptr);
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "USB",
    "click on ui_func1usb1con",
    build_content,
    "Check",
    usb_check_event_cb,
    nullptr,
    COLOR_PRIMARY,
    0xFFFFFF,
};

} // namespace

extern "C" void func2usb2clicked2func(lv_event_t* e)
{
    corep4_ui_open_function_window(e, WINDOW_CONFIG);
}
