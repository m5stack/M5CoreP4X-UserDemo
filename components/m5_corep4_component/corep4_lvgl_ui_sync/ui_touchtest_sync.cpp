#include "corep4_lvgl_ui_sync_internal.h"
#include "esp_heap_caps.h"

namespace {

constexpr const char* TAG = "UI_TOUCH";

lv_obj_t* s_touch_trail_canvas = nullptr;
void* s_canvas_buffer = nullptr;
lv_point_t s_last_touch_point = {-1, -1};
bool s_is_touching = false;

size_t touch_canvas_buffer_size(int32_t width, int32_t height, lv_color_format_t cf)
{
    return static_cast<size_t>(lv_draw_buf_width_to_stride(width, cf)) *
           static_cast<size_t>(height);
}

void draw_line_on_canvas(lv_obj_t* canvas,
                         int32_t x0,
                         int32_t y0,
                         int32_t x1,
                         int32_t y1,
                         lv_color_t color)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy;

    while (true) {
        lv_canvas_set_px(canvas, x0, y0, color, LV_OPA_COVER);
        lv_canvas_set_px(canvas, x0 + 1, y0, color, LV_OPA_COVER);
        lv_canvas_set_px(canvas, x0 - 1, y0, color, LV_OPA_COVER);
        lv_canvas_set_px(canvas, x0, y0 + 1, color, LV_OPA_COVER);
        lv_canvas_set_px(canvas, x0, y0 - 1, color, LV_OPA_COVER);

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

void touch_trail_event_cb(lv_event_t* e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_active();

    if (!indev || !s_touch_trail_canvas) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    if (event_code == LV_EVENT_PRESSED) {
        s_last_touch_point.x = point.x;
        s_last_touch_point.y = point.y;
        s_is_touching = true;
    } else if (event_code == LV_EVENT_PRESSING) {
        if (s_is_touching && s_last_touch_point.x >= 0 && s_last_touch_point.y >= 0) {
            int32_t dist_x = point.x - s_last_touch_point.x;
            int32_t dist_y = point.y - s_last_touch_point.y;
            int32_t dist_sq = dist_x * dist_x + dist_y * dist_y;

            if (dist_sq >= 16) {
                draw_line_on_canvas(s_touch_trail_canvas,
                                    s_last_touch_point.x, s_last_touch_point.y,
                                    point.x, point.y,
                                    lv_color_hex(0xFF0000));

                s_last_touch_point.x = point.x;
                s_last_touch_point.y = point.y;
            }
        }
    } else if (event_code == LV_EVENT_RELEASED) {
        s_is_touching = false;
        s_last_touch_point.x = -1;
        s_last_touch_point.y = -1;
    }
}

void touch_trail_canvas_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    void* buffer = lv_event_get_user_data(e);
    if (buffer) {
        heap_caps_free(buffer);
    }

    if (lv_event_get_target(e) == s_touch_trail_canvas) {
        s_touch_trail_canvas = nullptr;
        if (s_canvas_buffer == buffer) {
            s_canvas_buffer = nullptr;
        }
    }
}

} // namespace

extern "C" void touchtest2sloaded2func(lv_event_t* e)
{
    (void)e;

    s_last_touch_point.x = -1;
    s_last_touch_point.y = -1;
    s_is_touching = false;

    if (s_touch_trail_canvas != nullptr) {
        ESP_LOGI(TAG, "touchtest2sloaded2func called, reusing canvas buffer=%p", s_canvas_buffer);
        lv_canvas_fill_bg(s_touch_trail_canvas, lv_color_hex(0xFFFFFF), LV_OPA_COVER);
    } else {
        ESP_LOGI(TAG, "touchtest2sloaded2func called, creating touch trail canvas");

        s_touch_trail_canvas = lv_canvas_create(ui_ttcon);
        lv_obj_set_size(s_touch_trail_canvas, 480, 480);
        lv_obj_set_align(s_touch_trail_canvas, LV_ALIGN_CENTER);
        lv_obj_move_to_index(s_touch_trail_canvas, 0);

        lv_color_format_t cf = LV_COLOR_FORMAT_RGB565;
        size_t buf_size = touch_canvas_buffer_size(480, 480, cf);
        s_canvas_buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (s_canvas_buffer) {
            lv_canvas_set_buffer(s_touch_trail_canvas, s_canvas_buffer, 480, 480, cf);
            lv_obj_add_event_cb(s_touch_trail_canvas,
                                touch_trail_canvas_delete_event_cb,
                                LV_EVENT_DELETE,
                                s_canvas_buffer);
            lv_canvas_fill_bg(s_touch_trail_canvas, lv_color_hex(0xFFFFFF), LV_OPA_COVER);
            lv_obj_set_style_bg_color(s_touch_trail_canvas, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_touch_trail_canvas, LV_OPA_COVER, LV_PART_MAIN);
            ESP_LOGI(TAG, "Touch canvas created, buffer=%p size=%u", s_canvas_buffer, (unsigned)buf_size);
        } else {
            ESP_LOGE(TAG, "Failed to allocate touch canvas buffer from PSRAM");
            lv_obj_del(s_touch_trail_canvas);
            s_touch_trail_canvas = nullptr;
            s_canvas_buffer = nullptr;
            return;
        }
    }

    lv_obj_add_event_cb(ui_touchtest, touch_trail_event_cb, LV_EVENT_ALL, nullptr);
    ESP_LOGI(TAG, "Touch trail event registered on ui_touchtest=%p", static_cast<void*>(ui_touchtest));
}

extern "C" void touchtest2sunloaded2func(lv_event_t* e)
{
    (void)e;

    ESP_LOGI(TAG, "touchtest2sunloaded2func called, resetting touch state");

    if (ui_touchtest) {
        lv_obj_remove_event_cb(ui_touchtest, touch_trail_event_cb);
        ESP_LOGI(TAG, "Touch trail event removed from ui_touchtest");
    }

    s_last_touch_point.x = -1;
    s_last_touch_point.y = -1;
    s_is_touching = false;
}
