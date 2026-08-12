#include "ui_function_widgets.h"

#include "m5_corep4_pinmap.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_RTC";
constexpr uint32_t RTC_REFRESH_MS = 1000;

constexpr int32_t TIMER_PANEL_W = 432;
constexpr int32_t TIMER_PANEL_H = 190;
constexpr int32_t TIMER_PANEL_GAP = 8;
constexpr int32_t TIMER_PANEL_RADIUS = 8;
constexpr int32_t TIMER_HEADER_Y = 12;
constexpr int32_t TIMER_FIELD_Y = 48;
constexpr int32_t TIMER_FIELD_H = 128;
constexpr int32_t TIMER_FIELD_X = 10;
constexpr int32_t TIMER_FIELD_GAP = 6;
constexpr int32_t TIMER_SAVE_W = 92;
constexpr int32_t TIMER_SAVE_H = 34;
constexpr int32_t TIMER_SAVE_X = TIMER_PANEL_W - TIMER_SAVE_W - 12;
constexpr int32_t TIMER_SAVE_Y = 8;

constexpr uint32_t COLOR_DATE_ACCENT = 0x0F766E;
constexpr uint32_t COLOR_DATE_BG = 0xF0FDFA;
constexpr uint32_t COLOR_TIME_ACCENT = 0x2563EB;
constexpr uint32_t COLOR_TIME_BG = 0xEFF6FF;
constexpr uint32_t COLOR_NTP_BG = 0x7C3AED;
constexpr uint32_t COLOR_TILE_BG = 0xFFFFFF;
constexpr uint32_t COLOR_TILE_CONTROL_BG = 0xF8FAFC;
constexpr uint32_t COLOR_DISABLED_TEXT = 0x94A3B8;
constexpr const char* ALIYUN_NTP_SERVER = "ntp.aliyun.com";
constexpr uint32_t NTP_SYNC_TIMEOUT_MS = 6000;

enum class RtcEditField : uint8_t {
    YEAR,
    MONTH,
    DAY,
    HOUR,
    MINUTE,
    SECOND,
};

struct TimerFieldWidget {
    RtcEditField field = RtcEditField::YEAR;
    lv_obj_t* tile = nullptr;
    lv_obj_t* name_label = nullptr;
    lv_obj_t* value_label = nullptr;
    lv_obj_t* dec_button = nullptr;
    lv_obj_t* inc_button = nullptr;
};

struct TimerPageState {
    lv_obj_t* ntp_button = nullptr;
    lv_obj_t* date_save_button = nullptr;
    lv_obj_t* time_save_button = nullptr;
    TimerFieldWidget date_fields[3];
    TimerFieldWidget time_fields[3];
    lv_timer_t* timer = nullptr;
    TaskHandle_t ntp_task = nullptr;
    struct tm edit_time = {};
    bool edit_initialized = false;
    bool edit_dirty = false;
    bool ntp_available = false;
    volatile bool page_alive = false;
};

TimerPageState s_timer_page;

bool compile_time_to_tm(struct tm& out);
bool ensure_rtc_ready();
void ntp_button_event_cb(lv_event_t* e);
void ntp_header_button_ready(lv_obj_t* button);

const char* month_name_to_num(const char* mon)
{
    if (!mon) return "01";
    if (std::strcmp(mon, "Jan") == 0) return "01";
    if (std::strcmp(mon, "Feb") == 0) return "02";
    if (std::strcmp(mon, "Mar") == 0) return "03";
    if (std::strcmp(mon, "Apr") == 0) return "04";
    if (std::strcmp(mon, "May") == 0) return "05";
    if (std::strcmp(mon, "Jun") == 0) return "06";
    if (std::strcmp(mon, "Jul") == 0) return "07";
    if (std::strcmp(mon, "Aug") == 0) return "08";
    if (std::strcmp(mon, "Sep") == 0) return "09";
    if (std::strcmp(mon, "Oct") == 0) return "10";
    if (std::strcmp(mon, "Nov") == 0) return "11";
    if (std::strcmp(mon, "Dec") == 0) return "12";
    return "01";
}

void format_time(const struct tm& t, char* out, size_t out_len)
{
    std::snprintf(out, out_len,
                  "%04d-%02d-%02d %02d:%02d:%02d",
                  t.tm_year + 1900,
                  t.tm_mon + 1,
                  t.tm_mday,
                  t.tm_hour,
                  t.tm_min,
                  t.tm_sec);
}

void fill_weekday(struct tm& t)
{
    t.tm_isdst = -1;
    struct tm copy = t;
    time_t value = mktime(&copy);
    if (value != static_cast<time_t>(-1)) {
        t.tm_wday = copy.tm_wday;
    }
}

bool is_leap_year(int year)
{
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

int days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2) {
        return is_leap_year(year) ? 29 : 28;
    }
    if (month < 1 || month > 12) {
        return 31;
    }
    return days[month - 1];
}

const char* edit_field_name(RtcEditField field)
{
    switch (field) {
        case RtcEditField::YEAR:   return "Year";
        case RtcEditField::MONTH:  return "Month";
        case RtcEditField::DAY:    return "Day";
        case RtcEditField::HOUR:   return "Hour";
        case RtcEditField::MINUTE: return "Minute";
        case RtcEditField::SECOND: return "Second";
        default:                   return "Field";
    }
}

int value_for_field(RtcEditField field)
{
    switch (field) {
        case RtcEditField::YEAR:   return s_timer_page.edit_time.tm_year + 1900;
        case RtcEditField::MONTH:  return s_timer_page.edit_time.tm_mon + 1;
        case RtcEditField::DAY:    return s_timer_page.edit_time.tm_mday;
        case RtcEditField::HOUR:   return s_timer_page.edit_time.tm_hour;
        case RtcEditField::MINUTE: return s_timer_page.edit_time.tm_min;
        case RtcEditField::SECOND: return s_timer_page.edit_time.tm_sec;
        default:                   return 0;
    }
}

void format_field_value(RtcEditField field, char* out, size_t out_len)
{
    if (field == RtcEditField::YEAR) {
        std::snprintf(out, out_len, "%04d", value_for_field(field));
    } else {
        std::snprintf(out, out_len, "%02d", value_for_field(field));
    }
}

void normalize_edit_time()
{
    if (s_timer_page.edit_time.tm_year < 2020 - 1900) s_timer_page.edit_time.tm_year = 2020 - 1900;
    if (s_timer_page.edit_time.tm_year > 2099 - 1900) s_timer_page.edit_time.tm_year = 2099 - 1900;
    if (s_timer_page.edit_time.tm_mon < 0) s_timer_page.edit_time.tm_mon = 0;
    if (s_timer_page.edit_time.tm_mon > 11) s_timer_page.edit_time.tm_mon = 11;
    const int year = s_timer_page.edit_time.tm_year + 1900;
    const int month = s_timer_page.edit_time.tm_mon + 1;
    const int max_day = days_in_month(year, month);
    if (s_timer_page.edit_time.tm_mday < 1) s_timer_page.edit_time.tm_mday = 1;
    if (s_timer_page.edit_time.tm_mday > max_day) s_timer_page.edit_time.tm_mday = max_day;
    if (s_timer_page.edit_time.tm_hour < 0) s_timer_page.edit_time.tm_hour = 0;
    if (s_timer_page.edit_time.tm_hour > 23) s_timer_page.edit_time.tm_hour = 23;
    if (s_timer_page.edit_time.tm_min < 0) s_timer_page.edit_time.tm_min = 0;
    if (s_timer_page.edit_time.tm_min > 59) s_timer_page.edit_time.tm_min = 59;
    if (s_timer_page.edit_time.tm_sec < 0) s_timer_page.edit_time.tm_sec = 0;
    if (s_timer_page.edit_time.tm_sec > 59) s_timer_page.edit_time.tm_sec = 59;
}

lv_obj_t* create_abs_label(lv_obj_t* parent,
                           const char* text,
                           int32_t x,
                           int32_t y,
                           int32_t width,
                           int32_t height,
                           const lv_font_t* font,
                           uint32_t color,
                           lv_text_align_t align = LV_TEXT_ALIGN_LEFT)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    return label;
}

lv_obj_t* create_abs_button(lv_obj_t* parent,
                            const char* text,
                            int32_t x,
                            int32_t y,
                            int32_t width,
                            int32_t height,
                            uint32_t bg_color,
                            uint32_t text_color,
                            uint32_t border_color = COLOR_BORDER)
{
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(border_color), LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

void set_label_color(lv_obj_t* label, uint32_t color)
{
    if (label) {
        lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    }
}

void style_field_widget(TimerFieldWidget& widget, bool enabled)
{
    if (!widget.tile) {
        return;
    }

    const uint32_t text_color = enabled ? COLOR_TEXT : COLOR_DISABLED_TEXT;
    const uint32_t muted_color = enabled ? COLOR_MUTED : COLOR_DISABLED_TEXT;

    lv_obj_set_style_bg_color(widget.tile, lv_color_hex(COLOR_TILE_BG), LV_PART_MAIN);
    lv_obj_set_style_border_color(widget.tile, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_opa(widget.tile, enabled ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
    set_label_color(widget.name_label, muted_color);
    set_label_color(widget.value_label, text_color);

    lv_obj_t* buttons[] = {widget.dec_button, widget.inc_button};
    for (lv_obj_t* button : buttons) {
        if (!button) {
            continue;
        }
        lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_TILE_CONTROL_BG), LV_PART_MAIN);
        lv_obj_set_style_border_color(button, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
        set_button_enabled(button, enabled);
        lv_obj_t* label = lv_obj_get_child(button, 0);
        set_label_color(label, text_color);
    }
}

void update_field_value(TimerFieldWidget& widget)
{
    if (!widget.value_label) {
        return;
    }

    char value_buf[16];
    format_field_value(widget.field, value_buf, sizeof(value_buf));
    set_label_text(widget.value_label, value_buf);
}

void refresh_edit_time_ui()
{
    normalize_edit_time();
    for (auto& widget : s_timer_page.date_fields) {
        update_field_value(widget);
        style_field_widget(widget, true);
    }
    for (auto& widget : s_timer_page.time_fields) {
        update_field_value(widget);
        style_field_widget(widget, true);
    }
}

void set_timer_controls_enabled(bool enabled)
{
    set_button_enabled(s_timer_page.date_save_button, enabled);
    set_button_enabled(s_timer_page.time_save_button, enabled);

    for (auto& widget : s_timer_page.date_fields) {
        style_field_widget(widget, enabled);
    }
    for (auto& widget : s_timer_page.time_fields) {
        style_field_widget(widget, enabled);
    }
}

void seed_edit_time_if_needed()
{
    if (s_timer_page.edit_initialized) {
        return;
    }

    if (!compile_time_to_tm(s_timer_page.edit_time)) {
        s_timer_page.edit_time = {};
        s_timer_page.edit_time.tm_year = 2025 - 1900;
        s_timer_page.edit_time.tm_mon = 0;
        s_timer_page.edit_time.tm_mday = 1;
    }
    s_timer_page.edit_initialized = true;
    refresh_edit_time_ui();
}

void adjust_edit_field(RtcEditField field, int delta)
{
    switch (field) {
        case RtcEditField::YEAR:   s_timer_page.edit_time.tm_year += delta; break;
        case RtcEditField::MONTH:  s_timer_page.edit_time.tm_mon += delta; break;
        case RtcEditField::DAY:    s_timer_page.edit_time.tm_mday += delta; break;
        case RtcEditField::HOUR:   s_timer_page.edit_time.tm_hour += delta; break;
        case RtcEditField::MINUTE: s_timer_page.edit_time.tm_min += delta; break;
        case RtcEditField::SECOND: s_timer_page.edit_time.tm_sec += delta; break;
    }
    s_timer_page.edit_dirty = true;
    normalize_edit_time();
    refresh_edit_time_ui();
}

bool compile_time_to_tm(struct tm& out)
{
    char mon[4] = {};
    int day = 1;
    int year = 2025;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) {
        return false;
    }
    if (std::sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
        return false;
    }

    int month = std::atoi(month_name_to_num(mon));
    out = {};
    out.tm_year = year - 1900;
    out.tm_mon = month - 1;
    out.tm_mday = day;
    out.tm_hour = hour;
    out.tm_min = minute;
    out.tm_sec = second;
    fill_weekday(out);
    return true;
}

bool ensure_rtc_ready()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    if (!tool || !status) {
        return false;
    }
    if (status->rtc.rtc_initialized) {
        return true;
    }

    auto rtc_device_handle = tool->otool_tools.i2c_tools.get_i2c_device_handle(I2C_ADDR_RTC);
    bool ok = tool->rtc.init(tool->otool_tools.i2c_tools.get_i2c_bus_handle(),
                             &rtc_device_handle,
                             GPIO_NUM_NC);
    if (ok) {
        tool->rtc.initBat();
        tool->rtc.clearIrqFlags();
        status->rtc.rtc_initialized = true;
        status->rtc.timer_irq_enabled = false;
    }
    return ok;
}

void refresh_timer_status()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    if (!tool || !status) {
        set_timer_controls_enabled(false);
        return;
    }

    set_timer_controls_enabled(status->rtc.rtc_initialized);
}

void refresh_rtc_time()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    if (!tool || !status || !ensure_rtc_ready()) {
        seed_edit_time_if_needed();
        refresh_timer_status();
        return;
    }

    struct tm now = {};
    tool->rtc.getTime(&now);
    status->rtc.current_time = now;

    if (!s_timer_page.edit_initialized || !s_timer_page.edit_dirty) {
        s_timer_page.edit_time = now;
        s_timer_page.edit_initialized = true;
        refresh_edit_time_ui();
    }

    refresh_timer_status();
}

void rtc_timer_cb(lv_timer_t* /*timer*/)
{
    if (s_timer_page.page_alive) {
        refresh_rtc_time();
    }
}

void set_rtc_time(const struct tm& new_time, const char* label)
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    if (!tool || !status || !ensure_rtc_ready()) {
        ESP_LOGW(TAG, "%s save failed: RX8130 is not ready.", label);
        return;
    }

    struct tm t = new_time;
    fill_weekday(t);
    tool->rtc.setTime(&t);
    status->rtc.current_time = t;
    s_timer_page.edit_dirty = false;

    char time_buf[64];
    format_time(t, time_buf, sizeof(time_buf));
    ESP_LOGI(TAG, "%s saved: %s", label, time_buf);
    refresh_rtc_time();
}

bool local_time_from_system_clock(struct tm& out)
{
    time_t now = 0;
    time(&now);
    if (now < 1577836800) {
        return false;
    }

    setenv("TZ", "CST-8", 1);
    tzset();
    if (localtime_r(&now, &out) == nullptr) {
        return false;
    }

    const int year = out.tm_year + 1900;
    return year >= 2020 && year < 2099;
}

esp_err_t fetch_aliyun_ntp_time(struct tm& out)
{
    esp_netif_sntp_deinit();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ALIYUN_NTP_SERVER);
    config.wait_for_sync = true;
    config.start = true;

    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret == ESP_ERR_INVALID_STATE) {
        esp_netif_sntp_deinit();
        ret = esp_netif_sntp_init(&config);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Aliyun NTP init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_SYNC_TIMEOUT_MS));
    esp_netif_sntp_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Aliyun NTP sync failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!local_time_from_system_clock(out)) {
        ESP_LOGW(TAG, "Aliyun NTP sync produced invalid local time");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void set_ntp_button_visible_locked(bool visible)
{
    if (!s_timer_page.ntp_button) {
        return;
    }
    if (visible) {
        set_button_text(s_timer_page.ntp_button, "NTP");
        set_button_enabled(s_timer_page.ntp_button, true);
        lv_obj_clear_flag(s_timer_page.ntp_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_timer_page.ntp_button, LV_OBJ_FLAG_HIDDEN);
        set_button_enabled(s_timer_page.ntp_button, false);
    }
}

void ntp_probe_task(void* /*param*/)
{
    bool available = false;
    struct tm ntp_time = {};

    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    if (tool && status) {
        if (!status->ethernet.ethernet_connected || !status->ethernet.got_ip) {
            tool->wait_for_ethernet_connection(4000);
            status = tool->get_device_status();
        }

        if (status && status->ethernet.ethernet_connected && status->ethernet.got_ip) {
            available = (fetch_aliyun_ntp_time(ntp_time) == ESP_OK);
        } else {
            ESP_LOGI(TAG, "Aliyun NTP probe skipped: Ethernet is not ready");
        }
    }

    if (lvgl_port_lock(1000)) {
        if (s_timer_page.page_alive) {
            s_timer_page.ntp_available = available;
            set_ntp_button_visible_locked(available);
        }
        lvgl_port_unlock();
    }

    if (available) {
        char time_buf[64];
        format_time(ntp_time, time_buf, sizeof(time_buf));
        ESP_LOGI(TAG, "Aliyun NTP is available: %s", time_buf);
    }

    s_timer_page.ntp_task = nullptr;
    vTaskDelete(nullptr);
}

void start_ntp_probe_task()
{
    if (s_timer_page.ntp_task != nullptr) {
        return;
    }

    BaseType_t ret = xTaskCreate(ntp_probe_task,
                                 "rtc_ntp_probe",
                                 4096,
                                 nullptr,
                                 5,
                                 &s_timer_page.ntp_task);
    if (ret != pdPASS) {
        s_timer_page.ntp_task = nullptr;
        ESP_LOGW(TAG, "Failed to create Aliyun NTP probe task");
    }
}

void ntp_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s_timer_page.ntp_available) {
        return;
    }

    struct tm ntp_time = {};
    if (!local_time_from_system_clock(ntp_time)) {
        s_timer_page.ntp_available = false;
        set_ntp_button_visible_locked(false);
        ESP_LOGW(TAG, "NTP button ignored: system clock is not synchronized");
        return;
    }

    set_rtc_time(ntp_time, "NTP");
}

void ntp_header_button_ready(lv_obj_t* button)
{
    s_timer_page.ntp_button = button;
    set_ntp_button_visible_locked(false);
}

void field_adjust_event_cb(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED &&
        code != LV_EVENT_LONG_PRESSED &&
        code != LV_EVENT_LONG_PRESSED_REPEAT) {
        return;
    }

    uintptr_t raw = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    RtcEditField field = static_cast<RtcEditField>(raw >> 1);
    const int delta = (raw & 0x01U) ? 1 : -1;
    adjust_edit_field(field, delta);
}

void save_date_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    normalize_edit_time();
    set_rtc_time(s_timer_page.edit_time, "Date");
}

void save_time_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    normalize_edit_time();
    set_rtc_time(s_timer_page.edit_time, "Time");
}

lv_obj_t* create_timer_panel(lv_obj_t* parent,
                             const char* title,
                             uint32_t accent,
                             uint32_t bg_color)
{
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, TIMER_PANEL_H);
    lv_obj_set_style_bg_color(panel, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(accent), LV_PART_MAIN);
    lv_obj_set_style_border_opa(panel, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, TIMER_PANEL_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);

    create_abs_label(panel, title, 14, TIMER_HEADER_Y, 78, 24,
                     &lv_font_montserrat_20, accent);
    return panel;
}

void create_field_widget(TimerFieldWidget& widget,
                         lv_obj_t* parent,
                         RtcEditField field,
                         int32_t x,
                         int32_t width)
{
    widget.field = field;
    widget.tile = lv_obj_create(parent);
    lv_obj_remove_flag(widget.tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(widget.tile, x, TIMER_FIELD_Y);
    lv_obj_set_size(widget.tile, width, TIMER_FIELD_H);
    lv_obj_set_style_bg_color(widget.tile, lv_color_hex(COLOR_TILE_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget.tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget.tile, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(widget.tile, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(widget.tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(widget.tile, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(widget.tile, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget.tile, 0, LV_PART_MAIN);

    widget.name_label = create_abs_label(widget.tile,
                                         edit_field_name(field),
                                         0,
                                         10,
                                         width,
                                         20,
                                         &lv_font_montserrat_16,
                                         COLOR_MUTED,
                                         LV_TEXT_ALIGN_CENTER);
    widget.value_label = create_abs_label(widget.tile,
                                          "--",
                                          0,
                                          24,
                                          width,
                                          62,
                                          &lv_font_montserrat_44,
                                          COLOR_TEXT,
                                          LV_TEXT_ALIGN_CENTER);

    const int32_t control_w = 50;
    const int32_t control_h = 34;
    const int32_t control_y = 84;
    widget.dec_button = create_abs_button(widget.tile,
                                          "-",
                                          8,
                                          control_y,
                                          control_w,
                                          control_h,
                                          COLOR_TILE_CONTROL_BG,
                                          COLOR_TEXT);
    widget.inc_button = create_abs_button(widget.tile,
                                          "+",
                                          width - control_w - 8,
                                          control_y,
                                          control_w,
                                          control_h,
                                          COLOR_TILE_CONTROL_BG,
                                          COLOR_TEXT);
    if (lv_obj_t* label = lv_obj_get_child(widget.dec_button, 0)) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_center(label);
    }
    if (lv_obj_t* label = lv_obj_get_child(widget.inc_button, 0)) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_center(label);
    }

    const uintptr_t field_raw = static_cast<uintptr_t>(static_cast<uint8_t>(field));
    lv_obj_add_event_cb(widget.dec_button,
                        field_adjust_event_cb,
                        LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(field_raw << 1));
    lv_obj_add_event_cb(widget.dec_button,
                        field_adjust_event_cb,
                        LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void*>(field_raw << 1));
    lv_obj_add_event_cb(widget.dec_button,
                        field_adjust_event_cb,
                        LV_EVENT_LONG_PRESSED_REPEAT,
                        reinterpret_cast<void*>(field_raw << 1));
    lv_obj_add_event_cb(widget.inc_button,
                        field_adjust_event_cb,
                        LV_EVENT_CLICKED,
                        reinterpret_cast<void*>((field_raw << 1) | 0x01U));
    lv_obj_add_event_cb(widget.inc_button,
                        field_adjust_event_cb,
                        LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void*>((field_raw << 1) | 0x01U));
    lv_obj_add_event_cb(widget.inc_button,
                        field_adjust_event_cb,
                        LV_EVENT_LONG_PRESSED_REPEAT,
                        reinterpret_cast<void*>((field_raw << 1) | 0x01U));
}

void build_date_panel(lv_obj_t* parent)
{
    lv_obj_t* panel = create_timer_panel(parent, "DATE", COLOR_DATE_ACCENT, COLOR_DATE_BG);
    s_timer_page.date_save_button = create_abs_button(panel,
                                                      "Save",
                                                      TIMER_SAVE_X,
                                                      TIMER_SAVE_Y,
                                                      TIMER_SAVE_W,
                                                      TIMER_SAVE_H,
                                                      COLOR_DATE_ACCENT,
                                                      0xFFFFFF,
                                                      COLOR_DATE_ACCENT);
    lv_obj_add_event_cb(s_timer_page.date_save_button,
                        save_date_button_event_cb,
                        LV_EVENT_CLICKED,
                        nullptr);

    constexpr int32_t year_w = 144;
    constexpr int32_t month_w = 126;
    constexpr int32_t day_w = 126;
    create_field_widget(s_timer_page.date_fields[0], panel,
                        RtcEditField::YEAR,
                        TIMER_FIELD_X,
                        year_w);
    create_field_widget(s_timer_page.date_fields[1], panel,
                        RtcEditField::MONTH,
                        TIMER_FIELD_X + year_w + TIMER_FIELD_GAP,
                        month_w);
    create_field_widget(s_timer_page.date_fields[2], panel,
                        RtcEditField::DAY,
                        TIMER_FIELD_X + year_w + TIMER_FIELD_GAP + month_w + TIMER_FIELD_GAP,
                        day_w);
}

void build_time_panel(lv_obj_t* parent)
{
    lv_obj_t* panel = create_timer_panel(parent, "TIME", COLOR_TIME_ACCENT, COLOR_TIME_BG);
    s_timer_page.time_save_button = create_abs_button(panel,
                                                      "Save",
                                                      TIMER_SAVE_X,
                                                      TIMER_SAVE_Y,
                                                      TIMER_SAVE_W,
                                                      TIMER_SAVE_H,
                                                      COLOR_TIME_ACCENT,
                                                      0xFFFFFF,
                                                      COLOR_TIME_ACCENT);
    lv_obj_add_event_cb(s_timer_page.time_save_button,
                        save_time_button_event_cb,
                        LV_EVENT_CLICKED,
                        nullptr);

    constexpr int32_t field_w = 132;
    create_field_widget(s_timer_page.time_fields[0], panel,
                        RtcEditField::HOUR,
                        TIMER_FIELD_X,
                        field_w);
    create_field_widget(s_timer_page.time_fields[1], panel,
                        RtcEditField::MINUTE,
                        TIMER_FIELD_X + field_w + TIMER_FIELD_GAP,
                        field_w);
    create_field_widget(s_timer_page.time_fields[2], panel,
                        RtcEditField::SECOND,
                        TIMER_FIELD_X + (field_w + TIMER_FIELD_GAP) * 2,
                        field_w);
}

void timer_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (s_timer_page.timer) {
        lv_timer_delete(s_timer_page.timer);
        s_timer_page.timer = nullptr;
    }
    s_timer_page.page_alive = false;
    s_timer_page.ntp_button = nullptr;
    s_timer_page.date_save_button = nullptr;
    s_timer_page.time_save_button = nullptr;
    for (auto& widget : s_timer_page.date_fields) {
        widget = {};
    }
    for (auto& widget : s_timer_page.time_fields) {
        widget = {};
    }
    s_timer_page.edit_initialized = false;
    s_timer_page.edit_dirty = false;
    s_timer_page.ntp_available = false;
}

void build_content(lv_obj_t* parent, const CoreP4FunctionWindowConfig& config)
{
    (void)config;

    prepare_content(parent);
    lv_obj_set_style_pad_row(parent, TIMER_PANEL_GAP, LV_PART_MAIN);
    s_timer_page.page_alive = true;
    s_timer_page.edit_initialized = false;
    s_timer_page.edit_dirty = false;
    s_timer_page.ntp_available = false;
    set_ntp_button_visible_locked(false);
    lv_obj_add_event_cb(parent, timer_content_delete_event_cb, LV_EVENT_DELETE, nullptr);

    build_date_panel(parent);
    build_time_panel(parent);

    seed_edit_time_if_needed();
    set_timer_controls_enabled(false);
    refresh_rtc_time();
    s_timer_page.timer = lv_timer_create(rtc_timer_cb, RTC_REFRESH_MS, nullptr);
    start_ntp_probe_task();
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "Timer",
    "click on ui_func1timer1con",
    build_content,
    "NTP",
    ntp_button_event_cb,
    nullptr,
    COLOR_NTP_BG,
    0xFFFFFF,
    true,
    ntp_header_button_ready,
};

} // namespace

extern "C" void func2timer2clicked(lv_event_t* e)
{
    corep4_ui_open_function_window(e, WINDOW_CONFIG);
}
