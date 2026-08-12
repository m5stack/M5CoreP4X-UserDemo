#include "corep4_lvgl_ui_sync_internal.h"

#include <cstdio>
#include <inttypes.h>

namespace {

constexpr const char* TAG = "UI_SD";

void update_sd_ui_labels(bool detected, bool initialized,
                         uint64_t total_bytes, uint64_t free_bytes)
{
    if (ui_sddetvallab) {
        if (!detected) {
            lv_label_set_text(ui_sddetvallab, "No Card");
        } else if (!initialized) {
            lv_label_set_text(ui_sddetvallab, "Error");
        } else {
            lv_label_set_text(ui_sddetvallab, "Ready");
        }
    }

    if (ui_sdromvallab) {
        if (!initialized || total_bytes == 0) {
            lv_label_set_text(ui_sdromvallab, "N/A");
        } else {
            char buf[48];
            uint64_t total_mb = total_bytes / (1024ULL * 1024ULL);
            uint64_t free_mb = free_bytes / (1024ULL * 1024ULL);
            if (total_mb >= 1024) {
                std::snprintf(buf, sizeof(buf), "%.1fGB / %.1fGB free",
                              total_mb / 1024.0f, free_mb / 1024.0f);
            } else {
                std::snprintf(buf, sizeof(buf), "%" PRIu64 "MB / %" PRIu64 "MB free",
                              total_mb, free_mb);
            }
            lv_label_set_text(ui_sdromvallab, buf);
        }
    }
}

void sd_ui_sync_callback(StatusParam param, const StatusValue& value)
{
    (void)value;

    if (!is_sd_param(param)) {
        return;
    }

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        return;
    }

    auto* status = tool->get_device_status();
    if (!status) {
        return;
    }

    if (lvgl_port_lock(100)) {
        update_sd_ui_labels(status->sdcard.sd_detected,
                            status->sdcard.sd_initialized,
                            status->sdcard.sd_total_bytes,
                            status->sdcard.sd_free_bytes);
        lvgl_port_unlock();
        ESP_LOGD(TAG, "SD UI sync: detected=%d initialized=%d total=%" PRIu64 " free=%" PRIu64,
                 static_cast<int>(status->sdcard.sd_detected),
                 static_cast<int>(status->sdcard.sd_initialized),
                 status->sdcard.sd_total_bytes,
                 status->sdcard.sd_free_bytes);
    } else {
        ESP_LOGW(TAG, "Failed to acquire LVGL lock for SD UI sync");
    }
}

} // namespace

esp_err_t corep4_ui_sd_sync_init(m5_corep4_tool& tool)
{
    corep4_ui_sync_set_tool(tool);
    tool.register_ui_sync_callback(sd_ui_sync_callback);

    auto* status = tool.get_device_status();
    if (status && lvgl_port_lock(500)) {
        update_sd_ui_labels(status->sdcard.sd_detected,
                            status->sdcard.sd_initialized,
                            status->sdcard.sd_total_bytes,
                            status->sdcard.sd_free_bytes);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "SD UI sync initialized");
    return ESP_OK;
}
