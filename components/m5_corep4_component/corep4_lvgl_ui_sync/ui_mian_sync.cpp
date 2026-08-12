#include "corep4_lvgl_ui_sync_internal.h"
#include "audio_sr_afe.h"
#include "esp_heap_caps.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr const char* TAG = "UI_MIAN";

lv_timer_t* s_audio_status_timer = nullptr;

typedef enum {
    REC_STATE_IDLE = 0,
    REC_STATE_RECORDING,
    REC_STATE_PAUSED,
    REC_STATE_PLAYING
} rec_state_t;

rec_state_t s_rec_state = REC_STATE_IDLE;
int16_t* s_rec_buffer = nullptr;
size_t s_rec_buffer_size = 0;
size_t s_rec_buffer_used = 0;
TaskHandle_t s_rec_accum_task_handle = nullptr;
volatile bool s_rec_accum_stop_flag = false;
lv_timer_t* s_rec_longpress_timer = nullptr;
bool s_rec_longpress_handled = false;
lv_timer_t* s_rec_playback_timer = nullptr;

constexpr size_t REC_BUFFER_MAX_BYTES = 2 * 1024 * 1024;
constexpr uint32_t REC_LONGPRESS_MS = 3000;
constexpr uint32_t REC_ACCUM_READ_TIMEOUT = 100;
constexpr size_t REC_ACCUM_CHUNK_SAMPLES = 320;

void rec_enter_idle();
void rec_enter_recording();
void rec_enter_paused();
void rec_enter_playing();

void set_sensor_button_colors(sensor_type_t sensor)
{
    lv_obj_set_style_bg_color(ui_btnacc,
                              lv_color_hex(sensor == SENSOR_TYPE_ACC ? 0x3C88FF : 0xFFFFFF),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_btngry,
                              lv_color_hex(sensor == SENSOR_TYPE_GRY ? 0x3C88FF : 0xFFFFFF),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_btnmag,
                              lv_color_hex(sensor == SENSOR_TYPE_MAG ? 0x3C88FF : 0xFFFFFF),
                              LV_PART_MAIN);
}

void audio_status_timer_cb(lv_timer_t* timer)
{
    (void)timer;

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        return;
    }

    auto* pb = tool->otool_tools.audio.get_playback();
    if (!pb) {
        ESP_LOGE(TAG, "Cannot control audio: playback is NULL");
        return;
    }
    bool is_playing = pb && pb->is_async_playback_running();

    if (!is_playing && ui_esbtnstartlab) {
        const char* current_text = lv_label_get_text(ui_esbtnstartlab);
        if (current_text && std::strcmp(current_text, "STOP") == 0) {
            lv_label_set_text(ui_esbtnstartlab, "START");
            ESP_LOGI(TAG, "Audio playback completed, button restored to START");
        }
    }
}

void rec_update_ui()
{
    if (!ui_esbtnreclab || !ui_esbtnrec) {
        return;
    }

    switch (s_rec_state) {
        case REC_STATE_IDLE:
            lv_label_set_text(ui_esbtnreclab, "REC");
            lv_obj_set_style_bg_color(ui_esbtnrec, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            break;
        case REC_STATE_RECORDING:
            lv_label_set_text(ui_esbtnreclab, "REC..");
            lv_obj_set_style_bg_color(ui_esbtnrec, lv_color_hex(0xFF3838), LV_PART_MAIN);
            break;
        case REC_STATE_PAUSED:
            lv_label_set_text(ui_esbtnreclab, "PAUSE");
            lv_obj_set_style_bg_color(ui_esbtnrec, lv_color_hex(0xFFD700), LV_PART_MAIN);
            break;
        case REC_STATE_PLAYING:
            lv_label_set_text(ui_esbtnreclab, "PLAY");
            lv_obj_set_style_bg_color(ui_esbtnrec, lv_color_hex(0x38FF38), LV_PART_MAIN);
            break;
    }
}

void rec_accumulator_task(void* param)
{
    (void)param;

    int16_t chunk[REC_ACCUM_CHUNK_SAMPLES];
    ESP_LOGI("REC_ACCUM", "Accumulator task started");

    auto* tool = corep4_ui_sync_get_tool();
    audio_sr_afe* afe = tool ? tool->otool_tools.audio.get_sr_afe() : nullptr;
    if (!afe) {
        ESP_LOGE("REC_ACCUM", "sr_afe is NULL, task exits");
        s_rec_accum_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    while (!s_rec_accum_stop_flag) {
        int bytes = afe->aec_session_read(chunk, sizeof(chunk), REC_ACCUM_READ_TIMEOUT);
        if (bytes > 0 && s_rec_buffer) {
            if (s_rec_buffer_used + static_cast<size_t>(bytes) <= s_rec_buffer_size) {
                std::memcpy(reinterpret_cast<uint8_t*>(s_rec_buffer) + s_rec_buffer_used, chunk, bytes);
                s_rec_buffer_used += bytes;
            } else {
                ESP_LOGW("REC_ACCUM", "Buffer full (%.1f KB), stopping accumulation",
                         s_rec_buffer_used / 1024.0f);
                break;
            }
        }
    }

    ESP_LOGI("REC_ACCUM", "Accumulator task exiting, accumulated %u bytes (%.1f s)",
             static_cast<unsigned>(s_rec_buffer_used),
             s_rec_buffer_used / (16000.0f * 2));
    s_rec_accum_task_handle = nullptr;
    vTaskDelete(nullptr);
}

void rec_stop_accumulator()
{
    if (s_rec_accum_task_handle) {
        s_rec_accum_stop_flag = true;
        for (int i = 0; i < 20 && s_rec_accum_task_handle != nullptr; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (s_rec_accum_task_handle) {
            ESP_LOGW("REC_SM", "Accumulator task did not exit in time, force deleting");
            vTaskDelete(s_rec_accum_task_handle);
            s_rec_accum_task_handle = nullptr;
        }
    }
    s_rec_accum_stop_flag = false;
}

void rec_playback_monitor_cb(lv_timer_t* timer)
{
    (void)timer;

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        return;
    }

    auto* pb = tool->otool_tools.audio.get_playback();
    bool still_playing = pb && pb->is_async_playback_running();
    if (!still_playing && s_rec_state == REC_STATE_PLAYING) {
        ESP_LOGI("REC_SM", "Playback finished, returning to IDLE");
        rec_enter_idle();
    }
}

void rec_enter_idle()
{
    ESP_LOGI("REC_SM", "enter IDLE");

    rec_stop_accumulator();

    auto* tool = corep4_ui_sync_get_tool();
    if (tool) {
        audio_sr_afe* afe = tool->otool_tools.audio.get_sr_afe();
        if (afe && afe->aec_session_is_running()) {
            afe->aec_session_stop();
        }
    }

    auto* pb_stop = tool ? tool->otool_tools.audio.get_playback() : nullptr;
    if (pb_stop && pb_stop->is_async_playback_running()) {
        pb_stop->stop_async_playback();
    }

    if (s_rec_playback_timer) {
        lv_timer_delete(s_rec_playback_timer);
        s_rec_playback_timer = nullptr;
    }

    if (s_rec_buffer) {
        heap_caps_free(s_rec_buffer);
        s_rec_buffer = nullptr;
    }
    s_rec_buffer_size = 0;
    s_rec_buffer_used = 0;

    s_rec_state = REC_STATE_IDLE;
    rec_update_ui();
}

void rec_enter_recording()
{
    ESP_LOGI("REC_SM", "enter RECORDING");

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        ESP_LOGE("REC_SM", "g_tool is NULL, cannot start recording");
        return;
    }

    if (!s_rec_buffer) {
        s_rec_buffer = static_cast<int16_t*>(heap_caps_malloc(REC_BUFFER_MAX_BYTES, MALLOC_CAP_SPIRAM));
        if (!s_rec_buffer) {
            ESP_LOGE("REC_SM", "Failed to allocate %u bytes from SPIRAM",
                     static_cast<unsigned>(REC_BUFFER_MAX_BYTES));
            return;
        }
        s_rec_buffer_size = REC_BUFFER_MAX_BYTES;
        ESP_LOGI("REC_SM", "Allocated %u KB SPIRAM buffer",
                 static_cast<unsigned>(REC_BUFFER_MAX_BYTES / 1024));
    }
    s_rec_buffer_used = 0;

    audio_sr_afe* afe = tool->otool_tools.audio.get_sr_afe();
    if (!afe) {
        ESP_LOGE("REC_SM", "get_sr_afe() returned NULL");
        heap_caps_free(s_rec_buffer);
        s_rec_buffer = nullptr;
        s_rec_buffer_size = 0;
        return;
    }
    if (!afe->is_initialized()) {
        esp_err_t ret = afe->aec_init(AUDIO_MIC_CHANNEL_1, AUDIO_MIC_CHANNEL_3, 4, AFE_MODE_LOW_COST);
        if (ret != ESP_OK) {
            ESP_LOGE("REC_SM", "aec_init failed: %s", esp_err_to_name(ret));
            heap_caps_free(s_rec_buffer);
            s_rec_buffer = nullptr;
            s_rec_buffer_size = 0;
            return;
        }
    }
    if (!afe->aec_session_is_running()) {
        esp_err_t ret = afe->aec_session_start();
        if (ret != ESP_OK) {
            ESP_LOGE("REC_SM", "aec_session_start failed: %s", esp_err_to_name(ret));
            heap_caps_free(s_rec_buffer);
            s_rec_buffer = nullptr;
            s_rec_buffer_size = 0;
            return;
        }
    }

    s_rec_accum_stop_flag = false;
    BaseType_t xret = xTaskCreatePinnedToCore(
        rec_accumulator_task, "rec_accum", 4096, nullptr,
        configMAX_PRIORITIES - 3, &s_rec_accum_task_handle, 1);
    if (xret != pdPASS) {
        ESP_LOGE("REC_SM", "Failed to create accumulator task");
        afe->aec_session_stop();
        heap_caps_free(s_rec_buffer);
        s_rec_buffer = nullptr;
        s_rec_buffer_size = 0;
        return;
    }

    s_rec_state = REC_STATE_RECORDING;
    rec_update_ui();
}

void rec_enter_paused()
{
    ESP_LOGI("REC_SM", "enter PAUSED (recorded %u bytes, %.1f s)",
             static_cast<unsigned>(s_rec_buffer_used),
             s_rec_buffer_used / (16000.0f * 2));

    rec_stop_accumulator();

    auto* tool = corep4_ui_sync_get_tool();
    if (tool) {
        audio_sr_afe* afe = tool->otool_tools.audio.get_sr_afe();
        if (afe && afe->aec_session_is_running()) {
            afe->aec_session_stop();
        }
    }

    s_rec_state = REC_STATE_PAUSED;
    rec_update_ui();
}

void rec_enter_playing()
{
    ESP_LOGI("REC_SM", "enter PLAYING (%u bytes, %.1f s)",
             static_cast<unsigned>(s_rec_buffer_used),
             s_rec_buffer_used / (16000.0f * 2));

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool || !s_rec_buffer || s_rec_buffer_used == 0) {
        ESP_LOGW("REC_SM", "Nothing to play, returning to IDLE");
        rec_enter_idle();
        return;
    }

    auto* pb_play = tool->otool_tools.audio.get_playback();
    esp_err_t ret = pb_play->play_audio_buffer(
        reinterpret_cast<const uint8_t*>(s_rec_buffer), s_rec_buffer_used,
        16000, AUDIO_CHANNELS_MONO, I2S_DATA_BIT_WIDTH_16BIT,
        AUDIO_PLAYBACK_ASYNC);

    if (ret != ESP_OK) {
        ESP_LOGE("REC_SM", "play_audio_buffer failed: %s", esp_err_to_name(ret));
        rec_enter_idle();
        return;
    }

    if (!s_rec_playback_timer) {
        s_rec_playback_timer = lv_timer_create(rec_playback_monitor_cb, 300, nullptr);
    }

    s_rec_state = REC_STATE_PLAYING;
    rec_update_ui();
}

void rec_handle_click()
{
    switch (s_rec_state) {
        case REC_STATE_IDLE:
            rec_enter_recording();
            break;
        case REC_STATE_RECORDING:
            rec_enter_paused();
            break;
        case REC_STATE_PAUSED:
            rec_enter_playing();
            break;
        case REC_STATE_PLAYING:
            rec_enter_idle();
            break;
    }
}

void rec_handle_longpress()
{
    ESP_LOGI("REC_SM", "Long press detected in state %d, returning to IDLE", s_rec_state);
    if (s_rec_state != REC_STATE_IDLE) {
        rec_enter_idle();
    }
}

void rec_longpress_timer_cb(lv_timer_t* timer)
{
    lv_timer_delete(timer);
    s_rec_longpress_timer = nullptr;

    if (ui_esbtnrec && lv_obj_has_state(ui_esbtnrec, LV_STATE_PRESSED)) {
        s_rec_longpress_handled = true;
        rec_handle_longpress();
    }
}

void rec_btn_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_rec_longpress_handled = false;
        if (s_rec_longpress_timer) {
            lv_timer_delete(s_rec_longpress_timer);
        }
        s_rec_longpress_timer = lv_timer_create(rec_longpress_timer_cb, REC_LONGPRESS_MS, nullptr);
        lv_timer_set_repeat_count(s_rec_longpress_timer, 1);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_rec_longpress_timer) {
            lv_timer_delete(s_rec_longpress_timer);
            s_rec_longpress_timer = nullptr;
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (s_rec_longpress_handled) {
            s_rec_longpress_handled = false;
            return;
        }
        rec_handle_click();
    }
}

} // namespace

sensor_display_manager_t g_sensor_manager = {
    .current_sensor = SENSOR_TYPE_ACC,
    .lines = {
        {"X", 0xFF3838, 15.0f, 50, nullptr},
        {"Y", 0x38FF38, 15.0f, 50, nullptr},
        {"Z", 0x3838FF, 15.0f, 50, nullptr}
    },
    .data_updated = false,
    .sensor_changed = false
};

void switch_sensor_display(sensor_type_t new_sensor_type)
{
    if (new_sensor_type < SENSOR_TYPE_ACC || new_sensor_type >= SENSOR_TYPE_MAX) {
        ESP_LOGW(TAG, "Invalid sensor display type: %d", static_cast<int>(new_sensor_type));
        return;
    }

    const char* sensor_names[] = {"ACC", "GRY", "MAG"};
    ESP_LOGI(TAG, "Switching to %s sensor display", sensor_names[new_sensor_type]);

    g_sensor_manager.current_sensor = new_sensor_type;
    g_sensor_manager.sensor_changed = true;

    switch (new_sensor_type) {
        case SENSOR_TYPE_ACC:
            for (int i = 0; i < 3; i++) {
                g_sensor_manager.lines[i].scale_factor = 4.0f;
                g_sensor_manager.lines[i].center_offset = 100;
            }
            break;
        case SENSOR_TYPE_GRY:
            for (int i = 0; i < 3; i++) {
                g_sensor_manager.lines[i].scale_factor = 1.5f;
                g_sensor_manager.lines[i].center_offset = 100;
            }
            break;
        case SENSOR_TYPE_MAG:
            for (int i = 0; i < 3; i++) {
                g_sensor_manager.lines[i].scale_factor = 1.0f;
                g_sensor_manager.lines[i].center_offset = 100;
            }
            break;
        default:
            break;
    }
}

void recreate_chart_series(void)
{
    const char* task_tag = "CHART_RECREATE";

    if (!ui_bmichart) {
        ESP_LOGW(task_tag, "ui_bmichart is NULL, chart series not recreated");
        return;
    }

    lvgl_port_lock(-1);

    lv_chart_series_t* old_series;
    while ((old_series = lv_chart_get_series_next(ui_bmichart, nullptr)) != nullptr) {
        lv_chart_remove_series(ui_bmichart, old_series);
    }
    ESP_LOGI(task_tag, "Removed all old chart series");

    lv_chart_set_point_count(ui_bmichart, 100);

    const char* sensor_names[] = {"ACC", "GRY", "MAG"};
    const char* axis_names[] = {"X", "Y", "Z"};

    for (int i = 0; i < 3; i++) {
        g_sensor_manager.lines[i].series = lv_chart_add_series(
            ui_bmichart,
            lv_color_hex(g_sensor_manager.lines[i].color),
            LV_CHART_AXIS_PRIMARY_Y);

        if (g_sensor_manager.lines[i].series != nullptr) {
            lv_chart_set_all_value(ui_bmichart, g_sensor_manager.lines[i].series, LV_CHART_POINT_NONE);
            ESP_LOGI(task_tag, "Created series for %s %s axis (color: %06X)",
                     sensor_names[g_sensor_manager.current_sensor], axis_names[i],
                     static_cast<unsigned>(g_sensor_manager.lines[i].color));
        } else {
            ESP_LOGE(task_tag, "Failed to create series for %s %s axis",
                     sensor_names[g_sensor_manager.current_sensor], axis_names[i]);
        }
    }

    lvgl_port_unlock();
    g_sensor_manager.sensor_changed = false;
}

esp_err_t corep4_ui_mian_bind_events(m5_corep4_tool& tool)
{
    corep4_ui_sync_set_tool(tool);

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "Failed to lock LVGL for ui_mian event binding");
        return ESP_FAIL;
    }

    if (ui_esbtnrec) {
        lv_obj_add_event_cb(ui_esbtnrec, rec_btn_event_cb, LV_EVENT_ALL, nullptr);
        ESP_LOGI(TAG, "REC button event bound (click cycle + 3s long press)");
    } else {
        ESP_LOGW(TAG, "ui_esbtnrec is NULL, REC button event not bound");
    }

    lvgl_port_unlock();
    return ESP_OK;
}

extern "C" void btnaccevefunc(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "btnaccevefunc called by ui_mian ACC button click");
    set_sensor_button_colors(SENSOR_TYPE_ACC);
    switch_sensor_display(SENSOR_TYPE_ACC);
}

extern "C" void btngryevefunc(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "btngryevefunc called by ui_mian GRY button click");
    set_sensor_button_colors(SENSOR_TYPE_GRY);
    switch_sensor_display(SENSOR_TYPE_GRY);
}

extern "C" void btnmagevefunc(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "btnmagevefunc called by ui_mian MAG button click");
    set_sensor_button_colors(SENSOR_TYPE_MAG);
    switch_sensor_display(SENSOR_TYPE_MAG);
}

extern "C" void esbtnstart2clicked2func(lv_event_t* e)
{
    (void)e;

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        ESP_LOGE(TAG, "Cannot control audio: tool is NULL");
        return;
    }

    if (!s_audio_status_timer) {
        s_audio_status_timer = lv_timer_create(audio_status_timer_cb, 500, nullptr);
        if (s_audio_status_timer) {
            ESP_LOGI(TAG, "Audio status monitor timer created");
        }
    }

    auto* pb = tool->otool_tools.audio.get_playback();
    bool is_playing = pb && pb->is_async_playback_running();

    if (!is_playing) {
        ESP_LOGI(TAG, "ES start button clicked, starting playback");
        lv_label_set_text(ui_esbtnstartlab, "STOP");

        audio_file_type_t track_type = AUDIO_FILE_CANDY_WIND_1CH_44K_16B_45S;
        if (!pb->is_audio_file_available(track_type)) {
            if (pb->is_audio_file_available(AUDIO_FILE_CANDY_WIND_2CH_44K_16B_45S)) {
                track_type = AUDIO_FILE_CANDY_WIND_2CH_44K_16B_45S;
            } else if (pb->is_audio_file_available(AUDIO_FILE_CANDY_WIND_1CH_16K_16B_9S)) {
                track_type = AUDIO_FILE_CANDY_WIND_1CH_16K_16B_9S;
            } else {
                track_type = AUDIO_FILE_CANDY_WIND_2CH_16K_16B_9S;
            }
        }

        esp_err_t ret = pb->play_audio_file(track_type, AUDIO_PLAYBACK_ASYNC);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Audio playback started successfully");
        } else {
            ESP_LOGE(TAG, "Failed to start audio playback: %s", esp_err_to_name(ret));
            lv_label_set_text(ui_esbtnstartlab, "START");
        }
    } else {
        ESP_LOGI(TAG, "ES start button clicked while playing, stopping playback");

        esp_err_t ret = pb->stop_async_playback();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Audio playback stopped successfully");
        } else {
            ESP_LOGE(TAG, "Failed to stop audio playback: %s", esp_err_to_name(ret));
        }

        lv_label_set_text(ui_esbtnstartlab, "START");
    }
}

extern "C" void esslider2released2func(lv_event_t* e)
{
    (void)e;

    int32_t value = lv_slider_get_value(ui_esslider);

    char value_str[16];
    std::snprintf(value_str, sizeof(value_str), "%ld", value);
    lv_label_set_text(ui_esliderval, value_str);

    auto* tool = corep4_ui_sync_get_tool();
    if (tool) {
        float volume = static_cast<float>(value);
        esp_err_t ret = tool->otool_tools.audio.set_volume(volume);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "ES slider released, volume set to %.0f%%", volume);
        } else {
            ESP_LOGW(TAG, "Failed to set volume to %.0f%%: %s", volume, esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Cannot set volume: tool is NULL");
    }
}

extern "C" void blslider2released2func(lv_event_t* e)
{
    (void)e;

    int32_t value = lv_slider_get_value(ui_blslider);
    if (value < 2) {
        value = 2;
        lv_slider_set_value(ui_blslider, value, LV_ANIM_OFF);
    }

    char value_str[16];
    std::snprintf(value_str, sizeof(value_str), "%ld", value);
    lv_label_set_text(ui_blpwmvallab, value_str);

    auto* tool = corep4_ui_sync_get_tool();
    if (tool) {
        esp_err_t ret = tool->set_lcd_brightness(static_cast<uint8_t>(value));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Backlight slider released, brightness set to %ld%%", value);
        } else {
            ESP_LOGW(TAG, "Failed to set brightness to %ld%%: %s", value, esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Cannot set brightness: tool is NULL");
    }
}
