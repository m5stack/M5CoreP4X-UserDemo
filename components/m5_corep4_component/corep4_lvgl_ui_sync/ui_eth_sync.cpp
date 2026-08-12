#include "corep4_lvgl_ui_sync_internal.h"

#include <cstdio>

namespace {

constexpr const char* TAG = "UI_ETH";

void clear_eth_detail_labels()
{
    lv_label_set_text(ui_laniplab, "IP: ---");
    lv_label_set_text(ui_langwlab, "GW: ---");
    lv_label_set_text(ui_lanmasklab, "Mask: ---");
    lv_label_set_text(ui_lanhostlab, "MAC: ---");
}

void update_eth_got_ip_labels(const device_realtime_status_t* status)
{
    lv_label_set_text(ui_lantitle, "LAN: OK");

    char buf[64];
    std::snprintf(buf, sizeof(buf), "IP: %s", status->ethernet.ip_address);
    lv_label_set_text(ui_laniplab, buf);

    std::snprintf(buf, sizeof(buf), "GW: %s", status->ethernet.gateway);
    lv_label_set_text(ui_langwlab, buf);

    std::snprintf(buf, sizeof(buf), "Mask: %s", status->ethernet.netmask);
    lv_label_set_text(ui_lanmasklab, buf);

    std::snprintf(buf, sizeof(buf), "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                  status->ethernet.eth_mac_addr[0], status->ethernet.eth_mac_addr[1],
                  status->ethernet.eth_mac_addr[2], status->ethernet.eth_mac_addr[3],
                  status->ethernet.eth_mac_addr[4], status->ethernet.eth_mac_addr[5]);
    lv_label_set_text(ui_lanhostlab, buf);
}

void update_eth_all_labels(const device_realtime_status_t* status)
{
    if (status->ethernet.ethernet_initialized) {
        if (status->ethernet.got_ip) {
            update_eth_got_ip_labels(status);
        } else if (status->ethernet.ethernet_connected) {
            lv_label_set_text(ui_lantitle, "LAN: UP");
            clear_eth_detail_labels();
        } else {
            lv_label_set_text(ui_lantitle, "LAN: --");
            clear_eth_detail_labels();
        }
    } else {
        lv_label_set_text(ui_lantitle, "LAN: NA");
        clear_eth_detail_labels();
    }
}

void eth_ui_sync_callback(StatusParam param, const StatusValue& value)
{
    if (!is_eth_param(param)) {
        return;
    }

    if (!lvgl_port_lock(500)) {
        ESP_LOGW(TAG, "Failed to lock LVGL port for Ethernet UI update");
        return;
    }

    switch (param) {
        case StatusParam::ETH_INITIALIZED: {
            bool initialized = false;
            if (get_status_value(value, initialized)) {
                if (initialized) {
                    lv_label_set_text(ui_lantitle, "LAN: --");
                } else {
                    lv_label_set_text(ui_lantitle, "LAN: NA");
                    clear_eth_detail_labels();
                }
            }
            break;
        }
        case StatusParam::ETH_CONNECTED: {
            bool connected = false;
            if (get_status_value(value, connected)) {
                if (connected) {
                    lv_label_set_text(ui_lantitle, "LAN: UP");
                } else {
                    lv_label_set_text(ui_lantitle, "LAN: --");
                    clear_eth_detail_labels();
                }
            }
            break;
        }
        case StatusParam::ETH_GOT_IP: {
            bool got_ip = false;
            auto* tool = corep4_ui_sync_get_tool();
            if (get_status_value(value, got_ip) && got_ip && tool) {
                auto* status = tool->get_device_status();
                if (status) {
                    update_eth_got_ip_labels(status);
                }
            }
            break;
        }
        default:
            break;
    }

    lvgl_port_unlock();
}

} // namespace

esp_err_t corep4_ui_eth_sync_init(m5_corep4_tool& tool)
{
    corep4_ui_sync_set_tool(tool);
    ESP_LOGI(TAG, "Initializing Ethernet UI sync");

    tool.register_ui_sync_callback(eth_ui_sync_callback);

    auto* status = tool.get_device_status();
    if (status && lvgl_port_lock(500)) {
        update_eth_all_labels(status);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Ethernet UI sync initialized");
    return ESP_OK;
}
