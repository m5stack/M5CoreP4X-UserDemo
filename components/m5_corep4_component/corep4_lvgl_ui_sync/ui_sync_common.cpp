#include "corep4_lvgl_ui_sync_internal.h"

static m5_corep4_tool* s_tool = nullptr;

m5_corep4_tool* corep4_ui_sync_get_tool()
{
    return s_tool;
}

void corep4_ui_sync_set_tool(m5_corep4_tool& tool)
{
    s_tool = &tool;
}

const char* corep4_ui_event_code_name(lv_event_code_t code)
{
    switch (code) {
        case LV_EVENT_PRESSED: return "LV_EVENT_PRESSED";
        case LV_EVENT_PRESSING: return "LV_EVENT_PRESSING";
        case LV_EVENT_PRESS_LOST: return "LV_EVENT_PRESS_LOST";
        case LV_EVENT_SHORT_CLICKED: return "LV_EVENT_SHORT_CLICKED";
        case LV_EVENT_LONG_PRESSED: return "LV_EVENT_LONG_PRESSED";
        case LV_EVENT_LONG_PRESSED_REPEAT: return "LV_EVENT_LONG_PRESSED_REPEAT";
        case LV_EVENT_CLICKED: return "LV_EVENT_CLICKED";
        case LV_EVENT_RELEASED: return "LV_EVENT_RELEASED";
        case LV_EVENT_VALUE_CHANGED: return "LV_EVENT_VALUE_CHANGED";
        case LV_EVENT_GESTURE: return "LV_EVENT_GESTURE";
        case LV_EVENT_SCREEN_LOADED: return "LV_EVENT_SCREEN_LOADED";
        case LV_EVENT_SCREEN_UNLOADED: return "LV_EVENT_SCREEN_UNLOADED";
        default: return "LV_EVENT_OTHER";
    }
}

void corep4_ui_log_placeholder_event(lv_event_t* e,
                                     const char* function_name,
                                     const char* screen_name,
                                     const char* trigger_desc,
                                     const char* reason)
{
    lv_event_code_t code = e ? lv_event_get_code(e) : LV_EVENT_ALL;
    void* target = e ? static_cast<void*>(lv_event_get_target(e)) : nullptr;

    ESP_LOGW("UI_PLACEHOLDER",
             "%s called: screen=%s trigger=%s event=%s target=%p reason=%s",
             function_name ? function_name : "(unknown)",
             screen_name ? screen_name : "(unknown)",
             trigger_desc ? trigger_desc : "(unknown)",
             corep4_ui_event_code_name(code),
             target,
             reason ? reason : "no handler is connected yet");
}
