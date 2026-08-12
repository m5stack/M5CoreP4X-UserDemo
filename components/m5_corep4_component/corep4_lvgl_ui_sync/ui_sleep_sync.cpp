#include "corep4_lvgl_ui_sync_internal.h"

namespace {

constexpr const char* TAG = "UI_SLEEP";

struct SleepCheckboxBinding {
    StatusParam param;
    lv_obj_t** object;
    const char* name;
};

SleepCheckboxBinding s_sleep_checkboxes[] = {
    {StatusParam::SLEEP_BTN_ENABLE, &ui_cboxbtnsleep, "BTN_SLEEP"},
    {StatusParam::SLEEP_WAKE_RTC, &ui_cboxrtcwake, "RTC_WAKE"},
    {StatusParam::SLEEP_WAKE_TOUCH, &ui_cboxtouchwake, "TOUCH_WAKE"},
    {StatusParam::SLEEP_WAKE_BMI, &ui_cboxbmiwake, "IMU_WAKE"},
};

const SleepCheckboxBinding* find_binding(StatusParam param)
{
    for (const auto& binding : s_sleep_checkboxes) {
        if (binding.param == param) {
            return &binding;
        }
    }
    return nullptr;
}

void set_checkbox_state(lv_obj_t* checkbox, bool checked)
{
    if (checked) {
        lv_obj_add_state(checkbox, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(checkbox, LV_STATE_CHECKED);
    }
}

void sleep_ui_sync_callback(StatusParam param, const StatusValue& value)
{
    if (!is_sleep_param(param)) {
        return;
    }

    const SleepCheckboxBinding* binding = find_binding(param);
    if (!binding) {
        return;
    }

    bool on = false;
    if (!get_status_value(value, on)) {
        return;
    }

    lv_obj_t* checkbox = binding->object ? *binding->object : nullptr;
    if (!checkbox) {
        ESP_LOGW(TAG, "Checkbox for %s not found", binding->name);
        return;
    }

    if (lvgl_port_lock(100)) {
        set_checkbox_state(checkbox, on);
        lvgl_port_unlock();
        ESP_LOGD(TAG, "UI sync: %s checkbox -> %s", binding->name, on ? "checked" : "unchecked");
    } else {
        ESP_LOGW(TAG, "Failed to acquire LVGL lock for sleep UI sync");
    }
}

void sleep_checkbox_event_cb(lv_event_t* e)
{
    auto* binding = static_cast<const SleepCheckboxBinding*>(lv_event_get_user_data(e));
    auto* tool = corep4_ui_sync_get_tool();
    if (!tool || !binding) {
        ESP_LOGE(TAG, "Cannot handle sleep checkbox event: tool=%p binding=%p",
                 static_cast<void*>(tool), static_cast<const void*>(binding));
        return;
    }

    lv_obj_t* checkbox = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool is_checked = lv_obj_has_state(checkbox, LV_STATE_CHECKED);

    tool->set_status(binding->param, is_checked, StatusConfigFlag::UPDATE_STATUS);

    if (binding->param == StatusParam::SLEEP_WAKE_RTC && is_checked) {
        tool->set_status(StatusParam::SLEEP_WAKE_RTC_SEC, 10UL, StatusConfigFlag::UPDATE_STATUS);
    }

    ESP_LOGI(TAG, "%s %s by checkbox", binding->name,
             is_checked ? "enabled" : "disabled");
}

void deepsleepbtn_event_cb(lv_event_t* e)
{
    (void)e;

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        return;
    }

    ESP_LOGI(TAG, "Deep sleep button clicked");
    tool->sleep_before_work();
    tool->enter_deep_sleep();
}

void poweroffbtn_event_cb(lv_event_t* e)
{
    (void)e;

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        return;
    }

    ESP_LOGI(TAG, "Power off button clicked, shutting down");
    tool->pm1.shutdown();
}

} // namespace

esp_err_t corep4_ui_sleep_sync_init(m5_corep4_tool& tool)
{
    corep4_ui_sync_set_tool(tool);
    tool.register_ui_sync_callback(sleep_ui_sync_callback);
    ESP_LOGI(TAG, "Sleep UI sync initialized");
    return ESP_OK;
}

esp_err_t corep4_ui_sleep_bind_events(m5_corep4_tool& tool)
{
    corep4_ui_sync_set_tool(tool);

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for sleep event binding");
        return ESP_FAIL;
    }

    for (auto& binding : s_sleep_checkboxes) {
        lv_obj_t* checkbox = binding.object ? *binding.object : nullptr;
        if (checkbox) {
            lv_obj_add_event_cb(checkbox, sleep_checkbox_event_cb,
                                LV_EVENT_VALUE_CHANGED, &binding);
            ESP_LOGD(TAG, "Bound %s checkbox event", binding.name);
        }
    }

    if (ui_deepsleepbtn) {
        lv_obj_add_event_cb(ui_deepsleepbtn, deepsleepbtn_event_cb, LV_EVENT_CLICKED, nullptr);
        ESP_LOGD(TAG, "Bound Deep Sleep button event");
    }

    if (ui_poweroffbtn) {
        lv_obj_add_event_cb(ui_poweroffbtn, poweroffbtn_event_cb, LV_EVENT_CLICKED, nullptr);
        ESP_LOGD(TAG, "Bound Power Off button event");
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Sleep checkbox events bound successfully");
    return ESP_OK;
}

esp_err_t corep4_ui_sleep_sync_to_checkbox(m5_corep4_tool& tool)
{
    corep4_ui_sync_set_tool(tool);

    if (!lvgl_port_lock(100)) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for sleep UI sync");
        return ESP_FAIL;
    }

    for (const auto& binding : s_sleep_checkboxes) {
        StatusValue value;
        esp_err_t ret = tool.get_status(binding.param, value);
        if (ret != ESP_OK) {
            continue;
        }

        bool checked = false;
        if (!get_status_value(value, checked)) {
            continue;
        }

        lv_obj_t* checkbox = binding.object ? *binding.object : nullptr;
        if (checkbox) {
            set_checkbox_state(checkbox, checked);
        }
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Sleep status synced to UI checkboxes");
    return ESP_OK;
}
