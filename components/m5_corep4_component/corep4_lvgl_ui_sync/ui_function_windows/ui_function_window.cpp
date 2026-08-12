#include "ui_function_window.h"

namespace {

constexpr const char* TAG = "UI_FUNCTION_WINDOW";

lv_obj_t* s_active_window = nullptr;

// All window geometry is defined in absolute screen pixels.  LVGL object sizes
// are treated as half-open ranges: [left, right), [top, bottom).
constexpr int16_t SCREEN_WIDTH = 480;
constexpr int16_t SCREEN_HEIGHT = 480;
constexpr int16_t WINDOW_MARGIN = 10;
constexpr int16_t HEADER_TOP = 4;
constexpr int16_t HEADER_WIDTH = 170;
constexpr int16_t CONTENT_TOP = 52;

constexpr int16_t BORDER_WIDTH = 2;
constexpr int16_t DASH_LENGTH = 7;
constexpr int16_t DASH_GAP = 5;

constexpr int16_t CONTENT_LEFT = WINDOW_MARGIN;
constexpr int16_t CONTENT_RIGHT = SCREEN_WIDTH - WINDOW_MARGIN;
constexpr int16_t CONTENT_BOTTOM = SCREEN_HEIGHT - WINDOW_MARGIN;
constexpr int16_t CONTENT_WIDTH = CONTENT_RIGHT - CONTENT_LEFT;
constexpr int16_t CONTENT_HEIGHT = CONTENT_BOTTOM - CONTENT_TOP;
constexpr int16_t PAGED_CONTENT_HEIGHT = 370;
constexpr int16_t PAGED_TAB_LEFT = CONTENT_LEFT;
constexpr int16_t PAGED_TAB_TOP = 421;
constexpr int16_t PAGED_TAB_BORDER_Y = 422;
constexpr int16_t PAGED_TAB_WIDTH = 110;
constexpr int16_t PAGED_TAB_HEIGHT = 50;
constexpr uint8_t PAGED_WINDOW_MAX_TABS = 3;

constexpr int16_t HEADER_RIGHT = CONTENT_RIGHT;
constexpr int16_t HEADER_LEFT = HEADER_RIGHT - HEADER_WIDTH;
constexpr int16_t HEADER_BOTTOM = CONTENT_TOP;
constexpr int16_t HEADER_HEIGHT = HEADER_BOTTOM - HEADER_TOP;

constexpr int16_t HEADER_BUTTON_PAD_X = 6;
constexpr int16_t HEADER_BUTTON_GAP = 6;
constexpr int16_t HEADER_BUTTON_HEIGHT = 38;
constexpr int16_t HEADER_BUTTON_WIDTH =
    (HEADER_WIDTH - HEADER_BUTTON_PAD_X * 2 - HEADER_BUTTON_GAP) / 2;
constexpr int16_t HEADER_BUTTON_TOP = (HEADER_HEIGHT - HEADER_BUTTON_HEIGHT) / 2;
constexpr int16_t PLACEHOLDER_BUTTON_LEFT = HEADER_BUTTON_PAD_X;
constexpr int16_t EXIT_BUTTON_LEFT =
    PLACEHOLDER_BUTTON_LEFT + HEADER_BUTTON_WIDTH + HEADER_BUTTON_GAP;

constexpr int16_t WINDOW_RADIUS = 0;
constexpr int16_t BUTTON_RADIUS = 6;
constexpr int16_t CONTENT_INNER_PAD = 14;

constexpr uint32_t WINDOW_BG_COLOR = 0xFFFFFF;
constexpr uint32_t WINDOW_BORDER_COLOR = 0xCBD5E1;
constexpr uint32_t PLACEHOLDER_BG_COLOR = 0xF3F4F6;
constexpr uint32_t TAB_INACTIVE_BG_COLOR = 0xE2E8F0;
constexpr uint32_t TAB_INACTIVE_TEXT_COLOR = 0x64748B;
constexpr uint32_t TAB_ACTIVE_TEXT_COLOR = 0x2563EB;
constexpr uint32_t EXIT_BG_COLOR = 0xD32F2F;
constexpr uint32_t DIALOG_TEXT_COLOR = 0x111827;
constexpr uint32_t DIALOG_BUTTON_BORDER_COLOR = 0xCBD5E1;
constexpr uint32_t DIALOG_BORDER_COLOR = 0x475569;
constexpr uint32_t DIALOG_BACKDROP_COLOR = 0x111827;

// Window open/close animation parameters
constexpr uint32_t WINDOW_OPEN_DURATION_MS = 220;
constexpr uint32_t WINDOW_CLOSE_DURATION_MS = 180;
constexpr int32_t ANIM_PROGRESS_MAX = 1000;

constexpr int16_t DIALOG_WIDTH = CONTENT_WIDTH;
constexpr int16_t DIALOG_SMALL_HEIGHT = 176;
constexpr int16_t DIALOG_MEDIUM_HEIGHT = 248;
constexpr int16_t DIALOG_LARGE_HEIGHT = 328;
constexpr int16_t DIALOG_TEXT_WIDTH = DIALOG_WIDTH - 40;
constexpr int16_t DIALOG_BUTTON_WIDTH = 116;
constexpr int16_t DIALOG_BUTTON_HEIGHT = 40;

struct WindowAnimCtx {
    lv_obj_t* window;
    int16_t from_x, from_y, from_w, from_h;
    int16_t to_x, to_y, to_w, to_h;
    bool is_opening;
};

WindowAnimCtx* s_window_anim_ctx = nullptr;
lv_obj_t* s_active_dialog = nullptr;

static_assert(CONTENT_LEFT == 10);
static_assert(CONTENT_TOP == 52);
static_assert(CONTENT_RIGHT == 470);
static_assert(CONTENT_BOTTOM == 470);
static_assert(CONTENT_WIDTH == 460);
static_assert(CONTENT_HEIGHT == 418);
static_assert(HEADER_LEFT == 300);
static_assert(HEADER_RIGHT == CONTENT_RIGHT);
static_assert(HEADER_BOTTOM == CONTENT_TOP);
static_assert(HEADER_HEIGHT == 48);
static_assert(HEADER_BUTTON_WIDTH == 76);
static_assert(EXIT_BUTTON_LEFT + HEADER_BUTTON_WIDTH + HEADER_BUTTON_PAD_X == HEADER_WIDTH);

struct DashState {
    int16_t remaining = DASH_LENGTH;
    bool drawing = true;
};

int16_t abs_i16(int16_t value)
{
    return value < 0 ? static_cast<int16_t>(-value) : value;
}

int16_t min_i16(int16_t a, int16_t b)
{
    return a < b ? a : b;
}

void reset_obj_defaults(lv_obj_t* obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
}

void set_manual_positioning(lv_obj_t* obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_FLOATING);
}

void active_window_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_DELETE &&
        lv_event_get_target(e) == s_active_window) {
        s_active_window = nullptr;
    }
}

void get_trigger_center(lv_event_t* e, int16_t* out_x, int16_t* out_y)
{
    if (e) {
        lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
        if (target) {
            lv_area_t coords;
            lv_obj_get_coords(target, &coords);
            *out_x = (coords.x1 + coords.x2) / 2;
            *out_y = (coords.y1 + coords.y2) / 2;
            return;
        }
    }
    *out_x = SCREEN_WIDTH / 2;
    *out_y = SCREEN_HEIGHT / 2;
}

void window_anim_exec_cb(void* var, int32_t v)
{
    auto* ctx = static_cast<WindowAnimCtx*>(var);
    if (!ctx || !ctx->window) {
        return;
    }

    float t = static_cast<float>(v) / ANIM_PROGRESS_MAX;

    int16_t x = static_cast<int16_t>(ctx->from_x + (ctx->to_x - ctx->from_x) * t);
    int16_t y = static_cast<int16_t>(ctx->from_y + (ctx->to_y - ctx->from_y) * t);
    int16_t w = static_cast<int16_t>(ctx->from_w + (ctx->to_w - ctx->from_w) * t);
    int16_t h = static_cast<int16_t>(ctx->from_h + (ctx->to_h - ctx->from_h) * t);

    if (w < 1) w = 1;
    if (h < 1) h = 1;

    lv_obj_set_pos(ctx->window, x, y);
    lv_obj_set_size(ctx->window, w, h);
    const int32_t opa_val = ctx->is_opening
        ? static_cast<int32_t>(t * 255.0f)
        : static_cast<int32_t>((1.0f - t) * 255.0f);
    lv_obj_set_style_opa(ctx->window, static_cast<lv_opa_t>(opa_val), LV_PART_MAIN);
}

void window_anim_completed_cb(lv_anim_t* a)
{
    // 仅处理关闭动画的窗口对象删除；ctx 的生命周期统一由 deleted_cb 管理
    auto* ctx = static_cast<WindowAnimCtx*>(a->var);
    if (!ctx) {
        return;
    }

    if (!ctx->is_opening && ctx->window) {
        lv_obj_delete(ctx->window);
        ctx->window = nullptr;  // 标记已删除，防止 deleted_cb 重复删除
    }
    // 注意：不在此处 delete ctx，LVGL 完成后还会调用 deleted_cb，统一在那里释放
}

void window_anim_deleted_cb(lv_anim_t* a)
{
    // 无论动画正常结束还是被取消，此回调都是唯一的清理点
    auto* ctx = static_cast<WindowAnimCtx*>(a->var);
    if (!ctx) {
        return;
    }

    // 动画被中途取消时 completed_cb 不会执行，需在此兜底删除窗口
    if (!ctx->is_opening && ctx->window) {
        lv_obj_delete(ctx->window);
        ctx->window = nullptr;
    }

    if (s_window_anim_ctx == ctx) {
        s_window_anim_ctx = nullptr;
    }
    delete ctx;
}

lv_obj_t* create_dash_segment(lv_obj_t* parent, int16_t x, int16_t y,
                              int16_t width, int16_t height)
{
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    lv_obj_t* segment = lv_obj_create(parent);
    reset_obj_defaults(segment);
    set_manual_positioning(segment);
    lv_obj_remove_flag(segment, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(segment, x, y);
    lv_obj_set_size(segment, width, height);
    lv_obj_set_style_radius(segment, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(segment, lv_color_hex(WINDOW_BORDER_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, LV_PART_MAIN);
    return segment;
}

void step_dash_state(DashState& dash)
{
    dash.drawing = !dash.drawing;
    dash.remaining = dash.drawing ? DASH_LENGTH : DASH_GAP;
}

void add_dashed_axis_edge(lv_obj_t* parent,
                          int16_t x1, int16_t y1,
                          int16_t x2, int16_t y2,
                          int16_t draw_offset_x, int16_t draw_offset_y,
                          DashState& dash)
{
    const bool horizontal = y1 == y2;
    const int16_t length = horizontal ? abs_i16(static_cast<int16_t>(x2 - x1))
                                      : abs_i16(static_cast<int16_t>(y2 - y1));
    if (length <= 0) {
        return;
    }

    const int16_t dir = (horizontal ? x2 - x1 : y2 - y1) > 0 ? 1 : -1;
    int16_t cursor_x = x1;
    int16_t cursor_y = y1;
    int16_t remaining = length;

    while (remaining > 0) {
        const int16_t run = dash.remaining < remaining ? dash.remaining : remaining;
        const int16_t next_x = horizontal ? static_cast<int16_t>(cursor_x + dir * run)
                                          : cursor_x;
        const int16_t next_y = horizontal ? cursor_y
                                          : static_cast<int16_t>(cursor_y + dir * run);

        if (dash.drawing) {
            if (horizontal) {
                create_dash_segment(parent,
                                    static_cast<int16_t>(min_i16(cursor_x, next_x) + draw_offset_x),
                                    static_cast<int16_t>(y1 + draw_offset_y),
                                    run,
                                    BORDER_WIDTH);
            } else {
                create_dash_segment(parent,
                                    static_cast<int16_t>(x1 + draw_offset_x),
                                    static_cast<int16_t>(min_i16(cursor_y, next_y) + draw_offset_y),
                                    BORDER_WIDTH,
                                    run);
            }
        }

        cursor_x = next_x;
        cursor_y = next_y;
        remaining = static_cast<int16_t>(remaining - run);
        dash.remaining = static_cast<int16_t>(dash.remaining - run);
        if (dash.remaining == 0) {
            step_dash_state(dash);
        }
    }
}

void add_dashed_rect_outline(lv_obj_t* parent,
                             int16_t left, int16_t top,
                             int16_t right, int16_t bottom)
{
    DashState dash;
    add_dashed_axis_edge(parent, left, top, right, top, 0, 0, dash);
    add_dashed_axis_edge(parent, right, top, right, bottom,
                         -BORDER_WIDTH, 0, dash);
    add_dashed_axis_edge(parent, right, bottom, left, bottom,
                         0, -BORDER_WIDTH, dash);
    add_dashed_axis_edge(parent, left, bottom, left, top, 0, 0, dash);
}

void add_function_window_outline(lv_obj_t* parent)
{
    DashState dash;

    add_dashed_axis_edge(parent, HEADER_LEFT, HEADER_TOP,
                         HEADER_RIGHT, HEADER_TOP,
                         0, 0, dash);
    add_dashed_axis_edge(parent, HEADER_RIGHT, HEADER_TOP,
                         CONTENT_RIGHT, CONTENT_BOTTOM,
                         -BORDER_WIDTH, 0, dash);
    add_dashed_axis_edge(parent, CONTENT_RIGHT, CONTENT_BOTTOM,
                         CONTENT_LEFT, CONTENT_BOTTOM,
                         0, -BORDER_WIDTH, dash);
    add_dashed_axis_edge(parent, CONTENT_LEFT, CONTENT_BOTTOM,
                         CONTENT_LEFT, CONTENT_TOP,
                         0, 0, dash);
    add_dashed_axis_edge(parent, CONTENT_LEFT, CONTENT_TOP,
                         HEADER_LEFT, CONTENT_TOP,
                         0, 0, dash);
    add_dashed_axis_edge(parent, HEADER_LEFT, CONTENT_TOP,
                         HEADER_LEFT, HEADER_TOP,
                         0, 0, dash);
}

void exit_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    auto* window = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    if (!window) {
        return;
    }

    // Get exit button screen coords as closing animation target
    lv_obj_t* exit_btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int16_t target_cx, target_cy;
    if (exit_btn) {
        lv_area_t btn_coords;
        lv_obj_get_coords(exit_btn, &btn_coords);
        target_cx = (btn_coords.x1 + btn_coords.x2) / 2;
        target_cy = (btn_coords.y1 + btn_coords.y2) / 2;
    } else {
        target_cx = SCREEN_WIDTH;
        target_cy = 0;
    }

    // Cancel any existing window animation
    if (s_window_anim_ctx) {
        s_window_anim_ctx->window = nullptr;
        lv_anim_delete(s_window_anim_ctx, window_anim_exec_cb);
        // s_window_anim_ctx is cleared by deleted_cb
    }

    // Create close animation context
    auto* ctx = new WindowAnimCtx();
    ctx->window = window;
    ctx->from_x = lv_obj_get_x(window);
    ctx->from_y = lv_obj_get_y(window);
    ctx->from_w = lv_obj_get_width(window);
    ctx->from_h = lv_obj_get_height(window);
    ctx->to_x = target_cx;
    ctx->to_y = target_cy;
    ctx->to_w = 1;
    ctx->to_h = 1;
    ctx->is_opening = false;

    s_window_anim_ctx = ctx;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctx);
    lv_anim_set_exec_cb(&a, window_anim_exec_cb);
    lv_anim_set_values(&a, 0, ANIM_PROGRESS_MAX);
    lv_anim_set_duration(&a, WINDOW_CLOSE_DURATION_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, window_anim_completed_cb);
    lv_anim_set_deleted_cb(&a, window_anim_deleted_cb);
    lv_anim_start(&a);

    ESP_LOGI(TAG, "Closing window (anim to %d,%d)", target_cx, target_cy);
}

lv_obj_t* create_backdrop()
{
    lv_obj_t* backdrop = lv_obj_create(lv_screen_active());
    reset_obj_defaults(backdrop);
    set_manual_positioning(backdrop);
    lv_obj_set_pos(backdrop, 0, 0);
    lv_obj_set_size(backdrop, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    // 阻止手势事件冒泡到父级 Screen，避免弹出功能窗口时滑动导致退出页面
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_GESTURE_BUBBLE);
    // 动画展开时裁剪超出 backdrop 边界的子对象，防止边缘控件提前闪现
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(backdrop, active_window_delete_event_cb, LV_EVENT_DELETE, nullptr);
    return backdrop;
}

void apply_panel_fill_style(lv_obj_t* obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(WINDOW_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, WINDOW_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
}

lv_obj_t* create_header(lv_obj_t* parent)
{
    lv_obj_t* header = lv_obj_create(parent);
    reset_obj_defaults(header);
    set_manual_positioning(header);
    lv_obj_set_pos(header, HEADER_LEFT, HEADER_TOP);
    lv_obj_set_size(header, HEADER_WIDTH, HEADER_HEIGHT);
    apply_panel_fill_style(header);
    return header;
}

lv_obj_t* create_placeholder_button(lv_obj_t* parent)
{
    lv_obj_t* button = lv_obj_create(parent);
    reset_obj_defaults(button);
    set_manual_positioning(button);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(button, PLACEHOLDER_BUTTON_LEFT, HEADER_BUTTON_TOP);
    lv_obj_set_size(button, HEADER_BUTTON_WIDTH, HEADER_BUTTON_HEIGHT);
    lv_obj_set_style_bg_color(button, lv_color_hex(PLACEHOLDER_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, BUTTON_RADIUS, LV_PART_MAIN);
    add_dashed_rect_outline(button, 0, 0, HEADER_BUTTON_WIDTH, HEADER_BUTTON_HEIGHT);
    return button;
}

lv_obj_t* create_header_action_button(lv_obj_t* parent,
                                      const CoreP4FunctionWindowConfig& config)
{
    lv_obj_t* button = lv_button_create(parent);
    reset_obj_defaults(button);
    set_manual_positioning(button);
    lv_obj_set_pos(button, PLACEHOLDER_BUTTON_LEFT, HEADER_BUTTON_TOP);
    lv_obj_set_size(button, HEADER_BUTTON_WIDTH, HEADER_BUTTON_HEIGHT);
    lv_obj_set_style_bg_color(button, lv_color_hex(config.header_action_bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, BUTTON_RADIUS, LV_PART_MAIN);
    lv_obj_add_event_cb(button, config.header_action_callback, LV_EVENT_CLICKED,
                        config.header_action_user_data);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, config.header_action_text ? config.header_action_text : "Action");
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_hex(config.header_action_text_color), LV_PART_MAIN);
    if (config.header_action_initially_hidden) {
        lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    }
    if (config.header_action_ready_callback) {
        config.header_action_ready_callback(button);
    }
    return button;
}

lv_obj_t* create_exit_button(lv_obj_t* parent,
                             lv_obj_t* window,
                             const CoreP4FunctionWindowConfig* config = nullptr)
{
    lv_obj_t* button = lv_button_create(parent);
    reset_obj_defaults(button);
    set_manual_positioning(button);
    lv_obj_set_pos(button, EXIT_BUTTON_LEFT, HEADER_BUTTON_TOP);
    lv_obj_set_size(button, HEADER_BUTTON_WIDTH, HEADER_BUTTON_HEIGHT);
    lv_obj_set_style_bg_color(button, lv_color_hex(EXIT_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, BUTTON_RADIUS, LV_PART_MAIN);
    if (config && config->exit_callback) {
        lv_obj_add_event_cb(button,
                            config->exit_callback,
                            LV_EVENT_CLICKED,
                            config->exit_user_data);
    } else {
        lv_obj_add_event_cb(button, exit_button_event_cb, LV_EVENT_CLICKED, window);
    }

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Exit");
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    if (config && config->exit_button_ready_callback) {
        config->exit_button_ready_callback(button);
    }

    return button;
}

lv_obj_t* create_content(lv_obj_t* parent)
{
    lv_obj_t* content = lv_obj_create(parent);
    reset_obj_defaults(content);
    set_manual_positioning(content);
    lv_obj_set_pos(content, CONTENT_LEFT, CONTENT_TOP);
    lv_obj_set_size(content, CONTENT_WIDTH, CONTENT_HEIGHT);
    apply_panel_fill_style(content);
    lv_obj_set_style_pad_all(content, CONTENT_INNER_PAD, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return content;
}

int16_t dialog_height(CoreP4FunctionDialogSize size)
{
    switch (size) {
        case CoreP4FunctionDialogSize::SMALL: return DIALOG_SMALL_HEIGHT;
        case CoreP4FunctionDialogSize::MEDIUM: return DIALOG_MEDIUM_HEIGHT;
        case CoreP4FunctionDialogSize::LARGE: return DIALOG_LARGE_HEIGHT;
        default: return DIALOG_MEDIUM_HEIGHT;
    }
}

void active_dialog_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_DELETE &&
        lv_event_get_target(e) == s_active_dialog) {
        s_active_dialog = nullptr;
    }
}

void style_dialog_button(lv_obj_t* button, uint32_t bg_color, uint32_t text_color)
{
    if (!button) {
        return;
    }

    lv_obj_set_size(button, DIALOG_BUTTON_WIDTH, DIALOG_BUTTON_HEIGHT);
    lv_obj_set_style_bg_color(button, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(DIALOG_BUTTON_BORDER_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, BUTTON_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);

    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (label) {
        lv_obj_set_style_text_color(label, lv_color_hex(text_color), LV_PART_MAIN);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_center(label);
    }
}

void style_dialog(CoreP4FunctionDialogHandle& handle,
                  CoreP4FunctionDialogSize size)
{
    if (!handle.box) {
        return;
    }

    lv_obj_set_size(handle.box, DIALOG_WIDTH, dialog_height(size));
    lv_obj_center(handle.box);
    lv_obj_set_style_radius(handle.box, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(handle.box, lv_color_hex(WINDOW_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(handle.box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(handle.box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(handle.box, lv_color_hex(DIALOG_BORDER_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_opa(handle.box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(handle.box, 0, LV_PART_MAIN);
    // 阻止 Dialog 自身的手势冒泡，防止滑动误退
    lv_obj_remove_flag(handle.box, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t* backdrop = lv_obj_get_parent(handle.box);
    if (backdrop) {
        lv_obj_set_style_bg_color(backdrop, lv_color_hex(DIALOG_BACKDROP_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(backdrop, LV_OPA_50, LV_PART_MAIN);
        // 阻止弹窗遮罩层的手势冒泡
        lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }

    lv_obj_t* content = lv_msgbox_get_content(handle.box);
    if (content) {
        lv_obj_set_width(content, lv_pct(100));
        lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    }

    lv_obj_t* footer = lv_msgbox_get_footer(handle.box);
    if (footer) {
        lv_obj_set_style_pad_column(footer, 10, LV_PART_MAIN);
    }
}

struct PagedWindowCtx {
    lv_obj_t* pages[PAGED_WINDOW_MAX_TABS] = {};
    lv_obj_t* tabs[PAGED_WINDOW_MAX_TABS] = {};
    lv_obj_t* tab_labels[PAGED_WINDOW_MAX_TABS] = {};
    lv_obj_t* tab_extension = nullptr;
    lv_obj_t* border_container;
    uint8_t page_count = 0;
    uint8_t active_tab = 0;
};

uint8_t paged_window_page_count(const CoreP4FunctionPagedWindowConfig& config)
{
    return (config.tab3_text || config.build_tab3_content) ? 3 : 2;
}

int16_t paged_tab_x(uint8_t index)
{
    return static_cast<int16_t>(PAGED_TAB_LEFT + index * PAGED_TAB_WIDTH);
}

int16_t paged_tab_right(uint8_t index, uint8_t page_count)
{
    if (index + 1 >= page_count) {
        return CONTENT_RIGHT;
    }
    return paged_tab_x(static_cast<uint8_t>(index + 1));
}

lv_obj_t* create_paged_page(lv_obj_t* parent)
{
    lv_obj_t* page = lv_obj_create(parent);
    reset_obj_defaults(page);
    lv_obj_set_size(page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return page;
}

lv_obj_t* create_paged_tab(lv_obj_t* parent,
                           const char* text,
                           const char* fallback_text,
                           uint8_t index)
{
    lv_obj_t* tab = lv_button_create(parent);
    reset_obj_defaults(tab);
    set_manual_positioning(tab);
    lv_obj_set_pos(tab, paged_tab_x(index), PAGED_TAB_TOP);
    lv_obj_set_size(tab, PAGED_TAB_WIDTH, PAGED_TAB_HEIGHT);

    lv_obj_t* label = lv_label_create(tab);
    lv_label_set_text(label, text ? text : fallback_text);
    lv_obj_set_width(label, PAGED_TAB_WIDTH - 8);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_center(label);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    return tab;
}

void update_paged_window_view(PagedWindowCtx* ctx)
{
    if (!ctx) {
        return;
    }

    for (uint8_t i = 0; i < ctx->page_count; ++i) {
        if (ctx->pages[i]) {
            if (ctx->active_tab == i) {
                lv_obj_remove_flag(ctx->pages[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(ctx->pages[i], LV_OBJ_FLAG_HIDDEN);
            }
        }

        if (ctx->tabs[i]) {
            const bool active = (ctx->active_tab == i);
            lv_obj_set_style_bg_color(ctx->tabs[i],
                                      lv_color_hex(active ? WINDOW_BG_COLOR
                                                         : TAB_INACTIVE_BG_COLOR),
                                      LV_PART_MAIN);
            lv_obj_set_style_bg_opa(ctx->tabs[i], LV_OPA_COVER, LV_PART_MAIN);
            if (ctx->tab_labels[i]) {
                lv_obj_set_style_text_color(ctx->tab_labels[i],
                                            lv_color_hex(active ? TAB_ACTIVE_TEXT_COLOR
                                                               : TAB_INACTIVE_TEXT_COLOR),
                                            LV_PART_MAIN);
            }
        }
    }

    if (ctx->tab_extension) {
        const bool last_tab_active = (ctx->active_tab + 1 == ctx->page_count);
        lv_obj_set_style_bg_color(ctx->tab_extension,
                                  lv_color_hex(last_tab_active ? WINDOW_BG_COLOR
                                                               : TAB_INACTIVE_BG_COLOR),
                                  LV_PART_MAIN);
    }

    lv_obj_clean(ctx->border_container);

    // Tab 顶边线 (仅对未激活页签绘制)
    for (uint8_t i = 0; i < ctx->page_count; ++i) {
        if (ctx->active_tab != i) {
            DashState dash_h;
            add_dashed_axis_edge(ctx->border_container,
                                 paged_tab_x(i),
                                 PAGED_TAB_BORDER_Y,
                                 paged_tab_right(i, ctx->page_count),
                                 PAGED_TAB_BORDER_Y,
                                 0,
                                 0,
                                 dash_h);
        }
    }
}

void paged_tab_click_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto* ctx = static_cast<PagedWindowCtx*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!ctx || !target) {
        return;
    }

    for (uint8_t i = 0; i < ctx->page_count; ++i) {
        if (ctx->tabs[i] == target && ctx->active_tab != i) {
            ctx->active_tab = i;
            update_paged_window_view(ctx);
            return;
        }
    }
}

void paged_window_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        auto* ctx = static_cast<PagedWindowCtx*>(lv_event_get_user_data(e));
        if (ctx) {
            delete ctx;
        }
        if (lv_event_get_target(e) == s_active_window) {
            s_active_window = nullptr;
        }
    }
}

} // namespace

void corep4_ui_add_default_function_label(lv_obj_t* parent, const char* text)
{
    if (!parent) {
        return;
    }

    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "Function triggered");
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(0x111827), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
}

CoreP4FunctionDialogHandle corep4_ui_open_function_dialog(
    const char* title,
    const char* message,
    CoreP4FunctionDialogSize size,
    const CoreP4FunctionDialogButtonConfig* buttons,
    size_t button_count)
{
    CoreP4FunctionDialogHandle handle = {};

    if (s_active_dialog) {
        lv_msgbox_close(s_active_dialog);
        s_active_dialog = nullptr;
    }

    if (button_count > COREP4_FUNCTION_DIALOG_MAX_BUTTONS) {
        button_count = COREP4_FUNCTION_DIALOG_MAX_BUTTONS;
    }
    if (!buttons) {
        button_count = 0;
    }

    handle.box = lv_msgbox_create(nullptr);
    handle.button_count = static_cast<uint8_t>(button_count);
    s_active_dialog = handle.box;
    lv_obj_add_event_cb(handle.box, active_dialog_delete_event_cb, LV_EVENT_DELETE, nullptr);

    lv_obj_t* title_label = lv_msgbox_add_title(handle.box, title ? title : "");
    if (title_label) {
        lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(title_label, lv_color_hex(DIALOG_TEXT_COLOR), LV_PART_MAIN);
    }

    handle.text_label = lv_msgbox_add_text(handle.box, message ? message : "");
    if (handle.text_label) {
        lv_obj_set_width(handle.text_label, DIALOG_TEXT_WIDTH);
        lv_label_set_long_mode(handle.text_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(handle.text_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(handle.text_label, lv_color_hex(DIALOG_TEXT_COLOR), LV_PART_MAIN);
    }

    for (size_t i = 0; i < button_count; ++i) {
        const auto& button_config = buttons[i];
        lv_obj_t* button = lv_msgbox_add_footer_button(handle.box,
                                                       button_config.text ? button_config.text : "");
        handle.buttons[i] = button;
        style_dialog_button(button, button_config.bg_color, button_config.text_color);
        if (button_config.callback) {
            lv_obj_add_event_cb(button, button_config.callback,
                                LV_EVENT_CLICKED, button_config.user_data);
        }
        if (button_config.hidden) {
            lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
        }
    }

    style_dialog(handle, size);
    lv_obj_move_foreground(handle.box);
    lv_obj_center(handle.box);
    return handle;
}

void corep4_ui_close_function_dialog(CoreP4FunctionDialogHandle* dialog)
{
    if (!dialog) {
        return;
    }

    if (dialog->box) {
        if (s_active_dialog == dialog->box) {
            s_active_dialog = nullptr;
        }
        lv_msgbox_close(dialog->box);
    }

    *dialog = {};
}

void corep4_ui_set_function_dialog_text(CoreP4FunctionDialogHandle* dialog,
                                        const char* text)
{
    if (!dialog || !dialog->text_label) {
        return;
    }

    lv_label_set_text(dialog->text_label, text ? text : "");
}

void corep4_ui_set_function_dialog_button_hidden(CoreP4FunctionDialogHandle* dialog,
                                                 uint8_t index,
                                                 bool hidden)
{
    if (!dialog || index >= dialog->button_count || !dialog->buttons[index]) {
        return;
    }

    if (hidden) {
        lv_obj_add_flag(dialog->buttons[index], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(dialog->buttons[index], LV_OBJ_FLAG_HIDDEN);
    }
}

void corep4_ui_open_function_window(lv_event_t* e,
                                    const CoreP4FunctionWindowConfig& config)
{
    if (e && lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    // Force-delete any existing window and cancel its animation
    if (s_active_window) {
        if (s_window_anim_ctx) {
            s_window_anim_ctx->window = nullptr;
            lv_anim_delete(s_window_anim_ctx, window_anim_exec_cb);
            // s_window_anim_ctx is cleared by deleted_cb
        }
        lv_obj_delete(s_active_window);
        s_active_window = nullptr;
    }

    // Determine animation origin: center of the clicked trigger object
    int16_t origin_x, origin_y;
    get_trigger_center(e, &origin_x, &origin_y);

    // Create backdrop and set initial state: 1x1 at trigger center, fully transparent
    lv_obj_t* backdrop = create_backdrop();
    s_active_window = backdrop;
    lv_obj_set_pos(backdrop, origin_x, origin_y);
    lv_obj_set_size(backdrop, 1, 1);
    lv_obj_set_style_opa(backdrop, LV_OPA_TRANSP, LV_PART_MAIN);

    // Build window contents at final relative positions (clipped until backdrop expands)
    lv_obj_t* content = create_content(backdrop);
    lv_obj_t* header = create_header(backdrop);
    if (config.header_action_text && config.header_action_callback) {
        create_header_action_button(header, config);
    } else {
        create_placeholder_button(header);
    }
    create_exit_button(header, backdrop, &config);
    add_function_window_outline(backdrop);

    if (config.build_content) {
        config.build_content(content, config);
    } else {
        corep4_ui_add_default_function_label(content, config.trigger_name);
    }

    // Create open animation context
    auto* ctx = new WindowAnimCtx();
    ctx->window = backdrop;
    ctx->from_x = origin_x;
    ctx->from_y = origin_y;
    ctx->from_w = 1;
    ctx->from_h = 1;
    ctx->to_x = 0;
    ctx->to_y = 0;
    ctx->to_w = SCREEN_WIDTH;
    ctx->to_h = SCREEN_HEIGHT;
    ctx->is_opening = true;

    s_window_anim_ctx = ctx;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctx);
    lv_anim_set_exec_cb(&a, window_anim_exec_cb);
    lv_anim_set_values(&a, 0, ANIM_PROGRESS_MAX);
    lv_anim_set_duration(&a, WINDOW_OPEN_DURATION_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, window_anim_completed_cb);
    lv_anim_set_deleted_cb(&a, window_anim_deleted_cb);
    lv_anim_start(&a);

    ESP_LOGI(TAG, "Opened %s window from %s (origin %d,%d)",
             config.title ? config.title : "Function",
             config.trigger_name ? config.trigger_name : "(unknown)",
             origin_x, origin_y);
}

void corep4_ui_open_paged_function_window(lv_event_t* e,
                                          const CoreP4FunctionPagedWindowConfig& config)
{
    if (e && lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    if (s_active_window) {
        if (s_window_anim_ctx) {
            s_window_anim_ctx->window = nullptr;
            lv_anim_delete(s_window_anim_ctx, window_anim_exec_cb);
        }
        lv_obj_delete(s_active_window);
        s_active_window = nullptr;
    }

    int16_t origin_x, origin_y;
    get_trigger_center(e, &origin_x, &origin_y);

    lv_obj_t* backdrop = create_backdrop();
    s_active_window = backdrop;
    lv_obj_set_pos(backdrop, origin_x, origin_y);
    lv_obj_set_size(backdrop, 1, 1);
    lv_obj_set_style_opa(backdrop, LV_OPA_TRANSP, LV_PART_MAIN);

    const uint8_t page_count = paged_window_page_count(config);

    lv_obj_t* paged_content = lv_obj_create(backdrop);
    reset_obj_defaults(paged_content);
    set_manual_positioning(paged_content);
    lv_obj_set_pos(paged_content, CONTENT_LEFT, CONTENT_TOP);
    lv_obj_set_size(paged_content, CONTENT_WIDTH, PAGED_CONTENT_HEIGHT);
    apply_panel_fill_style(paged_content);
    lv_obj_set_style_pad_all(paged_content, CONTENT_INNER_PAD, LV_PART_MAIN);

    lv_obj_t* tab_extension = lv_obj_create(backdrop);
    reset_obj_defaults(tab_extension);
    set_manual_positioning(tab_extension);
    lv_obj_set_pos(tab_extension, paged_tab_x(page_count), PAGED_TAB_BORDER_Y);
    lv_obj_set_size(tab_extension,
                    static_cast<int16_t>(CONTENT_RIGHT - paged_tab_x(page_count)),
                    static_cast<int16_t>(CONTENT_BOTTOM - PAGED_TAB_BORDER_Y));
    apply_panel_fill_style(tab_extension);

    // 静态虚线框容器，只在初始化时绘制一次
    lv_obj_t* static_border_container = lv_obj_create(backdrop);
    reset_obj_defaults(static_border_container);
    set_manual_positioning(static_border_container);
    lv_obj_set_pos(static_border_container, 0, 0);
    lv_obj_set_size(static_border_container, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_remove_flag(static_border_container, LV_OBJ_FLAG_CLICKABLE);

    DashState dash;
    // 1. Header 顶边: (300, 4) -> (470, 4)
    add_dashed_axis_edge(static_border_container, HEADER_LEFT, HEADER_TOP, HEADER_RIGHT, HEADER_TOP, 0, 0, dash);
    // 2. 右侧垂直边: (470, 4) -> (470, 470)
    add_dashed_axis_edge(static_border_container, HEADER_RIGHT, HEADER_TOP, CONTENT_RIGHT, CONTENT_BOTTOM, -BORDER_WIDTH, 0, dash);
    // 3. 底部横边: (470, 470) -> (10, 470)
    add_dashed_axis_edge(static_border_container, CONTENT_RIGHT, CONTENT_BOTTOM, CONTENT_LEFT, CONTENT_BOTTOM, 0, -BORDER_WIDTH, dash);
    // 4. 左侧垂直边: (10, 470) -> (10, 52)
    add_dashed_axis_edge(static_border_container, CONTENT_LEFT, CONTENT_BOTTOM, CONTENT_LEFT, CONTENT_TOP, 0, 0, dash);
    // 5. 左上横边: (10, 52) -> (300, 52)
    add_dashed_axis_edge(static_border_container, CONTENT_LEFT, CONTENT_TOP, HEADER_LEFT, CONTENT_TOP, 0, 0, dash);
    // 6. Header 左垂直边: (300, 52) -> (300, 4)
    add_dashed_axis_edge(static_border_container, HEADER_LEFT, CONTENT_TOP, HEADER_LEFT, HEADER_TOP, 0, 0, dash);

    // 内部垂直分割线
    for (uint8_t i = 1; i < page_count; ++i) {
        DashState dash_v;
        add_dashed_axis_edge(static_border_container,
                             paged_tab_x(i),
                             PAGED_TAB_BORDER_Y,
                             paged_tab_x(i),
                             CONTENT_BOTTOM,
                             0,
                             0,
                             dash_v);
    }

    // 动态虚线框容器，只包含随 Tab 改变的顶边水平线
    lv_obj_t* border_container = lv_obj_create(backdrop);
    reset_obj_defaults(border_container);
    set_manual_positioning(border_container);
    lv_obj_set_pos(border_container, 0, 0);
    lv_obj_set_size(border_container, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_remove_flag(border_container, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* header = create_header(backdrop);
    if (config.header_action_text && config.header_action_callback) {
        lv_obj_t* button = lv_button_create(header);
        reset_obj_defaults(button);
        set_manual_positioning(button);
        lv_obj_set_pos(button, PLACEHOLDER_BUTTON_LEFT, HEADER_BUTTON_TOP);
        lv_obj_set_size(button, HEADER_BUTTON_WIDTH, HEADER_BUTTON_HEIGHT);
        lv_obj_set_style_bg_color(button, lv_color_hex(config.header_action_bg_color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(button, BUTTON_RADIUS, LV_PART_MAIN);
        lv_obj_add_event_cb(button, config.header_action_callback, LV_EVENT_CLICKED,
                            config.header_action_user_data);

        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, config.header_action_text ? config.header_action_text : "Action");
        lv_obj_center(label);
        lv_obj_set_style_text_color(label, lv_color_hex(config.header_action_text_color), LV_PART_MAIN);
        if (config.header_action_initially_hidden) {
            lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
        }
        if (config.header_action_ready_callback) {
            config.header_action_ready_callback(button);
        }
    } else {
        create_placeholder_button(header);
    }
    create_exit_button(header, backdrop);

    lv_obj_t* pages[PAGED_WINDOW_MAX_TABS] = {};
    for (uint8_t i = 0; i < page_count; ++i) {
        pages[i] = create_paged_page(paged_content);
    }

    if (config.build_tab1_content) {
        config.build_tab1_content(pages[0], config);
    }
    if (config.build_tab2_content) {
        config.build_tab2_content(pages[1], config);
    }
    if (page_count > 2 && config.build_tab3_content) {
        config.build_tab3_content(pages[2], config);
    }

    const char* tab_texts[PAGED_WINDOW_MAX_TABS] = {
        config.tab1_text,
        config.tab2_text,
        config.tab3_text,
    };
    const char* tab_fallbacks[PAGED_WINDOW_MAX_TABS] = {
        "Page 1",
        "Page 2",
        "Page 3",
    };

    lv_obj_t* tabs[PAGED_WINDOW_MAX_TABS] = {};
    lv_obj_t* tab_labels[PAGED_WINDOW_MAX_TABS] = {};
    for (uint8_t i = 0; i < page_count; ++i) {
        tabs[i] = create_paged_tab(backdrop, tab_texts[i], tab_fallbacks[i], i);
        tab_labels[i] = lv_obj_get_child(tabs[i], 0);
    }

    auto* ctx = new PagedWindowCtx();
    ctx->tab_extension = tab_extension;
    ctx->border_container = border_container;
    ctx->page_count = page_count;
    for (uint8_t i = 0; i < page_count; ++i) {
        ctx->pages[i] = pages[i];
        ctx->tabs[i] = tabs[i];
        ctx->tab_labels[i] = tab_labels[i];
    }

    lv_obj_add_event_cb(backdrop, paged_window_delete_event_cb, LV_EVENT_DELETE, ctx);
    for (uint8_t i = 0; i < page_count; ++i) {
        lv_obj_add_event_cb(tabs[i], paged_tab_click_event_cb, LV_EVENT_CLICKED, ctx);
    }

    // 将静态虚线容器和动态虚线容器移动到最前层，确保虚线不会被 Tab 按钮或内容背景遮挡
    lv_obj_move_foreground(static_border_container);
    lv_obj_move_foreground(border_container);

    update_paged_window_view(ctx);

    auto* anim_ctx = new WindowAnimCtx();
    anim_ctx->window = backdrop;
    anim_ctx->from_x = origin_x;
    anim_ctx->from_y = origin_y;
    anim_ctx->from_w = 1;
    anim_ctx->from_h = 1;
    anim_ctx->to_x = 0;
    anim_ctx->to_y = 0;
    anim_ctx->to_w = SCREEN_WIDTH;
    anim_ctx->to_h = SCREEN_HEIGHT;
    anim_ctx->is_opening = true;

    s_window_anim_ctx = anim_ctx;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, anim_ctx);
    lv_anim_set_exec_cb(&a, window_anim_exec_cb);
    lv_anim_set_values(&a, 0, ANIM_PROGRESS_MAX);
    lv_anim_set_duration(&a, WINDOW_OPEN_DURATION_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, window_anim_completed_cb);
    lv_anim_set_deleted_cb(&a, window_anim_deleted_cb);
    lv_anim_start(&a);

    ESP_LOGI(TAG, "Opened paged window %s (origin %d,%d)",
             config.title ? config.title : "Paged Window", origin_x, origin_y);
}

extern "C" bool corep4_ui_has_active_function_window(void)
{
    return s_active_window != nullptr;
}
