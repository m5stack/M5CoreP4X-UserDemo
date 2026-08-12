#include "ui_function_widgets.h"

#include <cstdio>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_SLEEP";

struct SleepPageState {
    lv_obj_t* status_label = nullptr;
    lv_obj_t* result_label = nullptr;
    lv_obj_t* btn_sleep_button = nullptr;
    lv_obj_t* rtc_wake_button = nullptr;
    lv_obj_t* touch_wake_button = nullptr;
    lv_obj_t* bmi_wake_button = nullptr;
    lv_obj_t* rtc_10s_button = nullptr;
    lv_obj_t* rtc_60s_button = nullptr;
    lv_obj_t* rtc_300s_button = nullptr;
    lv_obj_t* deep_sleep_button = nullptr;
    volatile bool page_alive = false;
    volatile bool task_running = false;
};

SleepPageState s_sleep_page;

StatusParam PARAM_BTN_SLEEP = StatusParam::SLEEP_BTN_ENABLE;
StatusParam PARAM_RTC_WAKE = StatusParam::SLEEP_WAKE_RTC;
StatusParam PARAM_TOUCH_WAKE = StatusParam::SLEEP_WAKE_TOUCH;
StatusParam PARAM_BMI_WAKE = StatusParam::SLEEP_WAKE_BMI;
uint32_t RTC_SEC_10 = 10;
uint32_t RTC_SEC_60 = 60;
uint32_t RTC_SEC_300 = 300;

const char* sleep_param_name(StatusParam param)
{
    switch (param) {
        case StatusParam::SLEEP_BTN_ENABLE: return "BTN sleep";
        case StatusParam::SLEEP_WAKE_RTC: return "RTC wake";
        case StatusParam::SLEEP_WAKE_TOUCH: return "Touch wake";
        case StatusParam::SLEEP_WAKE_BMI: return "IMU wake";
        default: return "Sleep option";
    }
}

bool read_sleep_bool(const device_realtime_status_t* status, StatusParam param)
{
    if (!status) {
        return false;
    }

    switch (param) {
        case StatusParam::SLEEP_BTN_ENABLE: return status->sleep.btn_sleep_enable;
        case StatusParam::SLEEP_WAKE_RTC: return status->sleep.wake_rtc_enable;
        case StatusParam::SLEEP_WAKE_TOUCH: return status->sleep.wake_touch_enable;
        case StatusParam::SLEEP_WAKE_BMI: return status->sleep.wake_bmi_enable;
        default: return false;
    }
}

void set_button_colors(lv_obj_t* button, uint32_t bg_color, uint32_t text_color)
{
    if (!button) {
        return;
    }

    lv_obj_set_style_bg_color(button, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (label) {
        lv_obj_set_style_text_color(label, lv_color_hex(text_color), LV_PART_MAIN);
    }
}

void set_toggle_button(lv_obj_t* button, const char* name, bool enabled)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s %s", name, enabled ? "ON" : "OFF");
    set_button_text(button, buf);
    set_button_colors(button,
                      enabled ? COLOR_SUCCESS : COLOR_BUTTON_BG,
                      enabled ? 0xFFFFFF : COLOR_TEXT);
}

void set_rtc_seconds_button(lv_obj_t* button,
                            const char* text,
                            bool selected)
{
    set_button_text(button, text);
    set_button_colors(button,
                      selected ? COLOR_PRIMARY : COLOR_BUTTON_BG,
                      selected ? 0xFFFFFF : COLOR_TEXT);
}

void set_result_text(const char* text, uint32_t color)
{
    set_label_text(s_sleep_page.result_label, text);
    if (s_sleep_page.result_label) {
        lv_obj_set_style_text_color(s_sleep_page.result_label,
                                    lv_color_hex(color),
                                    LV_PART_MAIN);
    }
}

void set_sleep_controls_enabled(bool enabled)
{
    set_button_enabled(s_sleep_page.btn_sleep_button, enabled);
    set_button_enabled(s_sleep_page.rtc_wake_button, enabled);
    set_button_enabled(s_sleep_page.touch_wake_button, enabled);
    set_button_enabled(s_sleep_page.bmi_wake_button, enabled);
    set_button_enabled(s_sleep_page.rtc_10s_button, enabled);
    set_button_enabled(s_sleep_page.rtc_60s_button, enabled);
    set_button_enabled(s_sleep_page.rtc_300s_button, enabled);
    set_button_enabled(s_sleep_page.deep_sleep_button, enabled);
}

void refresh_sleep_ui()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;

    if (!status) {
        set_label_text(s_sleep_page.status_label, "Device tool: not ready");
        set_sleep_controls_enabled(false);
        return;
    }

    char status_buf[80];
    std::snprintf(status_buf, sizeof(status_buf),
                  "Task: %s  |  PM1 button wake always on",
                  s_sleep_page.task_running ? "preparing" : "idle");
    set_label_text(s_sleep_page.status_label, status_buf);

    set_toggle_button(s_sleep_page.btn_sleep_button,   "BTN",   status->sleep.btn_sleep_enable);
    set_toggle_button(s_sleep_page.rtc_wake_button,    "RTC",   status->sleep.wake_rtc_enable);
    set_toggle_button(s_sleep_page.touch_wake_button,  "Touch", status->sleep.wake_touch_enable);
    set_toggle_button(s_sleep_page.bmi_wake_button,    "IMU",   status->sleep.wake_bmi_enable);

    set_rtc_seconds_button(s_sleep_page.rtc_10s_button,  "10 s",
        status->sleep.wake_rtc_enable && status->sleep.wake_rtc_seconds == 10);
    set_rtc_seconds_button(s_sleep_page.rtc_60s_button,  "60 s",
        status->sleep.wake_rtc_enable && status->sleep.wake_rtc_seconds == 60);
    set_rtc_seconds_button(s_sleep_page.rtc_300s_button, "300 s",
        status->sleep.wake_rtc_enable && status->sleep.wake_rtc_seconds == 300);

    set_sleep_controls_enabled(!s_sleep_page.task_running);
}

void update_sleep_ui_from_task(const char* result_text,
                               uint32_t result_color,
                               bool refresh_controls)
{
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock LVGL for sleep update");
        return;
    }

    if (s_sleep_page.page_alive) {
        set_result_text(result_text, result_color);
        if (refresh_controls) {
            refresh_sleep_ui();
        }
    }

    lvgl_port_unlock();
}

void deep_sleep_task(void* param)
{
    (void)param;

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        s_sleep_page.task_running = false;
        update_sleep_ui_from_task("Deep sleep failed: device tool is not ready.",
                                  COLOR_ERROR,
                                  true);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Deep sleep requested from function window");
    update_sleep_ui_from_task("Preparing rails, touch, RTC and wake sources...",
                              COLOR_WARNING,
                              true);

    esp_err_t ret = tool->sleep_before_work();
    if (ret != ESP_OK) {
        char result_buf[128];
        std::snprintf(result_buf, sizeof(result_buf),
                      "Deep sleep prep failed: %s.",
                      esp_err_to_name(ret));
        ESP_LOGW(TAG, "%s", result_buf);
        s_sleep_page.task_running = false;
        update_sleep_ui_from_task(result_buf, COLOR_ERROR, true);
        vTaskDelete(nullptr);
        return;
    }

    ret = tool->enter_deep_sleep();

    char result_buf[128];
    std::snprintf(result_buf, sizeof(result_buf),
                  "Deep sleep failed: %s.",
                  esp_err_to_name(ret));
    ESP_LOGW(TAG, "%s", result_buf);
    s_sleep_page.task_running = false;
    update_sleep_ui_from_task(result_buf, COLOR_ERROR, true);

    vTaskDelete(nullptr);
}

void toggle_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_sleep_page.task_running) {
        return;
    }

    auto* param = static_cast<StatusParam*>(lv_event_get_user_data(e));
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    if (!param || !tool || !status) {
        set_result_text("Update failed: device tool is not ready.", COLOR_ERROR);
        return;
    }

    const bool next = !read_sleep_bool(status, *param);
    esp_err_t ret = tool->set_status(*param,
                                     next,
                                     StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
    if (ret == ESP_OK) {
        char result_buf[96];
        std::snprintf(result_buf, sizeof(result_buf),
                      "%s set to %s.",
                      sleep_param_name(*param),
                      next ? "ON" : "OFF");
        set_result_text(result_buf, COLOR_SUCCESS);
    } else {
        char result_buf[96];
        std::snprintf(result_buf, sizeof(result_buf),
                      "%s update failed: %s.",
                      sleep_param_name(*param),
                      esp_err_to_name(ret));
        set_result_text(result_buf, COLOR_ERROR);
    }

    refresh_sleep_ui();
}

void rtc_seconds_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_sleep_page.task_running) {
        return;
    }

    auto* seconds = static_cast<uint32_t*>(lv_event_get_user_data(e));
    auto* tool = corep4_ui_sync_get_tool();
    if (!seconds || !tool) {
        set_result_text("RTC seconds update failed: device tool is not ready.", COLOR_ERROR);
        return;
    }

    esp_err_t ret = tool->set_status(StatusParam::SLEEP_WAKE_RTC_SEC,
                                     *seconds,
                                     StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
    if (ret == ESP_OK) {
        ret = tool->set_status(StatusParam::SLEEP_WAKE_RTC,
                               true,
                               StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
    }

    if (ret == ESP_OK) {
        char result_buf[96];
        std::snprintf(result_buf, sizeof(result_buf),
                      "RTC wake enabled after %lu seconds.",
                      static_cast<unsigned long>(*seconds));
        set_result_text(result_buf, COLOR_SUCCESS);
    } else {
        char result_buf[96];
        std::snprintf(result_buf, sizeof(result_buf),
                      "RTC seconds update failed: %s.",
                      esp_err_to_name(ret));
        set_result_text(result_buf, COLOR_ERROR);
    }

    refresh_sleep_ui();
}

void deep_sleep_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_sleep_page.task_running) {
        return;
    }

    s_sleep_page.task_running = true;
    refresh_sleep_ui();
    set_result_text("Deep sleep task starting...", COLOR_WARNING);

    BaseType_t task_ret = xTaskCreate(deep_sleep_task,
                                      "func_sleep",
                                      4096,
                                      nullptr,
                                      configMAX_PRIORITIES - 4,
                                      nullptr);
    if (task_ret != pdPASS) {
        s_sleep_page.task_running = false;
        refresh_sleep_ui();
        set_result_text("Deep sleep failed: cannot create worker task.", COLOR_ERROR);
    }
}

void sleep_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    s_sleep_page.page_alive = false;
    s_sleep_page.status_label = nullptr;
    s_sleep_page.result_label = nullptr;
    s_sleep_page.btn_sleep_button = nullptr;
    s_sleep_page.rtc_wake_button = nullptr;
    s_sleep_page.touch_wake_button = nullptr;
    s_sleep_page.bmi_wake_button = nullptr;
    s_sleep_page.rtc_10s_button = nullptr;
    s_sleep_page.rtc_60s_button = nullptr;
    s_sleep_page.rtc_300s_button = nullptr;
    s_sleep_page.deep_sleep_button = nullptr;
}

void build_content(lv_obj_t* parent, const CoreP4FunctionWindowConfig& config)
{
    (void)config;

    prepare_content(parent);
    s_sleep_page.page_alive = true;
    lv_obj_add_event_cb(parent, sleep_content_delete_event_cb, LV_EVENT_DELETE, nullptr);

    // 单张卡片包含 5 行
    // 可用内部高度 390px，总占用 350px（行高/间距均匀分配，用到 90%）
    lv_obj_t* card = add_card(parent, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(card, 10, LV_PART_MAIN);   // 覆盖默认 6px 行间距

    // Row 1 (h=36): 状态 — 任务状态 + PM1 固定唤醒说明
    lv_obj_t* row1 = add_row(card, 36);
    add_label(row1, "Status", COLOR_MUTED, 110);
    s_sleep_page.status_label = add_label(row1,
                                           "Task: idle  |  PM1 button wake always on",
                                           COLOR_TEXT, 290);
    lv_label_set_long_mode(s_sleep_page.status_label, LV_LABEL_LONG_DOT);

    // Row 2 (h=46): 按键休眠 — 按住电源键进入休眠
    lv_obj_t* row2 = add_row(card, 46);
    add_label(row2, "Button Sleep", COLOR_MUTED, 110);
    s_sleep_page.btn_sleep_button = add_button(row2, "BTN --",
                                                COLOR_BUTTON_BG, COLOR_TEXT, 120);
    lv_obj_add_event_cb(s_sleep_page.btn_sleep_button,
                        toggle_button_event_cb, LV_EVENT_CLICKED, &PARAM_BTN_SLEEP);

    // Row 3 (h=46): RTC 定时唤醒
    // 110(label)+78(RTC)+64(10s)+64(60s)+64(300s)+4*8(gap) = 412px = 卡片内宽
    lv_obj_t* row3 = add_row(card, 46);
    add_label(row3, "RTC Wake", COLOR_MUTED, 110);
    s_sleep_page.rtc_wake_button  = add_button(row3, "RTC --",
                                                COLOR_BUTTON_BG, COLOR_TEXT, 78);
    s_sleep_page.rtc_10s_button   = add_button(row3, "10 s",
                                                COLOR_BUTTON_BG, COLOR_TEXT, 64);
    s_sleep_page.rtc_60s_button   = add_button(row3, "60 s",
                                                COLOR_BUTTON_BG, COLOR_TEXT, 64);
    s_sleep_page.rtc_300s_button  = add_button(row3, "300 s",
                                                COLOR_BUTTON_BG, COLOR_TEXT, 64);
    lv_obj_add_event_cb(s_sleep_page.rtc_wake_button,  toggle_button_event_cb,
                        LV_EVENT_CLICKED, &PARAM_RTC_WAKE);
    lv_obj_add_event_cb(s_sleep_page.rtc_10s_button,   rtc_seconds_button_event_cb,
                        LV_EVENT_CLICKED, &RTC_SEC_10);
    lv_obj_add_event_cb(s_sleep_page.rtc_60s_button,   rtc_seconds_button_event_cb,
                        LV_EVENT_CLICKED, &RTC_SEC_60);
    lv_obj_add_event_cb(s_sleep_page.rtc_300s_button,  rtc_seconds_button_event_cb,
                        LV_EVENT_CLICKED, &RTC_SEC_300);

    // Row 4 (h=46): 传感器唤醒 — 触摸屏或 IMU 运动触发
    lv_obj_t* row4 = add_row(card, 46);
    add_label(row4, "Sensor Wake", COLOR_MUTED, 110);
    s_sleep_page.touch_wake_button = add_button(row4, "Touch --",
                                                 COLOR_BUTTON_BG, COLOR_TEXT, 120);
    s_sleep_page.bmi_wake_button   = add_button(row4, "IMU --",
                                                 COLOR_BUTTON_BG, COLOR_TEXT, 110);
    lv_obj_add_event_cb(s_sleep_page.touch_wake_button,
                        toggle_button_event_cb, LV_EVENT_CLICKED, &PARAM_TOUCH_WAKE);
    lv_obj_add_event_cb(s_sleep_page.bmi_wake_button,
                        toggle_button_event_cb, LV_EVENT_CLICKED, &PARAM_BMI_WAKE);

    // Row 5 (h=36): 结果 — 最近操作反馈
    lv_obj_t* row5 = add_row(card, 36);
    add_label(row5, "Result", COLOR_MUTED, 110);
    s_sleep_page.result_label = add_label(row5,
                                           "Set wake sources, then tap Deep Sleep.",
                                           COLOR_MUTED, 290);
    lv_label_set_long_mode(s_sleep_page.result_label, LV_LABEL_LONG_DOT);

    // 底部全宽 Deep Sleep 按键 (h=60)，共 350px / 390px = 90%
    s_sleep_page.deep_sleep_button = add_button(parent, "Deep Sleep",
                                                  COLOR_PRIMARY, 0xFFFFFF,
                                                  lv_pct(100), 60);
    lv_obj_add_event_cb(s_sleep_page.deep_sleep_button,
                        deep_sleep_button_event_cb, LV_EVENT_CLICKED, nullptr);

    refresh_sleep_ui();
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "Sleep",
    "click on ui_func1sleep1con",
    build_content,
};

} // namespace

extern "C" void func2sleep2clicked2func(lv_event_t* e)
{
    corep4_ui_open_function_window(e, WINDOW_CONFIG);
}
