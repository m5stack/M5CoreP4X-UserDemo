#pragma once

#include "esp_lcd_touch.h"
#include "lvgl.h"

#include <cstdint>

// Convert a touch-driver point into LVGL's logical display coordinate space.
// esp_lcd_touch_get_data() has already applied the touch-controller mirror/swap
// configuration. LVGL additionally transforms pointer coordinates when the
// display itself is rotated, so direct multi-touch consumers must apply the same
// display-rotation transform as lvgl_port_touchpad_read().
inline esp_lcd_touch_point_data_t ui_touch_point_apply_display_rotation(
    const esp_lcd_touch_point_data_t& point,
    lv_display_rotation_t rotation,
    int32_t horizontal_resolution,
    int32_t vertical_resolution)
{
    esp_lcd_touch_point_data_t screen = point;
    const int32_t x = static_cast<int32_t>(point.x);
    const int32_t y = static_cast<int32_t>(point.y);

    switch (rotation) {
    case LV_DISPLAY_ROTATION_90:
        screen.x = static_cast<uint16_t>(horizontal_resolution - y - 1);
        screen.y = static_cast<uint16_t>(x);
        break;
    case LV_DISPLAY_ROTATION_180:
        screen.x = static_cast<uint16_t>(horizontal_resolution - x - 1);
        screen.y = static_cast<uint16_t>(vertical_resolution - y - 1);
        break;
    case LV_DISPLAY_ROTATION_270:
        screen.x = static_cast<uint16_t>(y);
        screen.y = static_cast<uint16_t>(vertical_resolution - x - 1);
        break;
    case LV_DISPLAY_ROTATION_0:
    default:
        break;
    }
    return screen;
}

inline esp_lcd_touch_point_data_t ui_touch_point_to_screen(
    esp_lcd_touch_handle_t touch,
    const esp_lcd_touch_point_data_t& point)
{
    (void)touch;
    lv_display_t* display = lv_display_get_default();
    if (!display) {
        return point;
    }
    return ui_touch_point_apply_display_rotation(
        point,
        lv_display_get_rotation(display),
        lv_display_get_horizontal_resolution(display),
        lv_display_get_vertical_resolution(display));
}
