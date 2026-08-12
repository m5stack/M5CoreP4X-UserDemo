#pragma once

#include "../corep4_lvgl_ui_sync_internal.h"

#include <cstddef>
#include <cstdint>

struct CoreP4FunctionWindowConfig {
    const char* title;
    const char* trigger_name;
    void (*build_content)(lv_obj_t* parent, const CoreP4FunctionWindowConfig& config);
    const char* header_action_text = nullptr;
    lv_event_cb_t header_action_callback = nullptr;
    void* header_action_user_data = nullptr;
    uint32_t header_action_bg_color = 0x2563EB;
    uint32_t header_action_text_color = 0xFFFFFF;
    bool header_action_initially_hidden = false;
    void (*header_action_ready_callback)(lv_obj_t* button) = nullptr;
    // When set, this owns the Exit click. The callback may defer destruction
    // until application-owned workers acknowledge cancellation.
    lv_event_cb_t exit_callback = nullptr;
    void* exit_user_data = nullptr;
    void (*exit_button_ready_callback)(lv_obj_t* button) = nullptr;
};

struct CoreP4FunctionPagedWindowConfig {
    const char* title;
    const char* trigger_name;
    const char* tab1_text;
    const char* tab2_text;
    void (*build_tab1_content)(lv_obj_t* parent, const CoreP4FunctionPagedWindowConfig& config);
    void (*build_tab2_content)(lv_obj_t* parent, const CoreP4FunctionPagedWindowConfig& config);
    const char* tab3_text = nullptr;
    void (*build_tab3_content)(lv_obj_t* parent, const CoreP4FunctionPagedWindowConfig& config) = nullptr;
    const char* header_action_text = nullptr;
    lv_event_cb_t header_action_callback = nullptr;
    void* header_action_user_data = nullptr;
    uint32_t header_action_bg_color = 0x2563EB;
    uint32_t header_action_text_color = 0xFFFFFF;
    bool header_action_initially_hidden = false;
    void (*header_action_ready_callback)(lv_obj_t* button) = nullptr;
};

enum class CoreP4FunctionDialogSize : uint8_t {
    SMALL,
    MEDIUM,
    LARGE,
};

constexpr size_t COREP4_FUNCTION_DIALOG_MAX_BUTTONS = 3;

struct CoreP4FunctionDialogButtonConfig {
    const char* text;
    lv_event_cb_t callback;
    void* user_data;
    uint32_t bg_color;
    uint32_t text_color;
    bool hidden;
};

struct CoreP4FunctionDialogHandle {
    lv_obj_t* box;
    lv_obj_t* text_label;
    lv_obj_t* buttons[COREP4_FUNCTION_DIALOG_MAX_BUTTONS];
    uint8_t button_count;
};

void corep4_ui_open_function_window(lv_event_t* e, const CoreP4FunctionWindowConfig& config);
void corep4_ui_open_paged_function_window(lv_event_t* e, const CoreP4FunctionPagedWindowConfig& config);
void corep4_ui_add_default_function_label(lv_obj_t* parent, const char* text);

CoreP4FunctionDialogHandle corep4_ui_open_function_dialog(
    const char* title,
    const char* message,
    CoreP4FunctionDialogSize size,
    const CoreP4FunctionDialogButtonConfig* buttons,
    size_t button_count);
void corep4_ui_close_function_dialog(CoreP4FunctionDialogHandle* dialog);
void corep4_ui_set_function_dialog_text(CoreP4FunctionDialogHandle* dialog,
                                        const char* text);
void corep4_ui_set_function_dialog_button_hidden(CoreP4FunctionDialogHandle* dialog,
                                                 uint8_t index,
                                                 bool hidden);

#ifdef __cplusplus
extern "C" {
#endif
bool corep4_ui_has_active_function_window(void);
#ifdef __cplusplus
}
#endif
