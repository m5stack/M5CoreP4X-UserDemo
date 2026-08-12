#include "corep4_lvgl_ui_sync_internal.h"

namespace {

constexpr const char* TAG = "UI_POWER";

struct PowerCheckboxBinding {
    StatusParam param;
    lv_obj_t** object;
    const char* name;
};

PowerCheckboxBinding s_power_checkboxes[] = {
    {StatusParam::POWER_GROVE, &ui_cboxgrovepwr, "Grove"},
    {StatusParam::POWER_PYB_USBA, &ui_cboxusbapwr, "USBA"},
    {StatusParam::POWER_PYB_L3B, &ui_cboxl3bpwr, "L3B"},
    {StatusParam::POWER_AUDIO_PA, &ui_cboxpapwr, "PA"},
    {StatusParam::POWER_P4USBC, &ui_cboxusbcoutpwr, "USBCOUT"},
    {StatusParam::POWER_AUDIO_CHIP, &ui_cboxaupwr, "AudioChip"},
    {StatusParam::POWER_M5BUS, &ui_cboxm5bus, "M5BUS"},
};

bool value_to_bool(const StatusValue& value, bool& out)
{
    if (get_status_value(value, out)) {
        return true;
    }

    uint8_t u8val = 0;
    if (get_status_value(value, u8val)) {
        out = (u8val != 0);
        return true;
    }

    return false;
}

const PowerCheckboxBinding* find_binding(StatusParam param)
{
    for (const auto& binding : s_power_checkboxes) {
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

void power_ui_sync_callback(StatusParam param, const StatusValue& value)
{
    if (!is_power_param(param)) {
        return;
    }

    const PowerCheckboxBinding* binding = find_binding(param);
    if (!binding) {
        return;
    }

    bool on = false;
    if (!value_to_bool(value, on)) {
        ESP_LOGW(TAG, "Unsupported value type for param 0x%04X", static_cast<uint16_t>(param));
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
        ESP_LOGW(TAG, "Failed to acquire LVGL lock for power UI sync");
    }
}

void power_checkbox_event_cb(lv_event_t* e)
{
    auto* binding = static_cast<const PowerCheckboxBinding*>(lv_event_get_user_data(e));
    auto* tool = corep4_ui_sync_get_tool();
    if (!tool || !binding) {
        ESP_LOGE(TAG, "Cannot handle power checkbox event: tool=%p binding=%p",
                 static_cast<void*>(tool), static_cast<const void*>(binding));
        return;
    }

    lv_obj_t* checkbox = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool is_checked = lv_obj_has_state(checkbox, LV_STATE_CHECKED);

    tool->set_status(binding->param, is_checked,
                     StatusConfigFlag::APPLY_HARDWARE | StatusConfigFlag::UPDATE_STATUS);

    ESP_LOGI(TAG, "%s power %s by checkbox", binding->name,
             is_checked ? "enabled" : "disabled");
}

} // namespace

esp_err_t corep4_ui_power_sync_init(m5_corep4_tool& tool)
{
    corep4_ui_sync_set_tool(tool);
    tool.register_ui_sync_callback(power_ui_sync_callback);
    ESP_LOGI(TAG, "Power UI sync initialized");
    return ESP_OK;
}

esp_err_t corep4_ui_power_bind_events(m5_corep4_tool& tool)
{
    corep4_ui_sync_set_tool(tool);

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for power event binding");
        return ESP_FAIL;
    }

    for (auto& binding : s_power_checkboxes) {
        lv_obj_t* checkbox = binding.object ? *binding.object : nullptr;
        if (checkbox) {
            if (binding.param == StatusParam::POWER_M5BUS) {
                lv_obj_add_state(checkbox, LV_STATE_CHECKED);
            }
            lv_obj_add_event_cb(checkbox, power_checkbox_event_cb,
                                LV_EVENT_VALUE_CHANGED, &binding);
            ESP_LOGD(TAG, "Bound %s checkbox event", binding.name);
        }
    }

    lvgl_port_unlock();

    if (ui_cboxm5bus) {
        tool.set_status(StatusParam::POWER_M5BUS, true,
                        StatusConfigFlag::APPLY_HARDWARE | StatusConfigFlag::UPDATE_STATUS);
        ESP_LOGI(TAG, "M5BUS power enabled by default");
    }

    ESP_LOGI(TAG, "Power checkbox events bound successfully");
    return ESP_OK;
}
