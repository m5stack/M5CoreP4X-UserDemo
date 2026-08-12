#include "ui_function_widgets.h"

#include "esp_err.h"
#include "esp_hosted.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_WIFI";
constexpr uint16_t MAX_SCAN_RESULTS = 12;
constexpr uint32_t WIFI_REFRESH_MS = 600;
constexpr int32_t AP_LIST_HEIGHT = 292;
constexpr int32_t AP_ROW_HEIGHT = 58;

struct WifiPageState {
    lv_obj_t* parent = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* mac_label = nullptr;
    lv_obj_t* scan_summary_label = nullptr;
    lv_obj_t* ap_list = nullptr;
    lv_obj_t* scan_button = nullptr;
    lv_obj_t* info_button = nullptr;
    lv_obj_t* overlay = nullptr;
    lv_obj_t* overlay_text = nullptr;
    lv_obj_t* overlay_retry_button = nullptr;
    lv_timer_t* refresh_timer = nullptr;
    volatile bool page_alive = false;
    volatile bool scan_task_running = false;
    volatile bool init_task_running = false;
};

WifiPageState s_wifi_page;
CoreP4FunctionDialogHandle s_wifi_info_dialog = {};

const char* auth_mode_name(wifi_auth_mode_t mode)
{
    switch (mode) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI";
        default:                        return "UNK";
    }
}

const char* signal_quality_name(int rssi)
{
    if (rssi >= -55) {
        return "STRONG";
    }
    if (rssi >= -67) {
        return "GOOD";
    }
    if (rssi >= -75) {
        return "FAIR";
    }
    return "WEAK";
}

uint32_t signal_quality_color(int rssi)
{
    if (rssi >= -55) {
        return COLOR_SUCCESS;
    }
    if (rssi >= -67) {
        return COLOR_PRIMARY;
    }
    if (rssi >= -75) {
        return COLOR_WARNING;
    }
    return COLOR_ERROR;
}

void copy_ssid(char* out, size_t out_size, const wifi_ap_record_t& record)
{
    if (!out || out_size == 0) {
        return;
    }
    std::memset(out, 0, out_size);
    const size_t copy_len = (out_size - 1) < sizeof(record.ssid) ? (out_size - 1) : sizeof(record.ssid);
    std::memcpy(out, record.ssid, copy_len);
    out[out_size - 1] = '\0';
    if (!out[0]) {
        std::snprintf(out, out_size, "<hidden>");
    }
}

bool wifi_busy()
{
    auto* tool = corep4_ui_sync_get_tool();
    return s_wifi_page.scan_task_running ||
           s_wifi_page.init_task_running ||
           (tool && tool->is_wifi_init_running());
}

bool wifi_ready()
{
    auto* tool = corep4_ui_sync_get_tool();
    return tool && tool->is_wifi_initialized();
}

void set_wifi_controls_enabled(bool enabled)
{
    set_button_enabled(s_wifi_page.scan_button, enabled);
}

void set_info_button_visible(bool visible)
{
    if (!s_wifi_page.info_button) {
        return;
    }

    if (visible) {
        lv_obj_remove_flag(s_wifi_page.info_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_page.info_button, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_flag(s_wifi_page.info_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_wifi_page.info_button, LV_OBJ_FLAG_CLICKABLE);
        corep4_ui_close_function_dialog(&s_wifi_info_dialog);
    }
}

void set_summary_text(const char* text, uint32_t color)
{
    set_label_text(s_wifi_page.scan_summary_label, text);
    if (s_wifi_page.scan_summary_label) {
        lv_obj_set_style_text_color(s_wifi_page.scan_summary_label, lv_color_hex(color), LV_PART_MAIN);
    }
}

void style_text_14(lv_obj_t* label)
{
    if (label) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    }
}

void update_overlay(bool ready, bool busy, esp_err_t last_ret)
{
    if (!s_wifi_page.overlay) {
        return;
    }

    if (ready) {
        lv_obj_add_flag(s_wifi_page.overlay, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(s_wifi_page.overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wifi_page.overlay);

    char text[192];
    if (busy) {
        std::snprintf(text, sizeof(text),
                      "WiFi requires the A172 Stamp-AddOn C6 For P4 module.\n"
                      "Initializing module, please wait.");
    } else if (last_ret == ESP_ERR_INVALID_STATE) {
        std::snprintf(text, sizeof(text),
                      "WiFi requires the A172 Stamp-AddOn C6 For P4 module.\n"
                      "Install it, then tap Retry.");
    } else {
        std::snprintf(text, sizeof(text),
                      "WiFi requires the A172 Stamp-AddOn C6 For P4 module.\n"
                      "Initialization failed: %s. Install it, then tap Retry.",
                      esp_err_to_name(last_ret));
    }

    set_label_text(s_wifi_page.overlay_text, text);
    set_button_text(s_wifi_page.overlay_retry_button, busy ? "Working" : "Retry");
    set_button_enabled(s_wifi_page.overlay_retry_button, !busy);
}

void show_ap_message(const char* message, uint32_t color)
{
    if (!s_wifi_page.ap_list) {
        return;
    }

    lv_obj_clean(s_wifi_page.ap_list);

    lv_obj_t* box = lv_obj_create(s_wifi_page.ap_list);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, AP_LIST_HEIGHT - 8);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(box, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* label = add_label(box, message, color, 372);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

void add_ap_footer(uint16_t hidden_count)
{
    if (!s_wifi_page.ap_list || hidden_count == 0) {
        return;
    }

    char text[64];
    std::snprintf(text, sizeof(text), "+%u more AP(s). Scan result is limited to %u.",
                  static_cast<unsigned>(hidden_count),
                  static_cast<unsigned>(MAX_SCAN_RESULTS));

    lv_obj_t* box = lv_obj_create(s_wifi_page.ap_list);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, 34);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xEFF6FF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(0xBFDBFE), LV_PART_MAIN);
    lv_obj_set_style_radius(box, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* label = add_label(box, text, COLOR_PRIMARY, 360);
    style_text_14(label);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

void add_ap_row(const wifi_ap_record_t& record, uint16_t index)
{
    if (!s_wifi_page.ap_list) {
        return;
    }

    char ssid[33];
    copy_ssid(ssid, sizeof(ssid), record);

    const uint32_t signal_color = signal_quality_color(static_cast<int>(record.rssi));

    lv_obj_t* row = lv_obj_create(s_wifi_page.ap_list);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, AP_ROW_HEIGHT);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* signal_box = lv_obj_create(row);
    lv_obj_remove_flag(signal_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(signal_box, 88, 44);
    lv_obj_set_style_bg_color(signal_box, lv_color_hex(signal_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(signal_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(signal_box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(signal_box, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(signal_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(signal_box, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_row(signal_box, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(signal_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(signal_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char rssi_text[16];
    std::snprintf(rssi_text, sizeof(rssi_text), "%d", static_cast<int>(record.rssi));
    lv_obj_t* rssi_label = add_label(signal_box, rssi_text, 0xFFFFFF, 80);
    style_text_14(rssi_label);
    lv_obj_set_style_text_align(rssi_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(rssi_label, LV_LABEL_LONG_DOT);

    lv_obj_t* quality_label = add_label(signal_box,
                                        signal_quality_name(static_cast<int>(record.rssi)),
                                        0xFFFFFF,
                                        80);
    style_text_14(quality_label);
    lv_obj_set_style_text_align(quality_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(quality_label, LV_LABEL_LONG_DOT);

    lv_obj_t* detail_box = lv_obj_create(row);
    lv_obj_remove_flag(detail_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(detail_box, 300, 44);
    lv_obj_set_style_bg_opa(detail_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(detail_box, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(detail_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(detail_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(detail_box, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(detail_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(detail_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    char ssid_line[44];
    std::snprintf(ssid_line, sizeof(ssid_line), "%u. %s",
                  static_cast<unsigned>(index + 1),
                  ssid);
    lv_obj_t* ssid_label = add_label(detail_box, ssid_line, COLOR_TEXT, 300);
    lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_DOT);

    char detail_line[64];
    std::snprintf(detail_line, sizeof(detail_line), "CH:%u  Auth:%s  RSSI:%d dBm",
                  static_cast<unsigned>(record.primary),
                  auth_mode_name(record.authmode),
                  static_cast<int>(record.rssi));
    lv_obj_t* detail_label = add_label(detail_box, detail_line, COLOR_MUTED, 300);
    style_text_14(detail_label);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);
}

void render_ap_list(const wifi_ap_record_t* records,
                    uint16_t record_count,
                    uint16_t total_count,
                    esp_err_t ret,
                    const char* empty_message,
                    uint32_t empty_color)
{
    if (!s_wifi_page.ap_list) {
        return;
    }

    lv_obj_clean(s_wifi_page.ap_list);

    if (ret != ESP_OK || !records || record_count == 0) {
        show_ap_message(empty_message, empty_color);
        return;
    }

    for (uint16_t i = 0; i < record_count; ++i) {
        add_ap_row(records[i], i);
    }

    if (total_count > record_count) {
        add_ap_footer(static_cast<uint16_t>(total_count - record_count));
    }
}

void refresh_wifi_status()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    bool busy = wifi_busy();
    bool ready = tool && tool->is_wifi_initialized();
    esp_err_t last_ret = tool ? tool->get_wifi_last_init_result() : ESP_ERR_INVALID_STATE;

    if (tool && ready && !busy && status && status->wifi.wifi_connected) {
        tool->wifi_refresh_status();
        status = tool->get_device_status();
    }

    set_info_button_visible(ready);

    if (!tool || !status) {
        set_label_text(s_wifi_page.status_label, "Module: tool not ready");
        set_label_text(s_wifi_page.mac_label, "STA MAC: --");
        set_wifi_controls_enabled(false);
        update_overlay(false, false, ESP_ERR_INVALID_STATE);
        return;
    }

    char status_buf[160];
    if (ready && status->wifi.wifi_connected) {
        std::snprintf(status_buf, sizeof(status_buf),
                      "Connected: %s  RSSI:%d dBm  CH:%u",
                      status->wifi.ssid[0] ? status->wifi.ssid : "--",
                      static_cast<int>(status->wifi.rssi),
                      static_cast<unsigned>(status->wifi.channel));
    } else if (ready) {
        std::snprintf(status_buf, sizeof(status_buf), "Ready: not connected, scan available");
    } else if (busy) {
        std::snprintf(status_buf, sizeof(status_buf), "Module: initializing...");
    } else if (last_ret == ESP_ERR_INVALID_STATE) {
        std::snprintf(status_buf, sizeof(status_buf), "Module: not initialized");
    } else {
        std::snprintf(status_buf, sizeof(status_buf), "Module: unavailable (%s)", esp_err_to_name(last_ret));
    }
    set_label_text(s_wifi_page.status_label, status_buf);

    char mac_buf[96];
    if (ready) {
        const uint8_t* mac = status->wifi.wifi_mac_addr;
        std::snprintf(mac_buf, sizeof(mac_buf), "STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        std::snprintf(mac_buf, sizeof(mac_buf), "STA MAC: --");
    }
    set_label_text(s_wifi_page.mac_label, mac_buf);

    set_wifi_controls_enabled(ready && !busy);
    update_overlay(ready, busy, last_ret);
}

void update_wifi_ui_from_task(const wifi_ap_record_t* records,
                              uint16_t record_count,
                              uint16_t total_count,
                              esp_err_t ret,
                              const char* summary,
                              uint32_t color)
{
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock LVGL for WiFi update");
        return;
    }

    if (s_wifi_page.page_alive) {
        refresh_wifi_status();
        set_summary_text(summary, color);
        render_ap_list(records, record_count, total_count, ret, summary, color);
    }

    lvgl_port_unlock();
}

void wifi_init_task(void* /*param*/)
{
    auto* tool = corep4_ui_sync_get_tool();
    esp_err_t ret = tool ? tool->wifi_module_init(false) : ESP_ERR_INVALID_STATE;

    char result[160];
    if (ret == ESP_OK) {
        std::snprintf(result, sizeof(result), "Ready. Tap Scan AP to find nearby networks.");
    } else {
        std::snprintf(result, sizeof(result), "WiFi module init failed: %s", esp_err_to_name(ret));
    }

    s_wifi_page.init_task_running = false;
    update_wifi_ui_from_task(nullptr, 0, 0, ret, result, ret == ESP_OK ? COLOR_SUCCESS : COLOR_ERROR);
    vTaskDelete(nullptr);
}

void wifi_scan_task(void* /*param*/)
{
    auto* tool = corep4_ui_sync_get_tool();
    char result[160];
    result[0] = '\0';
    uint32_t color = COLOR_ERROR;
    esp_err_t ret = ESP_FAIL;
    uint16_t total_count = 0;
    uint16_t record_count = 0;

    wifi_ap_record_t* records = static_cast<wifi_ap_record_t*>(
        std::calloc(MAX_SCAN_RESULTS, sizeof(wifi_ap_record_t)));
    if (!records) {
        ret = ESP_ERR_NO_MEM;
        std::snprintf(result, sizeof(result), "Scan failed: no memory for AP records.");
        goto done;
    }

    if (!tool) {
        ret = ESP_ERR_INVALID_STATE;
        std::snprintf(result, sizeof(result), "Scan failed: device tool not ready.");
        goto done;
    }

    ret = tool->wifi_scan_aps(records, MAX_SCAN_RESULTS,
                              &total_count, &record_count,
                              false, 10000);
    if (ret != ESP_OK) {
        std::snprintf(result, sizeof(result), "Scan failed: %s", esp_err_to_name(ret));
        goto done;
    }

    if (total_count == 0) {
        std::snprintf(result, sizeof(result), "Scan OK: no APs found nearby.");
        color = COLOR_WARNING;
        goto done;
    }

    std::snprintf(result, sizeof(result), "Scan OK: found %u AP(s), showing %u.",
                  static_cast<unsigned>(total_count),
                  static_cast<unsigned>(record_count));
    color = COLOR_SUCCESS;

done:
    ESP_LOGI(TAG, "WiFi task done: %s", result);
    s_wifi_page.scan_task_running = false;
    update_wifi_ui_from_task(records,
                             ret == ESP_OK ? record_count : 0,
                             ret == ESP_OK ? total_count : 0,
                             ret,
                             result,
                             color);
    if (records) {
        std::free(records);
    }
    vTaskDelete(nullptr);
}

void start_wifi_init_task()
{
    if (wifi_busy()) {
        return;
    }

    s_wifi_page.init_task_running = true;
    set_wifi_controls_enabled(false);
    set_button_enabled(s_wifi_page.overlay_retry_button, false);
    set_button_text(s_wifi_page.overlay_retry_button, "Working");
    set_summary_text("Initializing WiFi module...", COLOR_WARNING);
    show_ap_message("WiFi initialization is running...", COLOR_WARNING);
    refresh_wifi_status();

    BaseType_t ret = xTaskCreate(wifi_init_task,
                                 "func_wifi_init",
                                 8192,
                                 nullptr,
                                 configMAX_PRIORITIES - 4,
                                 nullptr);
    if (ret != pdPASS) {
        s_wifi_page.init_task_running = false;
        set_summary_text("WiFi init failed: cannot create worker task.", COLOR_ERROR);
        show_ap_message("WiFi init failed: cannot create worker task.", COLOR_ERROR);
        refresh_wifi_status();
    }
}

void start_wifi_scan_task()
{
    if (wifi_busy()) {
        return;
    }

    if (!wifi_ready()) {
        set_summary_text("WiFi module is not ready. Tap Retry after inserting the module.", COLOR_WARNING);
        show_ap_message("WiFi module is not ready.", COLOR_WARNING);
        refresh_wifi_status();
        return;
    }

    s_wifi_page.scan_task_running = true;
    set_wifi_controls_enabled(false);
    set_summary_text("Scanning APs via ESP32-C6...", COLOR_WARNING);
    show_ap_message("Scanning nearby networks...", COLOR_WARNING);

    BaseType_t ret = xTaskCreate(wifi_scan_task,
                                 "func_wifi_scan",
                                 8192,
                                 nullptr,
                                 configMAX_PRIORITIES - 4,
                                 nullptr);
    if (ret != pdPASS) {
        s_wifi_page.scan_task_running = false;
        set_summary_text("WiFi scan failed: cannot create worker task.", COLOR_ERROR);
        show_ap_message("WiFi scan failed: cannot create worker task.", COLOR_ERROR);
        refresh_wifi_status();
    }
}

void scan_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        start_wifi_scan_task();
    }
}

void close_info_dialog_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        corep4_ui_close_function_dialog(&s_wifi_info_dialog);
    }
}

void info_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    corep4_ui_close_function_dialog(&s_wifi_info_dialog);

    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    const bool ready = tool && tool->is_wifi_initialized();
    const esp_err_t last_ret = tool ? tool->get_wifi_last_init_result() : ESP_ERR_INVALID_STATE;

    uint32_t cp_chip_id = 0;
    char cp_target[24] = {};
    esp_hosted_coprocessor_fwver_t cp_fw = {};
    esp_hosted_app_desc_t cp_app = {};
    esp_err_t cp_info_ret = ESP_ERR_INVALID_STATE;
    esp_err_t cp_fw_ret = ESP_ERR_INVALID_STATE;
    esp_err_t cp_app_ret = ESP_ERR_INVALID_STATE;

    if (ready) {
        cp_info_ret = static_cast<esp_err_t>(
            esp_hosted_get_cp_info(&cp_chip_id, cp_target, sizeof(cp_target)));
        cp_fw_ret = static_cast<esp_err_t>(
            esp_hosted_get_coprocessor_fwversion(&cp_fw));
        cp_app_ret = esp_hosted_get_coprocessor_app_desc(&cp_app);
        cp_target[sizeof(cp_target) - 1] = '\0';
        cp_app.project_name[sizeof(cp_app.project_name) - 1] = '\0';
        cp_app.version[sizeof(cp_app.version) - 1] = '\0';
        cp_app.idf_ver[sizeof(cp_app.idf_ver) - 1] = '\0';
    }

    char mac_text[32] = "--";
    if (ready && status) {
        const uint8_t* mac = status->wifi.wifi_mac_addr;
        std::snprintf(mac_text, sizeof(mac_text), "%02X:%02X:%02X:%02X:%02X:%02X",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    char cp_target_text[48];
    if (cp_info_ret == ESP_OK && cp_target[0]) {
        std::snprintf(cp_target_text, sizeof(cp_target_text), "%s", cp_target);
    } else {
        std::snprintf(cp_target_text, sizeof(cp_target_text), "N/A (%s)", esp_err_to_name(cp_info_ret));
    }

    char cp_chip_text[48];
    if (cp_info_ret == ESP_OK) {
        std::snprintf(cp_chip_text, sizeof(cp_chip_text), "0x%08lX",
                      static_cast<unsigned long>(cp_chip_id));
    } else {
        std::snprintf(cp_chip_text, sizeof(cp_chip_text), "N/A");
    }

    char cp_fw_text[64];
    if (cp_fw_ret == ESP_OK) {
        std::snprintf(cp_fw_text, sizeof(cp_fw_text), "%lu.%lu.%lu",
                      static_cast<unsigned long>(cp_fw.major1),
                      static_cast<unsigned long>(cp_fw.minor1),
                      static_cast<unsigned long>(cp_fw.patch1));
    } else {
        std::snprintf(cp_fw_text, sizeof(cp_fw_text), "N/A (%s)", esp_err_to_name(cp_fw_ret));
    }

    const bool has_app_desc = cp_app_ret == ESP_OK && cp_app.magic_word == ESP_HOSTED_APP_DESC_MAGIC_WORD;
    const char* cp_app_name = has_app_desc && cp_app.project_name[0] ? cp_app.project_name : "N/A";
    const char* cp_app_ver = has_app_desc && cp_app.version[0] ? cp_app.version : "N/A";
    const char* cp_idf_ver = has_app_desc && cp_app.idf_ver[0] ? cp_app.idf_ver : "N/A";

    char info_text[900];
    std::snprintf(info_text, sizeof(info_text),
                  "Hardware\n"
                  "Module: A172 Stamp-AddOn C6 For P4\n"
                  "CP target: %s\n"
                  "CP chip ID: %s\n"
                  "STA MAC: %s\n"
                  "Host target: %s\n\n"
                  "Driver\n"
                  "Host driver: ESP-Hosted-MCU SDIO\n"
                  "WiFi wrapper: esp_wifi_remote 1.4.0\n"
                  "Transport: SDIO 4-bit, slot 1, reset GPIO42\n"
                  "TX/RX queue: %d/%d\n"
                  "Last init: %s\n\n"
                  "Version\n"
                  "Host ESP-Hosted: %u.%u.%u\n"
                  "CP ESP-Hosted FW: %s\n"
                  "Host ESP-IDF: %s\n"
                  "CP app: %s %s\n"
                  "CP ESP-IDF: %s",
                  cp_target_text,
                  cp_chip_text,
                  mac_text,
                  CONFIG_IDF_TARGET,
                  CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE,
                  CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE,
                  ready ? "ESP_OK" : esp_err_to_name(last_ret),
                  ESP_HOSTED_VERSION_MAJOR_1,
                  ESP_HOSTED_VERSION_MINOR_1,
                  ESP_HOSTED_VERSION_PATCH_1,
                  cp_fw_text,
                  esp_get_idf_version(),
                  cp_app_name,
                  cp_app_ver,
                  cp_idf_ver);

    const CoreP4FunctionDialogButtonConfig buttons[] = {
        {
            "Close",
            close_info_dialog_cb,
            nullptr,
            COLOR_BUTTON_BG,
            COLOR_TEXT,
            false,
        },
    };

    s_wifi_info_dialog = corep4_ui_open_function_dialog("ESP32-C6 WiFi Module",
                                                        info_text,
                                                        CoreP4FunctionDialogSize::LARGE,
                                                        buttons,
                                                        sizeof(buttons) / sizeof(buttons[0]));
}

void retry_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        start_wifi_init_task();
    }
}

void wifi_refresh_timer_cb(lv_timer_t* /*timer*/)
{
    if (!s_wifi_page.page_alive) {
        return;
    }
    refresh_wifi_status();
}

void wifi_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    corep4_ui_close_function_dialog(&s_wifi_info_dialog);

    if (s_wifi_page.refresh_timer) {
        lv_timer_delete(s_wifi_page.refresh_timer);
        s_wifi_page.refresh_timer = nullptr;
    }

    s_wifi_page.page_alive = false;
    s_wifi_page.parent = nullptr;
    s_wifi_page.status_label = nullptr;
    s_wifi_page.mac_label = nullptr;
    s_wifi_page.scan_summary_label = nullptr;
    s_wifi_page.ap_list = nullptr;
    s_wifi_page.scan_button = nullptr;
    s_wifi_page.info_button = nullptr;
    s_wifi_page.overlay = nullptr;
    s_wifi_page.overlay_text = nullptr;
    s_wifi_page.overlay_retry_button = nullptr;
}

void create_wifi_overlay(lv_obj_t* parent)
{
    s_wifi_page.overlay = lv_obj_create(parent);
    lv_obj_remove_flag(s_wifi_page.overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wifi_page.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_wifi_page.overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_wifi_page.overlay, lv_pct(100), lv_pct(100));
    lv_obj_align(s_wifi_page.overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_page.overlay, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_wifi_page.overlay, 232, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_wifi_page.overlay, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_wifi_page.overlay, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(s_wifi_page.overlay, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_wifi_page.overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_wifi_page.overlay, 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_wifi_page.overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wifi_page.overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_wifi_page.overlay_text = add_label(s_wifi_page.overlay,
                                         "Initializing ESP32-C6 WiFi module...",
                                         COLOR_TEXT,
                                         380);
    lv_obj_set_style_text_align(s_wifi_page.overlay_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(s_wifi_page.overlay_text, LV_LABEL_LONG_WRAP);

    s_wifi_page.overlay_retry_button = add_button(s_wifi_page.overlay,
                                                  "Retry",
                                                  COLOR_PRIMARY,
                                                  0xFFFFFF,
                                                  190,
                                                  52);
    lv_obj_add_event_cb(s_wifi_page.overlay_retry_button, retry_button_event_cb, LV_EVENT_CLICKED, nullptr);
}

void build_content(lv_obj_t* parent, const CoreP4FunctionWindowConfig& config)
{
    (void)config;

    prepare_content(parent);
    s_wifi_page = WifiPageState{};
    s_wifi_page.parent = parent;
    s_wifi_page.page_alive = true;
    lv_obj_add_event_cb(parent, wifi_content_delete_event_cb, LV_EVENT_DELETE, nullptr);
    if (lv_obj_t* backdrop = lv_obj_get_parent(parent)) {
        if (lv_obj_get_child_count(backdrop) > 1) {
            if (lv_obj_t* header = lv_obj_get_child(backdrop, 1)) {
                if (lv_obj_get_child_count(header) > 0) {
                    s_wifi_page.info_button = lv_obj_get_child(header, 0);
                }
            }
        }
    }

    lv_obj_t* list_card = add_card(parent, 398);
    lv_obj_t* header_row = add_row(list_card, 52);
    lv_obj_set_style_pad_all(header_row, 4, LV_PART_MAIN);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* title_label = add_label(header_row, "Nearby Networks", COLOR_TEXT, 250);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);

    s_wifi_page.scan_button = add_button(header_row, "Scan AP", COLOR_PRIMARY, 0xFFFFFF, 124, 44);
    lv_obj_add_event_cb(s_wifi_page.scan_button, scan_button_event_cb, LV_EVENT_CLICKED, nullptr);

    s_wifi_page.scan_summary_label = add_label(list_card,
                                               "Ready when the ESP32-C6 module is initialized.",
                                               COLOR_MUTED);
    style_text_14(s_wifi_page.scan_summary_label);
    lv_label_set_long_mode(s_wifi_page.scan_summary_label, LV_LABEL_LONG_DOT);

    s_wifi_page.ap_list = lv_obj_create(list_card);
    lv_obj_set_width(s_wifi_page.ap_list, lv_pct(100));
    lv_obj_set_height(s_wifi_page.ap_list, AP_LIST_HEIGHT);
    lv_obj_set_style_bg_opa(s_wifi_page.ap_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_wifi_page.ap_list, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_wifi_page.ap_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_wifi_page.ap_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_wifi_page.ap_list, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_wifi_page.ap_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wifi_page.ap_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(s_wifi_page.ap_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_wifi_page.ap_list, LV_SCROLLBAR_MODE_AUTO);
    show_ap_message("Tap Scan AP to browse nearby WiFi networks.", COLOR_MUTED);

    create_wifi_overlay(parent);
    refresh_wifi_status();
    s_wifi_page.refresh_timer = lv_timer_create(wifi_refresh_timer_cb, WIFI_REFRESH_MS, nullptr);
    if (!wifi_ready() && !wifi_busy()) {
        start_wifi_init_task();
    }
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "WiFi",
    "click on ui_func1wifi1con",
    build_content,
    "Info",
    info_button_event_cb,
    nullptr,
    COLOR_PRIMARY,
    0xFFFFFF,
};

} // namespace

extern "C" void func2wifi2clicked2func(lv_event_t* e)
{
    corep4_ui_open_function_window(e, WINDOW_CONFIG);
}
