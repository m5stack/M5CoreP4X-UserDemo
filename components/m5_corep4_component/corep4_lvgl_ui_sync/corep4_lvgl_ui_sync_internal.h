#pragma once

#include "m5_corep4_ui_sync.h"
#include "esp_log.h"
#include "ui.h"

constexpr const char* COREP4_UI_SYNC_TAG = "CoreP4UISync";

m5_corep4_tool* corep4_ui_sync_get_tool();
void corep4_ui_sync_set_tool(m5_corep4_tool& tool);

const char* corep4_ui_event_code_name(lv_event_code_t code);
void corep4_ui_log_placeholder_event(lv_event_t* e,
                                     const char* function_name,
                                     const char* screen_name,
                                     const char* trigger_desc,
                                     const char* reason);

esp_err_t corep4_ui_mian_bind_events(m5_corep4_tool& tool);

esp_err_t corep4_ui_power_sync_init(m5_corep4_tool& tool);
esp_err_t corep4_ui_power_bind_events(m5_corep4_tool& tool);

esp_err_t corep4_ui_sleep_sync_init(m5_corep4_tool& tool);
esp_err_t corep4_ui_sleep_bind_events(m5_corep4_tool& tool);
esp_err_t corep4_ui_sleep_sync_to_checkbox(m5_corep4_tool& tool);

esp_err_t corep4_ui_sd_sync_init(m5_corep4_tool& tool);
esp_err_t corep4_ui_eth_sync_init(m5_corep4_tool& tool);
esp_err_t corep4_ui_usb_phy_guard_start();
