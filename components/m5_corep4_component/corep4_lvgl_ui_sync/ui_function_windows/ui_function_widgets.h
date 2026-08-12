#pragma once

#include "ui_function_window.h"

namespace corep4_function_ui {

constexpr uint32_t COLOR_TEXT = 0x111827;
constexpr uint32_t COLOR_MUTED = 0x64748B;
constexpr uint32_t COLOR_BORDER = 0xCBD5E1;
constexpr uint32_t COLOR_CARD_BG = 0xF8FAFC;
constexpr uint32_t COLOR_BUTTON_BG = 0xF3F4F6;
constexpr uint32_t COLOR_PRIMARY = 0x2563EB;
constexpr uint32_t COLOR_SUCCESS = 0x16A34A;
constexpr uint32_t COLOR_WARNING = 0xD97706;
constexpr uint32_t COLOR_ERROR = 0xDC2626;

inline void prepare_content(lv_obj_t* parent)
{
    if (!parent) {
        return;
    }
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_row(parent, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(parent, 8, LV_PART_MAIN);
}

inline lv_obj_t* add_label(lv_obj_t* parent,
                           const char* text,
                           uint32_t color = COLOR_TEXT,
                           int32_t width = -1)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_width(label, width < 0 ? lv_pct(100) : width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    return label;
}

inline lv_obj_t* add_card(lv_obj_t* parent, int32_t height = LV_SIZE_CONTENT)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, height);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(card, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return card;
}

inline lv_obj_t* add_row(lv_obj_t* parent, int32_t height = 34)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

inline lv_obj_t* add_value_row(lv_obj_t* parent, const char* name, const char* value)
{
    lv_obj_t* row = add_row(parent);
    lv_obj_t* name_label = add_label(row, name, COLOR_MUTED, 132);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);

    lv_obj_t* value_label = add_label(row, value, COLOR_TEXT, 280);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
    return value_label;
}

inline lv_obj_t* add_button(lv_obj_t* parent,
                            const char* text,
                            uint32_t bg_color = COLOR_BUTTON_BG,
                            uint32_t text_color = COLOR_TEXT,
                            int32_t width = 132,
                            int32_t height = 38)
{
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

inline void set_button_text(lv_obj_t* button, const char* text)
{
    if (!button) {
        return;
    }
    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (label) {
        lv_label_set_text(label, text ? text : "");
        lv_obj_center(label);
    }
}

inline void set_button_colors(lv_obj_t* button, uint32_t bg_color, uint32_t text_color)
{
    if (!button) {
        return;
    }
    lv_obj_set_style_bg_color(button, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (label) {
        lv_obj_set_style_text_color(label, lv_color_hex(text_color), LV_PART_MAIN);
    }
}

inline void set_button_enabled(lv_obj_t* button, bool enabled)
{
    if (!button) {
        return;
    }

    if (enabled) {
        lv_obj_clear_state(button, LV_STATE_DISABLED);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(button, LV_OPA_50, LV_PART_MAIN);
    }
}

inline void set_label_text(lv_obj_t* label, const char* text)
{
    if (label) {
        lv_label_set_text(label, text ? text : "");
    }
}

} // namespace corep4_function_ui
