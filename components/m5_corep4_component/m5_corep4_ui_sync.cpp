/**
 * @file m5_corep4_ui_sync.cpp
 * @brief Top-level CoreP4 LVGL UI synchronization entry points.
 */

#include "m5_corep4_ui_sync.h"
#include "corep4_lvgl_ui_sync/corep4_lvgl_ui_sync_internal.h"

static const char* TAG = "CoreP4UISync";

esp_err_t ui_sync_init(m5_corep4_tool& tool)
{
    corep4_ui_sync_set_tool(tool);

    esp_err_t guard_ret = corep4_ui_usb_phy_guard_start();
    if (guard_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start USB PHY guard flow: %s", esp_err_to_name(guard_ret));
    }

    esp_err_t ret = corep4_ui_power_sync_init(tool);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init power UI sync");
        return ret;
    }

    ret = corep4_ui_sleep_sync_init(tool);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init sleep UI sync");
        return ret;
    }

    ret = corep4_ui_sd_sync_init(tool);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init SD UI sync");
    }

    ret = corep4_ui_eth_sync_init(tool);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init Ethernet UI sync");
    }

    ESP_LOGI(TAG, "UI sync module initialized");
    return ESP_OK;
}

esp_err_t ui_sync_bindAllEvents(m5_corep4_tool& tool)
{
    corep4_ui_sync_set_tool(tool);

    esp_err_t ret = corep4_ui_power_bind_events(tool);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to bind power checkbox events");
        return ret;
    }

    ret = corep4_ui_sleep_bind_events(tool);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to bind sleep checkbox events");
        return ret;
    }

    ret = corep4_ui_sleep_sync_to_checkbox(tool);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to sync sleep status to UI");
    }

    ret = corep4_ui_mian_bind_events(tool);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to bind ui_mian custom events");
    }

    ESP_LOGI(TAG, "All UI events bound successfully");
    return ESP_OK;
}

esp_err_t power_ui_sync_init(m5_corep4_tool& tool)
{
    return corep4_ui_power_sync_init(tool);
}

esp_err_t power_ui_bind_checkbox_events(m5_corep4_tool& tool)
{
    return corep4_ui_power_bind_events(tool);
}

esp_err_t sleep_ui_sync_init(m5_corep4_tool& tool)
{
    return corep4_ui_sleep_sync_init(tool);
}

esp_err_t sleep_ui_bind_checkbox_events(m5_corep4_tool& tool)
{
    return corep4_ui_sleep_bind_events(tool);
}

esp_err_t sleep_ui_sync_to_checkbox(m5_corep4_tool& tool)
{
    return corep4_ui_sleep_sync_to_checkbox(tool);
}

esp_err_t sd_ui_sync_init(m5_corep4_tool& tool)
{
    return corep4_ui_sd_sync_init(tool);
}

esp_err_t eth_ui_sync_init(m5_corep4_tool& tool)
{
    return corep4_ui_eth_sync_init(tool);
}
