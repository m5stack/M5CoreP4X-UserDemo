#include "ui_function_widgets.h"
#include "audio_recorder.h"
#include "audio_tools.h"

#include "esp_spiffs.h"
#include "esp_heap_caps.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_REC";
constexpr const char* STORAGE_PARTITION_LABEL = "storage";
constexpr const char* STORAGE_BASE_PATH       = "/webspiffs";
constexpr const char* REC_OUTPUT_FILE         = "/webspiffs/REC.PCM";
constexpr size_t      STORAGE_RESERVE_BYTES   = 32 * 1024;

constexpr uint32_t MONITOR_PERIOD_MS = 50;  ///< LVGL 刷新定时器周期（ms）
constexpr uint32_t MONITOR_CHUNK_MS  = 40;  ///< 每次 I2S 读取的时长（ms）
constexpr uint32_t MONITOR_LOG_PERIOD_MS = 1000; ///< MIC 统计日志周期（ms）

// 波形图配置
constexpr int32_t  N_WAVE_PTS       = 200;  ///< 每通道波形历史点数（每次刷新 4 点，约 2.5s）
constexpr int32_t  WAVE_PTS_PER_TICK = 4;   ///< 每次 LVGL 刷新推进的波形点数
constexpr int32_t  WAVE_ZERO_Y      = 33;   ///< 波形 0 点：距离图表底部约 1/3
constexpr int32_t  WAVE_GAIN_NUM    = 4;    ///< 波形显示增益：4/1 = 4.0x
constexpr int32_t  WAVE_GAIN_DEN    = 1;
constexpr int32_t  WAVE_REC_ANCHOR_INDEX = N_WAVE_PTS / 3;
constexpr int32_t  WAVE_REC_PREVIEW_PTS = N_WAVE_PTS - WAVE_REC_ANCHOR_INDEX - 1;
constexpr int32_t  N_SPEC_BINS      = 24;   ///< MIC1/MIC2/REF 频谱图频率分辨率
constexpr int32_t  SPEC_DFT_N       = 256;  ///< DFT 窗口长度
constexpr int32_t  SPEC_CANVAS_W    = 330;  ///< 频谱画布宽度（精确适配 460px 内容窗口）
constexpr int32_t  SPEC_CANVAS_H    = 112;  ///< 频谱画布高度
constexpr int32_t  SPEC_COL_W       = 1;    ///< 每次刷新写入 1px 频谱列
constexpr double   SPEC_PI = 3.14159265358979323846;
constexpr uint32_t REC_MAX_SECONDS = 10;

struct SpectrumTables {
    double cos_tbl[N_SPEC_BINS][SPEC_DFT_N];
    double sin_tbl[N_SPEC_BINS][SPEC_DFT_N];
    double window_tbl[SPEC_DFT_N];
};

// These tables are only needed by the optional REC spectrum view. Keeping
// them as function-local static arrays consumed 100,352 bytes of scarce
// internal BSS on every boot. Allocate them once, lazily, in PSRAM instead.
std::atomic<SpectrumTables*> s_spectrum_tables{nullptr};
std::atomic<bool> s_spectrum_tables_allocation_failed{false};

// REC 内容区为 460x418，局部收紧到 6px 内边距后：448x406。
constexpr int32_t REC_CONTENT_PAD       = 6;
constexpr int32_t REC_CONTENT_ROW_GAP   = 4;
constexpr int32_t REC_CONTENT_INNER_H   = 418 - REC_CONTENT_PAD * 2;
constexpr int32_t REC_BUTTON_CARD_H     = 104;
constexpr int32_t REC_DISPLAY_CARD_H    = REC_CONTENT_INNER_H - REC_BUTTON_CARD_H - REC_CONTENT_ROW_GAP;
constexpr int32_t REC_DISPLAY_CARD_PAD  = 6;
constexpr int32_t WAVE_TITLE_ROW_H      = 22;
constexpr int32_t WAVE_ROW_H_DEFAULT    =
    (REC_DISPLAY_CARD_H - REC_DISPLAY_CARD_PAD * 2 - WAVE_TITLE_ROW_H - REC_CONTENT_ROW_GAP * 2) / 2;
constexpr int32_t WAVE_ROW_H_WITH_REF   =
    (REC_DISPLAY_CARD_H - REC_DISPLAY_CARD_PAD * 2 - WAVE_TITLE_ROW_H - REC_CONTENT_ROW_GAP * 3) / 3;
constexpr int32_t SPEC_TITLE_ROW_H      = 22;
constexpr int32_t SPEC_ROW_H_DEFAULT    =
    (REC_CONTENT_INNER_H - REC_DISPLAY_CARD_PAD * 2 - SPEC_TITLE_ROW_H - REC_CONTENT_ROW_GAP * 2) / 2;
constexpr int32_t SPEC_ROW_H_WITH_REF   =
    (REC_CONTENT_INNER_H - REC_DISPLAY_CARD_PAD * 2 - SPEC_TITLE_ROW_H - REC_CONTENT_ROW_GAP * 3) / 3;
constexpr int32_t REC_BUTTON_H          = 51;
constexpr int32_t PLAY_BUTTON_H         = 39;
static_assert(REC_DISPLAY_CARD_H + REC_BUTTON_CARD_H + REC_CONTENT_ROW_GAP == REC_CONTENT_INNER_H);
static_assert(SPEC_CANVAS_H <= SPEC_ROW_H_WITH_REF);
static_assert(SPEC_COL_W > 0 && SPEC_COL_W <= SPEC_CANVAS_W);

// 波形图每通道颜色
constexpr uint32_t WAVE_COLOR_MIC1 = 0x4ADE80; ///< 绿色
constexpr uint32_t WAVE_COLOR_MIC2 = 0x60A5FA; ///< 蓝色
constexpr uint32_t WAVE_COLOR_MIC3 = 0xFB923C; ///< 橙色
constexpr uint32_t WAVE_BG_COLOR   = 0x1E293B; ///< 深色背景

constexpr uint32_t WAVE_MARKER_COLOR = 0x22C55E;
constexpr uint32_t PLAY_ACTIVE_COLOR = 0xF59E0B;
constexpr UBaseType_t REC_MONITOR_TASK_PRIORITY = 4;
constexpr UBaseType_t REC_WORKER_TASK_PRIORITY  = 5;
constexpr BaseType_t  REC_AUDIO_TASK_CORE       = 0;

struct RecAction {
    uint32_t duration_seconds;
};

enum class RecDisplayMode : uint8_t {
    WAVEFORM,
    SPECTRUM,
};

enum class RecSessionState : uint8_t {
    IDLE,
    RECORDING,
    RECORDED,
    PLAYING,
};

struct MicMonitorStats {
    bool has_data = false;
    int16_t min = 0;
    int16_t max = 0;
    int32_t peak = 0;
    double mean = 0.0;
    double rms = 0.0;
    double dbfs = -std::numeric_limits<double>::infinity();
};

struct RecPageState {
    // ---- 原有 UI 对象 ----
    lv_obj_t* status_label      = nullptr;
    lv_obj_t* format_label      = nullptr;
    lv_obj_t* result_label      = nullptr;
    lv_obj_t* rec_button        = nullptr;
    lv_obj_t* play_button       = nullptr;
    lv_obj_t* duration_slider   = nullptr;
    lv_obj_t* duration_label    = nullptr;
    lv_obj_t* record_button_card = nullptr;

    // ---- 波形监测 UI 对象 ----
    lv_obj_t*          waveform_card       = nullptr;
    lv_obj_t*          spectrum_card       = nullptr;
    lv_obj_t*          waveform_rows[3]    = {};  ///< MIC1/MIC2/REF 行
    lv_obj_t*          waveform_charts[3]  = {};  ///< 3 个通道的 lv_chart
    lv_chart_series_t* waveform_series[3]  = {};  ///< 对应的数据序列
    lv_chart_series_t* waveform_rms_series[3] = {}; ///< RMS 辅助线，让波形不只看峰值
    lv_obj_t*          db_labels[3]        = {};  ///< 每通道的 dBFS 标签
    lv_obj_t*          spec_rows[3]        = {};  ///< MIC1/MIC2/REF 频谱行
    lv_obj_t*          spec_canvas[3]      = {};  ///< MIC1/MIC2/REF 录音频谱图
    uint16_t*          spec_buf[3]         = {};  ///< RGB565 canvas buffer
    lv_obj_t*          waveform_markers[3] = {};
    lv_obj_t*          settings_ref_button = nullptr;

    // ---- 波形监测状态 ----
    TaskHandle_t   monitor_task_handle = nullptr;
    lv_timer_t*    waveform_timer      = nullptr;
    TaskHandle_t   play_task_handle    = nullptr;
    volatile bool  monitor_active      = false;   ///< 监测任务运行标志
    volatile bool  monitor_is_reading  = false;   ///< 当前正在执行 I2S 读（用于互斥）
    volatile bool  stop_requested      = false;   ///< REC 按键在录音中再次点击时置位
    volatile bool  play_running        = false;
    volatile bool  stop_play_requested = false;
    volatile int16_t latest_peak[3]    = {};      ///< 最新每通道峰值（0..32767）
    volatile int16_t latest_rms[3]     = {};      ///< 最新每通道 RMS（0..32767）
    volatile int16_t latest_wave_y[3][WAVE_PTS_PER_TICK] = {}; ///< 低开销波形点（0..100，零点 WAVE_ZERO_Y）
    volatile uint8_t latest_spectrum[3][N_SPEC_BINS] = {}; ///< 最新 MIC1/MIC2/REF 频谱强度（0..255）
    bool           spectrum_active     = false;
    char           last_db_text[3][12] = {};

    // ---- 原有状态 ----
    volatile bool page_alive  = false;
    volatile bool task_running = false;
    volatile bool discard_record_requested = false;
    RecSessionState session_state = RecSessionState::IDLE;
    uint32_t sequence = 0;
    char last_path[96] = {};
    size_t last_record_bytes = 0;
    uint32_t last_record_sample_rate = 0;
    audio_channels_t last_record_channels = AUDIO_CHANNELS_STEREO;
    i2s_data_bit_width_t last_record_bits = I2S_DATA_BIT_WIDTH_16BIT;
    RecDisplayMode display_mode = RecDisplayMode::WAVEFORM;
    bool show_ref = false;
    int16_t recorded_wave_y[3][WAVE_REC_PREVIEW_PTS] = {};
    int16_t recorded_peak[3] = {};
    bool recorded_wave_ready = false;
    bool recorded_waveform_dirty = false;
    int32_t rendered_preview_shift = -1;
    TickType_t playback_start_tick = 0;
    uint32_t playback_duration_ms = 0;
    bool playback_wave_active = false;
    CoreP4FunctionDialogHandle settings_dialog = {};
};

RecPageState s_rec_page;

esp_err_t ensure_rec_storage_mounted()
{
    if (esp_spiffs_mounted(STORAGE_PARTITION_LABEL)) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_BASE_PATH,
        .partition_label = STORAGE_PARTITION_LABEL,
        .max_files = 8,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_ERR_INVALID_STATE && esp_spiffs_mounted(STORAGE_PARTITION_LABEL)) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to mount storage SPIFFS: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info(STORAGE_PARTITION_LABEL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Mounted storage SPIFFS at %s: total=%u used=%u",
                 STORAGE_BASE_PATH,
                 static_cast<unsigned>(total),
                 static_cast<unsigned>(used));
    } else {
        ESP_LOGW(TAG, "Mounted storage SPIFFS, but info failed: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
}

esp_err_t get_rec_storage_info(size_t* total, size_t* used)
{
    if (!esp_spiffs_mounted(STORAGE_PARTITION_LABEL)) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_spiffs_info(STORAGE_PARTITION_LABEL, total, used);
}

void log_mic_monitor_stats(const MicMonitorStats stats[4], size_t samples_per_channel)
{
    char line[384];
    int offset = std::snprintf(line, sizeof(line),
                               "MIC stats samples=%u",
                               static_cast<unsigned>(samples_per_channel));

    for (int ch = 0; ch < 4 && offset > 0 && offset < static_cast<int>(sizeof(line)); ch++) {
        if (!stats[ch].has_data) {
            offset += std::snprintf(line + offset,
                                    sizeof(line) - static_cast<size_t>(offset),
                                    " | MIC%d: no-data",
                                    ch + 1);
            continue;
        }

        offset += std::snprintf(line + offset,
                                sizeof(line) - static_cast<size_t>(offset),
                                " | MIC%d: pk=%ld db=%+.1f min=%d max=%d mean=%.1f rms=%.1f",
                                ch + 1,
                                static_cast<long>(stats[ch].peak),
                                stats[ch].dbfs,
                                static_cast<int>(stats[ch].min),
                                static_cast<int>(stats[ch].max),
                                stats[ch].mean,
                                stats[ch].rms);
    }

    ESP_LOGI(TAG, "%s", line);
}

void delete_rec_output_file()
{
    if (std::remove(REC_OUTPUT_FILE) == 0) {
        ESP_LOGI(TAG, "Deleted previous REC file: %s", REC_OUTPUT_FILE);
    }
}

void set_rec_result_text(const char* text, uint32_t color)
{
    set_label_text(s_rec_page.result_label, text);
    if (s_rec_page.result_label) {
        lv_obj_set_style_text_color(s_rec_page.result_label, lv_color_hex(color), LV_PART_MAIN);
    }
}

uint16_t spec_color_from_level(uint8_t level)
{
    const uint8_t x = level;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    if (x < 64) {
        r = 5;
        g = static_cast<uint8_t>(x / 4);
        b = static_cast<uint8_t>(32 + x);
    } else if (x < 128) {
        r = static_cast<uint8_t>((x - 64) * 2);
        g = static_cast<uint8_t>(16 + (x - 64) / 2);
        b = static_cast<uint8_t>(96 + (x - 64));
    } else if (x < 192) {
        r = static_cast<uint8_t>(128 + (x - 128) * 2);
        g = static_cast<uint8_t>(48 + (x - 128));
        b = static_cast<uint8_t>(160 - (x - 128) * 2);
    } else {
        r = 255;
        g = static_cast<uint8_t>(112 + (x - 192) * 2);
        b = static_cast<uint8_t>((x > 224) ? (x - 224) * 4 : 0);
    }

    return lv_color_to_u16(lv_color_make(r, g, b));
}

void clear_rec_spectrogram(bool activate)
{
    s_rec_page.spectrum_active = activate;
    for (int ch = 0; ch < 3; ch++) {
        for (int bin = 0; bin < N_SPEC_BINS; bin++) {
            s_rec_page.latest_spectrum[ch][bin] = 0;
        }

        if (s_rec_page.spec_buf[ch]) {
            const uint16_t bg = lv_color_to_u16(lv_color_hex(0x050816));
            for (int32_t i = 0; i < SPEC_CANVAS_W * SPEC_CANVAS_H; i++) {
                s_rec_page.spec_buf[ch][i] = bg;
            }
        }
        if (s_rec_page.spec_canvas[ch]) {
            lv_obj_invalidate(s_rec_page.spec_canvas[ch]);
        }
    }
}

void* alloc_rec_canvas_buffer(size_t bytes)
{
    void* buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    return buffer;
}

void rec_spectrum_canvas_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    void* buffer = lv_event_get_user_data(e);
    if (buffer) {
        heap_caps_free(buffer);
    }

    lv_obj_t* canvas = lv_event_get_target_obj(e);
    for (int ch = 0; ch < 3; ch++) {
        if (s_rec_page.spec_canvas[ch] == canvas) {
            s_rec_page.spec_canvas[ch] = nullptr;
            if (s_rec_page.spec_buf[ch] == buffer) {
                s_rec_page.spec_buf[ch] = nullptr;
            }
            break;
        }
    }
}

int16_t sample_to_wave_y(int16_t sample)
{
    int32_t y = WAVE_ZERO_Y + (static_cast<int32_t>(sample) * 48 * WAVE_GAIN_NUM /
                               (32767 * WAVE_GAIN_DEN));
    if (y < 0) {
        y = 0;
    } else if (y > 100) {
        y = 100;
    }
    return static_cast<int16_t>(y);
}

const char* rec_session_state_name(RecSessionState state)
{
    switch (state) {
        case RecSessionState::IDLE: return "idle";
        case RecSessionState::RECORDING: return "recording";
        case RecSessionState::RECORDED: return "recorded";
        case RecSessionState::PLAYING: return "playing";
        default: return "unknown";
    }
}

uint32_t audio_channel_count(audio_channels_t channels)
{
    switch (channels) {
        case AUDIO_CHANNELS_MONO: return 1;
        case AUDIO_CHANNELS_STEREO: return 2;
        case AUDIO_CHANNELS_3CHs: return 3;
        case AUDIO_CHANNELS_4CHs: return 4;
        default: return 2;
    }
}

uint32_t record_duration_ms_from_bytes(size_t bytes,
                                       uint32_t sample_rate,
                                       audio_channels_t channels,
                                       i2s_data_bit_width_t bits)
{
    const uint32_t bit_count = static_cast<uint32_t>(bits);
    const uint32_t channel_count = audio_channel_count(channels);
    const size_t bytes_per_frame = static_cast<size_t>(channel_count) * bit_count / 8;
    if (bytes == 0 || sample_rate == 0 || bytes_per_frame == 0) {
        return 0;
    }
    return static_cast<uint32_t>((static_cast<uint64_t>(bytes) * 1000ULL) /
                                 (static_cast<uint64_t>(sample_rate) * bytes_per_frame));
}

size_t split_samples_per_channel_from_bytes(size_t bytes, i2s_data_bit_width_t bits)
{
    const size_t bytes_per_sample = static_cast<size_t>(bits) / 8;
    const size_t frame_bytes = bytes_per_sample * 2;
    return frame_bytes ? (bytes / frame_bytes) : 0;
}

void clear_recorded_wave_cache()
{
    for (int ch = 0; ch < 3; ch++) {
        s_rec_page.recorded_peak[ch] = 0;
        for (int pt = 0; pt < WAVE_REC_PREVIEW_PTS; pt++) {
            s_rec_page.recorded_wave_y[ch][pt] = WAVE_ZERO_Y;
        }
    }
    s_rec_page.recorded_wave_ready = false;
    s_rec_page.recorded_waveform_dirty = true;
    s_rec_page.rendered_preview_shift = -1;
    s_rec_page.playback_start_tick = 0;
    s_rec_page.playback_duration_ms = 0;
    s_rec_page.playback_wave_active = false;
}

void clear_record_file_state(bool delete_file)
{
    if (delete_file) {
        delete_rec_output_file();
    }
    s_rec_page.last_path[0] = '\0';
    s_rec_page.last_record_bytes = 0;
    s_rec_page.last_record_sample_rate = 0;
    s_rec_page.last_record_channels = AUDIO_CHANNELS_STEREO;
    s_rec_page.last_record_bits = I2S_DATA_BIT_WIDTH_16BIT;
    clear_recorded_wave_cache();
}

void update_waveform_markers(bool visible)
{
    for (int ch = 0; ch < 3; ch++) {
        lv_obj_t* marker = s_rec_page.waveform_markers[ch];
        lv_obj_t* chart = s_rec_page.waveform_charts[ch];
        if (!marker || !chart) {
            continue;
        }

        if (!visible || (ch == 2 && !s_rec_page.show_ref)) {
            lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const int32_t chart_w = lv_obj_get_width(chart);
        const int32_t chart_h = lv_obj_get_height(chart);
        const int32_t marker_x = (chart_w > 0) ? (chart_w * WAVE_REC_ANCHOR_INDEX / N_WAVE_PTS) : 0;
        lv_obj_set_size(marker, 3, chart_h > 8 ? chart_h - 8 : chart_h);
        lv_obj_set_pos(marker, marker_x, 4);
        lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
    }
}

void hide_waveform_markers()
{
    update_waveform_markers(false);
}

void fill_chart_series_direct(lv_obj_t* chart, lv_chart_series_t* ser, int32_t value)
{
    if (!chart || !ser) {
        return;
    }

    int32_t* y_points = lv_chart_get_y_array(chart, ser);
    const uint32_t point_count = lv_chart_get_point_count(chart);
    if (!y_points || point_count == 0) {
        return;
    }

    for (uint32_t i = 0; i < point_count; i++) {
        y_points[i] = value;
    }
    lv_chart_set_x_start_point(chart, ser, 0);
}

void push_live_chart_points(lv_obj_t* chart,
                            lv_chart_series_t* ser,
                            lv_chart_series_t* rms_ser,
                            const int16_t wave_values[WAVE_PTS_PER_TICK],
                            int16_t rms_y)
{
    if (!chart || !ser || !rms_ser || !wave_values) {
        return;
    }

    int32_t* y_points = lv_chart_get_y_array(chart, ser);
    int32_t* rms_points = lv_chart_get_y_array(chart, rms_ser);
    const uint32_t point_count = lv_chart_get_point_count(chart);
    if (!y_points || !rms_points || point_count == 0) {
        return;
    }

    const uint32_t start = lv_chart_get_x_start_point(chart, ser);
    for (int pt = 0; pt < WAVE_PTS_PER_TICK; pt++) {
        const uint32_t idx = (start + static_cast<uint32_t>(pt)) % point_count;
        y_points[idx] = wave_values[pt];
        rms_points[idx] = rms_y;
    }

    const uint32_t next_start =
        (start + static_cast<uint32_t>(WAVE_PTS_PER_TICK)) % point_count;
    lv_chart_set_x_start_point(chart, ser, next_start);
    lv_chart_set_x_start_point(chart, rms_ser, next_start);
    lv_obj_invalidate(chart);
}

void set_db_label_text_if_changed(int ch, const char* text)
{
    if (ch < 0 || ch >= 3 || !text || !s_rec_page.db_labels[ch]) {
        return;
    }

    if (std::strcmp(s_rec_page.last_db_text[ch], text) == 0) {
        return;
    }

    std::snprintf(s_rec_page.last_db_text[ch],
                  sizeof(s_rec_page.last_db_text[ch]),
                  "%s",
                  text);
    lv_label_set_text(s_rec_page.db_labels[ch], text);
}

void render_recorded_waveform_view(int32_t shift)
{
    if (shift < 0) {
        shift = 0;
    } else if (shift > WAVE_REC_PREVIEW_PTS) {
        shift = WAVE_REC_PREVIEW_PTS;
    }

    if (!s_rec_page.recorded_waveform_dirty &&
        s_rec_page.rendered_preview_shift == shift) {
        update_waveform_markers(s_rec_page.session_state == RecSessionState::RECORDED ||
                                s_rec_page.session_state == RecSessionState::PLAYING);
        return;
    }

    for (int ch = 0; ch < 3; ch++) {
        lv_obj_t* chart = s_rec_page.waveform_charts[ch];
        lv_chart_series_t* ser = s_rec_page.waveform_series[ch];
        lv_chart_series_t* rms_ser = s_rec_page.waveform_rms_series[ch];
        if (!chart || !ser) {
            continue;
        }

        fill_chart_series_direct(chart, ser, WAVE_ZERO_Y);
        if (rms_ser) {
            fill_chart_series_direct(chart, rms_ser, WAVE_ZERO_Y);
        }

        int32_t* y_points = lv_chart_get_y_array(chart, ser);
        const uint32_t point_count = lv_chart_get_point_count(chart);
        if (s_rec_page.recorded_wave_ready) {
            for (int src = 0; src < WAVE_REC_PREVIEW_PTS; src++) {
                const int32_t dst = WAVE_REC_ANCHOR_INDEX + 1 + src - shift;
                if (dst < 0 || dst >= N_WAVE_PTS) {
                    continue;
                }
                if (y_points && static_cast<uint32_t>(dst) < point_count) {
                    y_points[dst] = s_rec_page.recorded_wave_y[ch][src];
                }
            }
        }
        lv_chart_refresh(chart);

        if (s_rec_page.db_labels[ch]) {
            char db_buf[12];
            const int16_t peak = s_rec_page.recorded_peak[ch];
            if (peak <= 1) {
                std::snprintf(db_buf, sizeof(db_buf), "-inf");
            } else {
                const double db = 20.0 * std::log10(static_cast<double>(peak) / 32767.0);
                std::snprintf(db_buf, sizeof(db_buf), "%+.0f", db);
            }
            set_db_label_text_if_changed(ch, db_buf);
        }
    }

    s_rec_page.rendered_preview_shift = shift;
    s_rec_page.recorded_waveform_dirty = false;
    update_waveform_markers(s_rec_page.session_state == RecSessionState::RECORDED ||
                            s_rec_page.session_state == RecSessionState::PLAYING);
}

void consume_recorded_wave_chunk(const uint8_t* chunk,
                                 size_t chunk_bytes,
                                 size_t total_samples_per_channel,
                                 size_t* sample_offset,
                                 const esp_codec_dev_sample_info_t& fs,
                                 bool is_tdm,
                                 audio_mic_channel_t mic_mask,
                                 int16_t preview_samples[3][WAVE_REC_PREVIEW_PTS],
                                 int32_t preview_abs[3][WAVE_REC_PREVIEW_PTS],
                                 int32_t preview_peak[3])
{
    if (!chunk || chunk_bytes == 0 || total_samples_per_channel == 0 || !sample_offset) {
        return;
    }

    channel_split_result_t split =
        audio_recorder::split_recorded_channels(chunk, chunk_bytes, fs, is_tdm, mic_mask);
    if (split.status != ESP_OK || split.samples_per_channel == 0) {
        *sample_offset += split_samples_per_channel_from_bytes(chunk_bytes,
                                                               static_cast<i2s_data_bit_width_t>(fs.bits_per_sample));
        return;
    }

    for (int ch = 0; ch < 3; ch++) {
        const int16_t* samples = split.mic_buffers[ch];
        if (!samples) {
            continue;
        }

        for (size_t i = 0; i < split.samples_per_channel; i++) {
            size_t bucket = ((*sample_offset + i) * WAVE_REC_PREVIEW_PTS) /
                            total_samples_per_channel;
            if (bucket >= WAVE_REC_PREVIEW_PTS) {
                bucket = WAVE_REC_PREVIEW_PTS - 1;
            }

            const int32_t sample = samples[i];
            const int32_t abs_v = sample < 0 ? -sample : sample;
            if (abs_v > preview_abs[ch][bucket]) {
                preview_abs[ch][bucket] = abs_v;
                preview_samples[ch][bucket] = static_cast<int16_t>(sample);
            }
            if (abs_v > preview_peak[ch]) {
                preview_peak[ch] = abs_v;
            }
        }
    }

    *sample_offset += split.samples_per_channel;
    audio_recorder::free_channel_split_result(split);
}

void finalize_recorded_wave_preview(const int16_t preview_samples[3][WAVE_REC_PREVIEW_PTS],
                                    const int32_t preview_peak[3])
{
    for (int ch = 0; ch < 3; ch++) {
        s_rec_page.recorded_peak[ch] =
            static_cast<int16_t>(preview_peak[ch] > 32767 ? 32767 : preview_peak[ch]);
        for (int pt = 0; pt < WAVE_REC_PREVIEW_PTS; pt++) {
            s_rec_page.recorded_wave_y[ch][pt] = sample_to_wave_y(preview_samples[ch][pt]);
        }
    }
    s_rec_page.recorded_wave_ready = true;
    s_rec_page.recorded_waveform_dirty = true;
    s_rec_page.rendered_preview_shift = -1;
}

bool build_recorded_wave_preview_from_buffer(const uint8_t* buffer,
                                             size_t bytes,
                                             uint32_t sample_rate,
                                             i2s_data_bit_width_t bits,
                                             bool is_tdm,
                                             audio_mic_channel_t mic_mask)
{
    clear_recorded_wave_cache();
    if (!buffer || bytes == 0 || sample_rate == 0) {
        return false;
    }

    const size_t total_samples = split_samples_per_channel_from_bytes(bytes, bits);
    if (total_samples == 0) {
        return false;
    }

    int16_t preview_samples[3][WAVE_REC_PREVIEW_PTS] = {};
    int32_t preview_abs[3][WAVE_REC_PREVIEW_PTS] = {};
    int32_t preview_peak[3] = {};
    size_t sample_offset = 0;
    const size_t bytes_per_sample = static_cast<size_t>(bits) / 8;
    const size_t frame_bytes = bytes_per_sample * 2;
    size_t chunk_size = 4096;
    chunk_size -= frame_bytes ? (chunk_size % frame_bytes) : 0;
    if (chunk_size == 0) {
        chunk_size = frame_bytes ? frame_bytes : bytes;
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = sample_rate;
    fs.channel = 2;
    fs.bits_per_sample = static_cast<uint8_t>(bits);

    for (size_t offset = 0; offset < bytes;) {
        size_t to_process = bytes - offset;
        if (to_process > chunk_size) {
            to_process = chunk_size;
        }
        if (frame_bytes && to_process > frame_bytes) {
            to_process -= to_process % frame_bytes;
        }
        if (to_process == 0) {
            break;
        }

        consume_recorded_wave_chunk(buffer + offset,
                                    to_process,
                                    total_samples,
                                    &sample_offset,
                                    fs,
                                    is_tdm,
                                    mic_mask,
                                    preview_samples,
                                    preview_abs,
                                    preview_peak);
        offset += to_process;
    }

    finalize_recorded_wave_preview(preview_samples, preview_peak);
    return true;
}

bool build_recorded_wave_preview_from_file(const char* path,
                                           size_t bytes,
                                           uint32_t sample_rate,
                                           i2s_data_bit_width_t bits,
                                           bool is_tdm,
                                           audio_mic_channel_t mic_mask)
{
    clear_recorded_wave_cache();
    if (!path || bytes == 0 || sample_rate == 0) {
        return false;
    }

    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }

    const size_t total_samples = split_samples_per_channel_from_bytes(bytes, bits);
    if (total_samples == 0) {
        fclose(file);
        return false;
    }

    const size_t bytes_per_sample = static_cast<size_t>(bits) / 8;
    const size_t frame_bytes = bytes_per_sample * 2;
    size_t chunk_size = 4096;
    chunk_size -= frame_bytes ? (chunk_size % frame_bytes) : 0;
    if (chunk_size == 0) {
        chunk_size = frame_bytes ? frame_bytes : 512;
    }

    uint8_t* chunk = static_cast<uint8_t*>(malloc(chunk_size));
    if (!chunk) {
        fclose(file);
        return false;
    }

    int16_t preview_samples[3][WAVE_REC_PREVIEW_PTS] = {};
    int32_t preview_abs[3][WAVE_REC_PREVIEW_PTS] = {};
    int32_t preview_peak[3] = {};
    size_t sample_offset = 0;
    size_t remaining = bytes;

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = sample_rate;
    fs.channel = 2;
    fs.bits_per_sample = static_cast<uint8_t>(bits);

    while (remaining > 0) {
        size_t to_read = remaining < chunk_size ? remaining : chunk_size;
        if (frame_bytes && to_read > frame_bytes) {
            to_read -= to_read % frame_bytes;
        }
        if (to_read == 0) {
            break;
        }

        const size_t got = fread(chunk, 1, to_read, file);
        if (got == 0) {
            break;
        }

        consume_recorded_wave_chunk(chunk,
                                    got,
                                    total_samples,
                                    &sample_offset,
                                    fs,
                                    is_tdm,
                                    mic_mask,
                                    preview_samples,
                                    preview_abs,
                                    preview_peak);
        remaining -= got;
    }

    free(chunk);
    fclose(file);

    finalize_recorded_wave_preview(preview_samples, preview_peak);
    return sample_offset > 0;
}

size_t file_size_bytes(const char* path)
{
    if (!path) {
        return 0;
    }

    FILE* file = fopen(path, "rb");
    if (!file) {
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }

    const long pos = ftell(file);
    fclose(file);
    return pos > 0 ? static_cast<size_t>(pos) : 0;
}

bool restore_record_file_from_storage()
{
    const esp_err_t storage_ret = ensure_rec_storage_mounted();
    if (storage_ret != ESP_OK) {
        clear_record_file_state(false);
        s_rec_page.session_state = RecSessionState::IDLE;
        return false;
    }

    const size_t bytes = file_size_bytes(REC_OUTPUT_FILE);
    if (bytes == 0) {
        clear_record_file_state(false);
        s_rec_page.session_state = RecSessionState::IDLE;
        return false;
    }

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        clear_record_file_state(false);
        s_rec_page.session_state = RecSessionState::IDLE;
        return false;
    }

    std::snprintf(s_rec_page.last_path, sizeof(s_rec_page.last_path), "%s", REC_OUTPUT_FILE);
    s_rec_page.last_record_bytes = bytes;
    s_rec_page.last_record_sample_rate = tool->otool_tools.audio.get_sample_rate();
    s_rec_page.last_record_channels =
        (tool->otool_tools.audio.get_rx_channels() == AUDIO_CHANNELS_MONO)
            ? AUDIO_CHANNELS_MONO
            : AUDIO_CHANNELS_STEREO;
    s_rec_page.last_record_bits = tool->otool_tools.audio.get_bits_per_sample();
    s_rec_page.playback_duration_ms =
        record_duration_ms_from_bytes(s_rec_page.last_record_bytes,
                                      s_rec_page.last_record_sample_rate,
                                      s_rec_page.last_record_channels,
                                      s_rec_page.last_record_bits);

    const audio_mic_channel_t target_mask =
        static_cast<audio_mic_channel_t>(AUDIO_MIC_CHANNEL_1 |
                                          AUDIO_MIC_CHANNEL_2 |
                                          AUDIO_MIC_CHANNEL_3);
    const audio_mic_channel_t eff_mask =
        static_cast<audio_mic_channel_t>(tool->otool_tools.audio.get_mic_channels() & target_mask);
    build_recorded_wave_preview_from_file(s_rec_page.last_path,
                                          s_rec_page.last_record_bytes,
                                          s_rec_page.last_record_sample_rate,
                                          s_rec_page.last_record_bits,
                                          tool->otool_tools.audio.is_es7210_tdm_mode(),
                                          eff_mask);
    s_rec_page.session_state = RecSessionState::RECORDED;
    return true;
}

void clear_latest_wave_points(int ch)
{
    if (ch < 0 || ch >= 3) {
        return;
    }

    for (int pt = 0; pt < WAVE_PTS_PER_TICK; pt++) {
        s_rec_page.latest_wave_y[ch][pt] = WAVE_ZERO_Y;
    }
}

void clear_rec_waveforms()
{
    hide_waveform_markers();
    s_rec_page.rendered_preview_shift = -1;
    s_rec_page.recorded_waveform_dirty = true;

    for (int ch = 0; ch < 3; ch++) {
        s_rec_page.latest_peak[ch] = 0;
        s_rec_page.latest_rms[ch] = 0;
        clear_latest_wave_points(ch);

        if (s_rec_page.waveform_charts[ch] && s_rec_page.waveform_series[ch]) {
            lv_chart_set_all_values(s_rec_page.waveform_charts[ch],
                                    s_rec_page.waveform_series[ch],
                                    WAVE_ZERO_Y);
            if (s_rec_page.waveform_rms_series[ch]) {
                lv_chart_set_all_values(s_rec_page.waveform_charts[ch],
                                        s_rec_page.waveform_rms_series[ch],
                                        WAVE_ZERO_Y);
            }
        }
    }
}

void compute_spectrum_levels(const int16_t* samples, size_t count, volatile uint8_t out[N_SPEC_BINS]);
void clear_latest_audio_metrics(int ch, bool clear_spectrum);

struct RecRawChannelMetrics {
    bool has_data = false;
    size_t count = 0;
    int16_t min = 0;
    int16_t max = 0;
    int32_t peak = 0;
    int64_t sum = 0;
    uint64_t sum_sq = 0;
    int16_t wave_y[WAVE_PTS_PER_TICK] = {};
    int16_t spectrum_samples[SPEC_DFT_N] = {};
    size_t spectrum_count = 0;
};

struct RecRawMetrics {
    size_t samples_per_channel = 0;
    RecRawChannelMetrics channels[4] = {};
};

BaseType_t create_rec_audio_task(TaskFunction_t task_func,
                                 const char* name,
                                 uint32_t stack_depth,
                                 void* param,
                                 UBaseType_t priority,
                                 TaskHandle_t* handle)
{
#if CONFIG_FREERTOS_UNICORE
    return xTaskCreate(task_func, name, stack_depth, param, priority, handle);
#else
    return xTaskCreatePinnedToCore(task_func,
                                   name,
                                   stack_depth,
                                   param,
                                   priority,
                                   handle,
                                   REC_AUDIO_TASK_CORE);
#endif
}

uint32_t load_u32_le(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int16_t load_i16_le(const uint8_t* p)
{
    return static_cast<int16_t>(static_cast<uint16_t>(p[0]) |
                                (static_cast<uint16_t>(p[1]) << 8));
}

void init_raw_metrics(RecRawMetrics& metrics, size_t samples_per_channel)
{
    metrics = {};
    metrics.samples_per_channel = samples_per_channel;
    for (int ch = 0; ch < 4; ch++) {
        for (int pt = 0; pt < WAVE_PTS_PER_TICK; pt++) {
            metrics.channels[ch].wave_y[pt] = WAVE_ZERO_Y;
        }
    }
}

bool mic_mask_has_channel(audio_mic_channel_t mask, int ch)
{
    return (static_cast<uint32_t>(mask) & (1U << ch)) != 0;
}

uint8_t get_enabled_mic_indices(audio_mic_channel_t mask, uint8_t indices[4])
{
    uint8_t count = 0;
    for (int ch = 0; ch < 4; ch++) {
        if (mic_mask_has_channel(mask, ch)) {
            indices[count++] = static_cast<uint8_t>(ch);
        }
    }
    return count;
}

void record_raw_channel_sample(RecRawMetrics& metrics,
                               int ch,
                               int16_t sample,
                               size_t sample_index,
                               bool update_spectrum)
{
    if (ch < 0 || ch >= 4) {
        return;
    }

    RecRawChannelMetrics& dst = metrics.channels[ch];
    if (!dst.has_data) {
        dst.has_data = true;
        dst.min = sample;
        dst.max = sample;
    } else {
        if (sample < dst.min) {
            dst.min = sample;
        }
        if (sample > dst.max) {
            dst.max = sample;
        }
    }

    const int32_t sample32 = sample;
    const int32_t abs_v = sample32 < 0 ? -sample32 : sample32;
    if (abs_v > dst.peak) {
        dst.peak = abs_v;
    }
    const int64_t sample64 = sample32;
    dst.sum += sample64;
    dst.sum_sq += static_cast<uint64_t>(sample64 * sample64);
    dst.count++;

    if (ch < 3) {
        for (int pt = 0; pt < WAVE_PTS_PER_TICK; pt++) {
            const size_t target =
                (WAVE_PTS_PER_TICK <= 1 || metrics.samples_per_channel <= 1)
                    ? 0
                    : (static_cast<size_t>(pt) * (metrics.samples_per_channel - 1) /
                       static_cast<size_t>(WAVE_PTS_PER_TICK - 1));
            if (sample_index == target) {
                dst.wave_y[pt] = sample_to_wave_y(sample);
            }
        }

        if (update_spectrum &&
            (ch < 2 || s_rec_page.show_ref) &&
            dst.spectrum_count < SPEC_DFT_N) {
            dst.spectrum_samples[dst.spectrum_count++] = sample;
        }
    }
}

void clear_latest_spectrum_channel(int ch)
{
    if (ch < 0 || ch >= 3) {
        return;
    }
    for (int bin = 0; bin < N_SPEC_BINS; bin++) {
        s_rec_page.latest_spectrum[ch][bin] = 0;
    }
}

void apply_raw_metrics_to_latest(const RecRawMetrics& metrics, bool update_spectrum)
{
    for (int ch = 0; ch < 3; ch++) {
        const RecRawChannelMetrics& src = metrics.channels[ch];
        if (!src.has_data || src.count == 0) {
            clear_latest_audio_metrics(ch, update_spectrum);
            continue;
        }

        s_rec_page.latest_peak[ch] = static_cast<int16_t>(src.peak > 32767 ? 32767 : src.peak);
        const double rms = std::sqrt(static_cast<double>(src.sum_sq) /
                                     static_cast<double>(src.count));
        s_rec_page.latest_rms[ch] = static_cast<int16_t>(rms > 32767.0 ? 32767 : rms);
        for (int pt = 0; pt < WAVE_PTS_PER_TICK; pt++) {
            s_rec_page.latest_wave_y[ch][pt] = src.wave_y[pt];
        }

        if (update_spectrum && (ch < 2 || s_rec_page.show_ref)) {
            compute_spectrum_levels(src.spectrum_samples,
                                    src.spectrum_count,
                                    s_rec_page.latest_spectrum[ch]);
        } else if (update_spectrum && ch == 2) {
            clear_latest_spectrum_channel(ch);
        }
    }
}

void build_monitor_stats_from_raw(const RecRawMetrics& metrics, MicMonitorStats stats[4])
{
    for (int ch = 0; ch < 4; ch++) {
        const RecRawChannelMetrics& src = metrics.channels[ch];
        stats[ch] = {};
        if (!src.has_data || src.count == 0) {
            continue;
        }

        stats[ch].has_data = true;
        stats[ch].min = src.min;
        stats[ch].max = src.max;
        stats[ch].peak = src.peak;
        stats[ch].mean = static_cast<double>(src.sum) / static_cast<double>(src.count);
        stats[ch].rms = std::sqrt(static_cast<double>(src.sum_sq) /
                                  static_cast<double>(src.count));
        stats[ch].dbfs = (src.peak > 1)
                             ? (20.0 * std::log10(static_cast<double>(src.peak) / 32767.0))
                             : -std::numeric_limits<double>::infinity();
    }
}

bool update_latest_audio_metrics_from_raw(const uint8_t* buffer,
                                          size_t bytes_read,
                                          const esp_codec_dev_sample_info_t& fs,
                                          bool is_tdm,
                                          audio_mic_channel_t mic_mask,
                                          bool update_spectrum,
                                          RecRawMetrics* metrics_out)
{
    if (!metrics_out || !buffer || bytes_read == 0) {
        for (int ch = 0; ch < 3; ch++) {
            clear_latest_audio_metrics(ch, update_spectrum);
        }
        return false;
    }

    const size_t bytes_per_sample = static_cast<size_t>(fs.bits_per_sample) >> 3;
    if (bytes_per_sample < sizeof(int16_t)) {
        for (int ch = 0; ch < 3; ch++) {
            clear_latest_audio_metrics(ch, update_spectrum);
        }
        return false;
    }

    uint8_t enabled_indices[4] = {};
    const uint8_t mic_count = get_enabled_mic_indices(mic_mask, enabled_indices);
    if (mic_count == 0) {
        for (int ch = 0; ch < 3; ch++) {
            clear_latest_audio_metrics(ch, update_spectrum);
        }
        return false;
    }

    size_t samples_per_channel = 0;
    if (is_tdm) {
        const size_t frame_bytes = bytes_per_sample * 2;
        samples_per_channel = frame_bytes ? (bytes_read / frame_bytes) : 0;
    } else {
        const size_t channel_count = fs.channel ? static_cast<size_t>(fs.channel) : 1;
        const size_t frame_bytes = bytes_per_sample * channel_count;
        samples_per_channel = frame_bytes ? (bytes_read / frame_bytes) : 0;
    }

    if (samples_per_channel == 0) {
        for (int ch = 0; ch < 3; ch++) {
            clear_latest_audio_metrics(ch, update_spectrum);
        }
        return false;
    }

    RecRawMetrics& metrics = *metrics_out;
    init_raw_metrics(metrics, samples_per_channel);

    if (is_tdm) {
        const size_t frame_bytes = bytes_per_sample * 2;
        for (size_t frame = 0; frame < samples_per_channel; frame++) {
            const size_t base = frame * frame_bytes;
            int16_t left_high = 0;
            int16_t left_low = 0;
            int16_t right_high = 0;
            int16_t right_low = 0;

            if (bytes_per_sample == sizeof(uint32_t)) {
                const uint32_t left_word = load_u32_le(buffer + base);
                const uint32_t right_word = load_u32_le(buffer + base + bytes_per_sample);
                left_high = static_cast<int16_t>(left_word >> 16);
                left_low = static_cast<int16_t>(left_word & 0xFFFF);
                right_high = static_cast<int16_t>(right_word >> 16);
                right_low = static_cast<int16_t>(right_word & 0xFFFF);
            } else {
                const int16_t left_sample = load_i16_le(buffer + base);
                const int16_t right_sample = load_i16_le(buffer + base + bytes_per_sample);
                left_high = left_sample;
                left_low = left_sample;
                right_high = right_sample;
                right_low = right_sample;
            }

            if (mic_mask_has_channel(mic_mask, 0)) {
                record_raw_channel_sample(metrics, 0, left_high, frame, update_spectrum);
            }
            if (mic_mask_has_channel(mic_mask, 2)) {
                record_raw_channel_sample(metrics, 2, left_low, frame, update_spectrum);
            }
            if (mic_mask_has_channel(mic_mask, 1)) {
                record_raw_channel_sample(metrics, 1, right_high, frame, update_spectrum);
            }
            if (mic_mask_has_channel(mic_mask, 3)) {
                record_raw_channel_sample(metrics, 3, right_low, frame, update_spectrum);
            }
        }
    } else {
        const size_t channel_count = fs.channel ? static_cast<size_t>(fs.channel) : 1;
        const size_t frame_bytes = bytes_per_sample * channel_count;
        for (size_t i = 0; i < samples_per_channel; i++) {
            const size_t base = i * frame_bytes;
            const int16_t left_sample = load_i16_le(buffer + base);
            const int16_t right_sample =
                (channel_count >= 2) ? load_i16_le(buffer + base + bytes_per_sample) : left_sample;

            if (mic_count == 1) {
                const uint8_t mic_index = enabled_indices[0];
                const bool is_left_channel = (mic_index == 0 || mic_index == 2);
                record_raw_channel_sample(metrics,
                                          mic_index,
                                          is_left_channel ? left_sample : right_sample,
                                          i,
                                          update_spectrum);
            } else if (mic_count == 2) {
                record_raw_channel_sample(metrics, enabled_indices[0], left_sample, i, update_spectrum);
                record_raw_channel_sample(metrics, enabled_indices[1], right_sample, i, update_spectrum);
            } else {
                if (mic_mask_has_channel(mic_mask, 0)) {
                    record_raw_channel_sample(metrics, 0, left_sample, i, update_spectrum);
                }
                if (mic_mask_has_channel(mic_mask, 1)) {
                    record_raw_channel_sample(metrics, 1, right_sample, i, update_spectrum);
                }
                if (mic_mask_has_channel(mic_mask, 2)) {
                    record_raw_channel_sample(metrics, 2, left_sample, i, update_spectrum);
                }
                if (mic_mask_has_channel(mic_mask, 3)) {
                    record_raw_channel_sample(metrics, 3, right_sample, i, update_spectrum);
                }
            }
        }
    }

    apply_raw_metrics_to_latest(metrics, update_spectrum);
    return true;
}

void clear_latest_audio_metrics(int ch, bool clear_spectrum)
{
    if (ch < 0 || ch >= 3) {
        return;
    }

    s_rec_page.latest_peak[ch] = 0;
    s_rec_page.latest_rms[ch] = 0;
    clear_latest_wave_points(ch);

    if (clear_spectrum && ch >= 0 && ch < 3) {
        for (int bin = 0; bin < N_SPEC_BINS; bin++) {
            s_rec_page.latest_spectrum[ch][bin] = 0;
        }
    }
}

void update_rec_spectrogram()
{
    if (s_rec_page.display_mode != RecDisplayMode::SPECTRUM ||
        !s_rec_page.spectrum_active) {
        return;
    }

    for (int ch = 0; ch < 3; ch++) {
        if (ch == 2 && !s_rec_page.show_ref) {
            continue;
        }

        uint16_t* buf = s_rec_page.spec_buf[ch];
        if (!buf || !s_rec_page.spec_canvas[ch]) {
            continue;
        }

        for (int32_t y = 0; y < SPEC_CANVAS_H; y++) {
            uint16_t* row = buf + y * SPEC_CANVAS_W;
            std::memmove(row, row + SPEC_COL_W, (SPEC_CANVAS_W - SPEC_COL_W) * sizeof(uint16_t));
        }

        for (int bin = 0; bin < N_SPEC_BINS; bin++) {
            const int visual_bin = N_SPEC_BINS - 1 - bin;
            const uint16_t color = spec_color_from_level(s_rec_page.latest_spectrum[ch][bin]);
            const int32_t y0 = (visual_bin * SPEC_CANVAS_H) / N_SPEC_BINS;
            int32_t y1 = ((visual_bin + 1) * SPEC_CANVAS_H) / N_SPEC_BINS;
            if (y1 <= y0) {
                y1 = y0 + 1;
            } else if (y1 > SPEC_CANVAS_H) {
                y1 = SPEC_CANVAS_H;
            }
            for (int32_t y = y0; y < y1; y++) {
                uint16_t* row = buf + y * SPEC_CANVAS_W;
                for (int32_t x = SPEC_CANVAS_W - SPEC_COL_W; x < SPEC_CANVAS_W; x++) {
                    row[x] = color;
                }
            }
        }

        lv_obj_invalidate(s_rec_page.spec_canvas[ch]);
    }
}

void compute_spectrum_levels(const int16_t* samples, size_t count, volatile uint8_t out[N_SPEC_BINS])
{
    static constexpr uint8_t k_bins[N_SPEC_BINS] = {
        1, 2, 3, 4, 5, 6, 7, 8,
        10, 12, 14, 17, 20, 23, 27, 31,
        35, 39, 44, 49, 54, 58, 61, 63
    };

    if (!samples || count < SPEC_DFT_N) {
        for (int bin = 0; bin < N_SPEC_BINS; bin++) {
            out[bin] = 0;
        }
        return;
    }

    SpectrumTables* tables = s_spectrum_tables.load(std::memory_order_acquire);
    if (!tables && !s_spectrum_tables_allocation_failed.load(std::memory_order_acquire)) {
        auto* candidate = static_cast<SpectrumTables*>(
            heap_caps_calloc(1,
                             sizeof(SpectrumTables),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!candidate) {
            s_spectrum_tables_allocation_failed.store(true, std::memory_order_release);
            ESP_LOGE(TAG,
                     "Unable to allocate spectrum tables in PSRAM (%u bytes); spectrum disabled",
                     static_cast<unsigned>(sizeof(SpectrumTables)));
        } else {
            // Build the candidate before publishing it so the monitor task
            // never observes a partially initialized table.
            for (int bin = 0; bin < N_SPEC_BINS; bin++) {
                const double k = static_cast<double>(k_bins[bin]);
                for (int n = 0; n < SPEC_DFT_N; n++) {
                    const double phase = 2.0 * SPEC_PI * k * static_cast<double>(n) /
                                         static_cast<double>(SPEC_DFT_N);
                    candidate->cos_tbl[bin][n] = std::cos(phase);
                    candidate->sin_tbl[bin][n] = std::sin(phase);
                }
            }
            for (int n = 0; n < SPEC_DFT_N; n++) {
                candidate->window_tbl[n] =
                    0.5 - 0.5 * std::cos(2.0 * SPEC_PI * static_cast<double>(n) /
                                         static_cast<double>(SPEC_DFT_N - 1));
            }

            SpectrumTables* expected = nullptr;
            if (s_spectrum_tables.compare_exchange_strong(expected,
                                                          candidate,
                                                          std::memory_order_release,
                                                          std::memory_order_acquire)) {
                tables = candidate;
                ESP_LOGI(TAG,
                         "Spectrum tables allocated in PSRAM: bytes=%u psram_free=%u",
                         static_cast<unsigned>(sizeof(SpectrumTables)),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            } else {
                // A concurrent caller won the publication race.
                heap_caps_free(candidate);
                tables = expected;
            }
        }
    }

    if (!tables) {
        for (int bin = 0; bin < N_SPEC_BINS; bin++) {
            out[bin] = 0;
        }
        return;
    }

    int64_t sum = 0;
    for (int n = 0; n < SPEC_DFT_N; n++) {
        sum += samples[n];
    }
    const double mean = static_cast<double>(sum) / static_cast<double>(SPEC_DFT_N);

    for (int bin = 0; bin < N_SPEC_BINS; bin++) {
        double re = 0.0;
        double im = 0.0;
        for (int n = 0; n < SPEC_DFT_N; n++) {
            const double v = (static_cast<double>(samples[n]) - mean) *
                             tables->window_tbl[n];
            re += v * tables->cos_tbl[bin][n];
            im -= v * tables->sin_tbl[bin][n];
        }

        const double mag = (2.0 * std::sqrt(re * re + im * im)) /
                           (static_cast<double>(SPEC_DFT_N) * 32767.0);
        const double db = 20.0 * std::log10((mag > 1e-8) ? mag : 1e-8);
        double normalized = (db + 78.0) / 56.0;
        if (normalized < 0.0) {
            normalized = 0.0;
        } else if (normalized > 1.0) {
            normalized = 1.0;
        }
        out[bin] = static_cast<uint8_t>(normalized * 255.0);
    }
}

void set_record_buttons_enabled(bool enabled)
{
    const bool busy_stopping = s_rec_page.stop_requested || s_rec_page.stop_play_requested;
    const bool has_record = s_rec_page.last_record_bytes > 0 && s_rec_page.last_path[0] != '\0';

    if (s_rec_page.rec_button) {
        if (s_rec_page.session_state == RecSessionState::IDLE && !s_rec_page.task_running && !has_record) {
            set_button_text(s_rec_page.rec_button, "REC");
            set_button_colors(s_rec_page.rec_button,
                              enabled && !s_rec_page.play_running ? COLOR_PRIMARY : COLOR_BUTTON_BG,
                              enabled && !s_rec_page.play_running ? 0xFFFFFF : COLOR_TEXT);
            set_button_enabled(s_rec_page.rec_button, enabled && !s_rec_page.play_running);
        } else if (busy_stopping) {
            set_button_text(s_rec_page.rec_button, "ENDING...");
            set_button_colors(s_rec_page.rec_button, COLOR_WARNING, 0xFFFFFF);
            set_button_enabled(s_rec_page.rec_button, false);
        } else {
            set_button_text(s_rec_page.rec_button, "END REC");
            set_button_colors(s_rec_page.rec_button, COLOR_ERROR, 0xFFFFFF);
            set_button_enabled(s_rec_page.rec_button, true);
        }
    }

    if (s_rec_page.play_button) {
        if (s_rec_page.session_state == RecSessionState::PLAYING || s_rec_page.play_running) {
            set_button_text(s_rec_page.play_button, "END PLAY");
            set_button_colors(s_rec_page.play_button, PLAY_ACTIVE_COLOR, 0xFFFFFF);
            set_button_enabled(s_rec_page.play_button, !busy_stopping);
        } else if (has_record && s_rec_page.session_state == RecSessionState::RECORDED &&
                   !s_rec_page.task_running) {
            set_button_text(s_rec_page.play_button, "PLAY LAST");
            set_button_colors(s_rec_page.play_button, COLOR_PRIMARY, 0xFFFFFF);
            set_button_enabled(s_rec_page.play_button, enabled);
        } else {
            set_button_text(s_rec_page.play_button, "PLAY LAST");
            set_button_colors(s_rec_page.play_button, COLOR_BUTTON_BG, COLOR_TEXT);
            set_button_enabled(s_rec_page.play_button, false);
        }
    }

    if (s_rec_page.duration_slider) {
        if (enabled && s_rec_page.session_state == RecSessionState::IDLE &&
            !s_rec_page.task_running && !s_rec_page.play_running) {
            lv_obj_clear_state(s_rec_page.duration_slider, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_rec_page.duration_slider, LV_STATE_DISABLED);
        }
    }
}

void apply_ref_visibility()
{
    if (s_rec_page.waveform_rows[2]) {
        if (s_rec_page.show_ref) {
            lv_obj_remove_flag(s_rec_page.waveform_rows[2], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_rec_page.waveform_rows[2], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_rec_page.spec_rows[2]) {
        if (s_rec_page.show_ref) {
            lv_obj_remove_flag(s_rec_page.spec_rows[2], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_rec_page.spec_rows[2], LV_OBJ_FLAG_HIDDEN);
        }
    }

    const int32_t row_h = s_rec_page.show_ref ? WAVE_ROW_H_WITH_REF : WAVE_ROW_H_DEFAULT;
    for (int ch = 0; ch < 3; ch++) {
        if (s_rec_page.waveform_rows[ch]) {
            lv_obj_set_height(s_rec_page.waveform_rows[ch], row_h);
        }
        if (s_rec_page.waveform_charts[ch]) {
            lv_obj_set_height(s_rec_page.waveform_charts[ch], row_h - 10);
        }
    }

    const int32_t spec_row_h = s_rec_page.show_ref ? SPEC_ROW_H_WITH_REF : SPEC_ROW_H_DEFAULT;
    for (int ch = 0; ch < 3; ch++) {
        if (s_rec_page.spec_rows[ch]) {
            lv_obj_set_height(s_rec_page.spec_rows[ch], spec_row_h);
        }
        if (s_rec_page.spec_canvas[ch]) {
            lv_obj_set_size(s_rec_page.spec_canvas[ch], SPEC_CANVAS_W, SPEC_CANVAS_H);
        }
    }
    update_waveform_markers(s_rec_page.session_state == RecSessionState::RECORDED ||
                            s_rec_page.session_state == RecSessionState::PLAYING);
}

const char* rec_display_mode_name(RecDisplayMode mode)
{
    switch (mode) {
        case RecDisplayMode::WAVEFORM: return "Waveform";
        case RecDisplayMode::SPECTRUM: return "Spectrum";
        default: return "Unknown";
    }
}

void apply_rec_display_mode()
{
    const bool show_waveform = (s_rec_page.display_mode == RecDisplayMode::WAVEFORM);

    if (s_rec_page.waveform_card) {
        if (show_waveform) {
            lv_obj_remove_flag(s_rec_page.waveform_card, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_rec_page.waveform_card, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_rec_page.spectrum_card) {
        lv_obj_set_height(s_rec_page.spectrum_card,
                          show_waveform ? REC_DISPLAY_CARD_H : REC_CONTENT_INNER_H);
        if (show_waveform) {
            lv_obj_add_flag(s_rec_page.spectrum_card, LV_OBJ_FLAG_HIDDEN);
            s_rec_page.spectrum_active = false;
        } else {
            lv_obj_remove_flag(s_rec_page.spectrum_card, LV_OBJ_FLAG_HIDDEN);
            clear_rec_spectrogram(true);
        }
    }

    if (s_rec_page.record_button_card) {
        if (show_waveform) {
            lv_obj_remove_flag(s_rec_page.record_button_card, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_rec_page.record_button_card, LV_OBJ_FLAG_HIDDEN);
        }
    }

    apply_ref_visibility();
}

void refresh_settings_ref_button()
{
    if (!s_rec_page.settings_ref_button) {
        return;
    }

    set_button_text(s_rec_page.settings_ref_button, s_rec_page.show_ref ? "REF: ON" : "REF: OFF");
    set_button_colors(s_rec_page.settings_ref_button,
                      s_rec_page.show_ref ? COLOR_PRIMARY : COLOR_BUTTON_BG,
                      s_rec_page.show_ref ? 0xFFFFFF : COLOR_TEXT);
}

const char* channel_desc(audio_channels_t channels)
{
    switch (channels) {
        case AUDIO_CHANNELS_MONO: return "mono";
        case AUDIO_CHANNELS_STEREO: return "stereo";
        case AUDIO_CHANNELS_3CHs: return "3ch";
        case AUDIO_CHANNELS_4CHs: return "4ch";
        default: return "unknown";
    }
}

void refresh_rec_status_labels()
{
    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        return;
    }

    const bool audio_ready = tool->otool_tools.audio.is_system_initialized() &&
                             tool->otool_tools.audio.is_es7210_initialized();

    if (s_rec_page.status_label) {
        size_t total = 0;
        size_t used = 0;
        const esp_err_t storage_ret = get_rec_storage_info(&total, &used);

        char status_buf[160];
        if (storage_ret == ESP_OK) {
            const size_t free_bytes = (total > used) ? (total - used) : 0;
            std::snprintf(status_buf, sizeof(status_buf),
                          "Storage: %u/%u KB free | Audio: %s | Task: %s",
                          static_cast<unsigned>(free_bytes / 1024),
                          static_cast<unsigned>(total / 1024),
                          audio_ready ? "ready" : "not ready",
                          rec_session_state_name(s_rec_page.session_state));
        } else {
            std::snprintf(status_buf, sizeof(status_buf),
                          "Storage: not mounted | Audio: %s | Task: %s",
                          audio_ready ? "ready" : "not ready",
                          rec_session_state_name(s_rec_page.session_state));
        }
        set_label_text(s_rec_page.status_label, status_buf);
    }

    char format_buf[160];
    std::snprintf(format_buf, sizeof(format_buf),
                  "Format: %lu Hz, %d-bit, RX %s, MIC %s",
                  static_cast<unsigned long>(tool->otool_tools.audio.get_sample_rate()),
                  static_cast<int>(tool->otool_tools.audio.get_bits_per_sample()),
                  channel_desc(tool->otool_tools.audio.get_rx_channels()),
                  tool->otool_tools.audio.get_mic_channels_description(tool->otool_tools.audio.get_mic_channels()));
    if (s_rec_page.format_label) {
        set_label_text(s_rec_page.format_label, format_buf);
    }
}


void update_rec_ui_from_task(const char* result_text, uint32_t result_color)
{
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock LVGL for REC update");
        return;
    }

    if (s_rec_page.page_alive) {
        if (s_rec_page.session_state == RecSessionState::RECORDED) {
            s_rec_page.display_mode = RecDisplayMode::WAVEFORM;
            apply_rec_display_mode();
            clear_rec_spectrogram(false);
            render_recorded_waveform_view(0);
        } else if (s_rec_page.session_state == RecSessionState::IDLE) {
            clear_rec_waveforms();
            clear_rec_spectrogram(false);
        }

        refresh_rec_status_labels();
        set_record_buttons_enabled(!s_rec_page.task_running);
        set_rec_result_text(result_text, result_color);

        // 如果录音刚开始启动，清空波形图
        if (s_rec_page.task_running || s_rec_page.play_running) {
            (void)0;
        }
    }

    lvgl_port_unlock();
}

void monitor_task(void* /*param*/)
{
    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        ESP_LOGW(TAG, "monitor_task: no tool, exit");
        s_rec_page.monitor_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    auto* record_dev = tool->otool_tools.audio.get_record_device_handle();
    if (!record_dev) {
        ESP_LOGW(TAG, "monitor_task: no record device, exit");
        s_rec_page.monitor_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    const uint32_t sample_rate = tool->otool_tools.audio.get_sample_rate();
    const int bits = static_cast<int>(tool->otool_tools.audio.get_bits_per_sample());
    const bool is_tdm = tool->otool_tools.audio.is_es7210_tdm_mode();
    const audio_mic_channel_t mic_ch = tool->otool_tools.audio.get_mic_channels();
    const size_t bytes_per_frame = 2 * (bits / 8);
    const size_t chunk_bytes = (sample_rate * MONITOR_CHUNK_MS / 1000) * bytes_per_frame;

    uint8_t* buf = static_cast<uint8_t*>(malloc(chunk_bytes));
    if (!buf) {
        ESP_LOGE(TAG, "monitor_task: malloc failed (%u bytes)", static_cast<unsigned>(chunk_bytes));
        s_rec_page.monitor_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    RecRawMetrics* raw_metrics = static_cast<RecRawMetrics*>(
        heap_caps_malloc(sizeof(RecRawMetrics), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!raw_metrics) {
        raw_metrics = static_cast<RecRawMetrics*>(
            heap_caps_malloc(sizeof(RecRawMetrics), MALLOC_CAP_8BIT));
    }
    if (!raw_metrics) {
        ESP_LOGE(TAG, "monitor_task: metrics malloc failed (%u bytes)",
                 static_cast<unsigned>(sizeof(RecRawMetrics)));
        free(buf);
        s_rec_page.monitor_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = sample_rate;
    fs.channel = 2;
    fs.bits_per_sample = bits;

    ESP_LOGI(TAG, "monitor_task start: sr=%lu bits=%d tdm=%d chunk=%u",
             static_cast<unsigned long>(sample_rate),
             bits,
             static_cast<int>(is_tdm),
             static_cast<unsigned>(chunk_bytes));

    TickType_t last_log_tick = 0;

    while (s_rec_page.page_alive && s_rec_page.monitor_active) {
        if (s_rec_page.task_running || s_rec_page.play_running) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        s_rec_page.monitor_is_reading = true;
        if (s_rec_page.task_running) {
            s_rec_page.monitor_is_reading = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const int read_ret = esp_codec_dev_read(record_dev, buf, static_cast<int>(chunk_bytes));
        s_rec_page.monitor_is_reading = false;

        if (!s_rec_page.page_alive || !s_rec_page.monitor_active) {
            break;
        }
        if (read_ret != 0) {
            ESP_LOGW(TAG, "monitor_task: read err %d", read_ret);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const audio_mic_channel_t all_mic_mask =
            static_cast<audio_mic_channel_t>(AUDIO_MIC_CHANNEL_1 |
                                              AUDIO_MIC_CHANNEL_2 |
                                              AUDIO_MIC_CHANNEL_3 |
                                              AUDIO_MIC_CHANNEL_4);
        const audio_mic_channel_t eff_mask =
            static_cast<audio_mic_channel_t>(mic_ch & all_mic_mask);

        const bool update_spectrum = (s_rec_page.display_mode == RecDisplayMode::SPECTRUM);
        const bool metrics_ok = update_latest_audio_metrics_from_raw(buf,
                                                                     chunk_bytes,
                                                                     fs,
                                                                     is_tdm,
                                                                     eff_mask,
                                                                     update_spectrum,
                                                                     raw_metrics);

        const TickType_t now_tick = xTaskGetTickCount();
        if (metrics_ok &&
            (last_log_tick == 0 ||
             pdTICKS_TO_MS(now_tick - last_log_tick) >= MONITOR_LOG_PERIOD_MS)) {
            MicMonitorStats stats[4] = {};
            build_monitor_stats_from_raw(*raw_metrics, stats);
            log_mic_monitor_stats(stats, raw_metrics->samples_per_channel);
            last_log_tick = now_tick;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    heap_caps_free(raw_metrics);
    free(buf);
    ESP_LOGI(TAG, "monitor_task exit");
    s_rec_page.monitor_is_reading = false;
    s_rec_page.monitor_task_handle = nullptr;
    vTaskDelete(nullptr);
}

// LVGL 定时器回调：将最新峰值推送到图表并更新 dBFS 标签
void waveform_timer_cb(lv_timer_t* /*timer*/)
{
    if (!s_rec_page.page_alive) {
        return;
    }

    if (s_rec_page.session_state == RecSessionState::RECORDED) {
        render_recorded_waveform_view(0);
        update_rec_spectrogram();
        return;
    }

    if (s_rec_page.session_state == RecSessionState::PLAYING) {
        int32_t shift = 0;
        if (s_rec_page.playback_wave_active &&
            s_rec_page.playback_start_tick != 0 &&
            s_rec_page.playback_duration_ms > 0) {
            const uint32_t elapsed_ms =
                pdTICKS_TO_MS(xTaskGetTickCount() - s_rec_page.playback_start_tick);
            shift = static_cast<int32_t>(
                (static_cast<uint64_t>(elapsed_ms) * WAVE_REC_PREVIEW_PTS) /
                s_rec_page.playback_duration_ms);
        }
        render_recorded_waveform_view(shift);
        update_rec_spectrogram();
        return;
    }

    for (int ch = 0; ch < 3; ch++) {
        lv_obj_t* chart = s_rec_page.waveform_charts[ch];
        lv_chart_series_t* ser = s_rec_page.waveform_series[ch];
        lv_chart_series_t* rms_ser = s_rec_page.waveform_rms_series[ch];
        lv_obj_t* db_lbl = s_rec_page.db_labels[ch];

        if (!chart || !ser || !rms_ser || !db_lbl) {
            continue;
        }

        const int16_t peak = s_rec_page.latest_peak[ch];
        const int16_t rms = s_rec_page.latest_rms[ch];

        const bool chart_visible =
            (s_rec_page.display_mode == RecDisplayMode::WAVEFORM) &&
            !(ch == 2 && !s_rec_page.show_ref);
        if (!chart_visible) {
            continue;
        }

        int16_t wave_values[WAVE_PTS_PER_TICK];
        for (int pt = 0; pt < WAVE_PTS_PER_TICK; pt++) {
            wave_values[pt] = s_rec_page.latest_wave_y[ch][pt];
        }
        push_live_chart_points(chart, ser, rms_ser, wave_values, sample_to_wave_y(rms));

        // 计算 dBFS（-60 dBFS 下限）
        char db_buf[12];
        if (peak <= 1) {
            std::snprintf(db_buf, sizeof(db_buf), "-inf");
        } else {
            const double db = 20.0 * std::log10(static_cast<double>(peak) / 32767.0);
            if (db < -60.0) {
                std::snprintf(db_buf, sizeof(db_buf), "-inf");
            } else {
                std::snprintf(db_buf, sizeof(db_buf), "%+.0f", db);
            }
        }
        set_db_label_text_if_changed(ch, db_buf);
    }

    update_rec_spectrogram();
}

// ============================================================
// REC 批量录音任务（原有逻辑，增加对 monitor_is_reading 的等待）
// ============================================================

void rec_task(void* param)
{
    auto* action = static_cast<RecAction*>(param);
    char result_buf[256];
    result_buf[0] = '\0';
    uint32_t result_color = COLOR_ERROR;

    auto* tool = corep4_ui_sync_get_tool();
    esp_err_t ret = ESP_FAIL;

    if (!action) {
        std::snprintf(result_buf, sizeof(result_buf), "REC failed: invalid action");
        ret = ESP_ERR_INVALID_ARG;
    } else if (!tool) {
        std::snprintf(result_buf, sizeof(result_buf), "REC failed: device tool not ready");
        ret = ESP_ERR_INVALID_STATE;
    } else {
        const esp_err_t storage_ret = ensure_rec_storage_mounted();
        const bool audio_ready = tool->otool_tools.audio.is_system_initialized() &&
                                 tool->otool_tools.audio.is_es7210_initialized();

        if (storage_ret != ESP_OK) {
            std::snprintf(result_buf, sizeof(result_buf),
                          "REC failed: mount storage %s",
                          esp_err_to_name(storage_ret));
            ret = storage_ret;
        } else if (!audio_ready) {
            std::snprintf(result_buf, sizeof(result_buf), "REC failed: audio recorder is not ready");
            ret = ESP_ERR_INVALID_STATE;
        } else {
            // 确保 MIC1 和 MIC2 增益相同
            tool->otool_tools.audio.es7210_set_mic_channel_gain(AUDIO_MIC_CHANNEL_1, ES7210_MIC_GAIN_27DB);
            tool->otool_tools.audio.es7210_set_mic_channel_gain(AUDIO_MIC_CHANNEL_2, ES7210_MIC_GAIN_27DB);

            for (int w = 0; w < 20 && s_rec_page.monitor_is_reading; w++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            char file_path[96];
            std::snprintf(file_path, sizeof(file_path), "%s", REC_OUTPUT_FILE);

            const uint32_t sample_rate = tool->otool_tools.audio.get_sample_rate();
            const int bits = static_cast<int>(tool->otool_tools.audio.get_bits_per_sample());
            const bool is_tdm = tool->otool_tools.audio.is_es7210_tdm_mode();
            const audio_mic_channel_t mic_ch = tool->otool_tools.audio.get_mic_channels();

            const audio_channels_t rec_channels =
                (tool->otool_tools.audio.get_rx_channels() == AUDIO_CHANNELS_MONO)
                    ? AUDIO_CHANNELS_MONO
                    : AUDIO_CHANNELS_STEREO;
            const uint32_t record_seconds =
                action->duration_seconds > REC_MAX_SECONDS ? REC_MAX_SECONDS : action->duration_seconds;
            uint32_t bytes_per_sample = (bits * audio_channel_count(rec_channels)) / 8;
            size_t total_bytes = static_cast<size_t>(sample_rate * record_seconds * bytes_per_sample);

            clear_record_file_state(true);

            if (bytes_per_sample == 0 || total_bytes == 0) {
                std::snprintf(result_buf, sizeof(result_buf), "REC failed: invalid audio format");
                ret = ESP_ERR_INVALID_ARG;
            } else {
                uint8_t* rec_buffer = static_cast<uint8_t*>(
                    heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!rec_buffer) {
                    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                    std::snprintf(result_buf, sizeof(result_buf),
                                  "REC failed: PSRAM need %llu B free %llu B",
                                  static_cast<unsigned long long>(total_bytes),
                                  static_cast<unsigned long long>(psram_free));
                    ret = ESP_ERR_NO_MEM;
                } else {
                    ret = ESP_OK;
                    size_t chunk_size = (sample_rate * MONITOR_CHUNK_MS / 1000) * bytes_per_sample;
                    if (chunk_size < 512) {
                        chunk_size = 512;
                    } else if (chunk_size > 4096) {
                        chunk_size = 4096;
                    }
                    chunk_size -= chunk_size % bytes_per_sample;
                    if (chunk_size == 0) {
                        chunk_size = bytes_per_sample;
                    }

                    uint8_t* chunk_buffer = static_cast<uint8_t*>(malloc(chunk_size));
                    RecRawMetrics* raw_metrics = nullptr;
                    if (chunk_buffer) {
                        raw_metrics = static_cast<RecRawMetrics*>(
                            heap_caps_malloc(sizeof(RecRawMetrics), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
                        if (!raw_metrics) {
                            raw_metrics = static_cast<RecRawMetrics*>(
                                heap_caps_malloc(sizeof(RecRawMetrics), MALLOC_CAP_8BIT));
                        }
                    }
                    if (!chunk_buffer || !raw_metrics) {
                        if (raw_metrics) {
                            heap_caps_free(raw_metrics);
                        }
                        if (chunk_buffer) {
                            free(chunk_buffer);
                        }
                        std::snprintf(result_buf, sizeof(result_buf), "REC failed: no memory");
                        ret = ESP_ERR_NO_MEM;
                    } else {
                        size_t recorded_bytes = 0;
                        auto* record_dev = tool->otool_tools.audio.get_record_device_handle();

                        esp_codec_dev_sample_info_t fs = {};
                        fs.sample_rate = sample_rate;
                        fs.channel = 2;
                        fs.bits_per_sample = bits;

                        TickType_t start_tick = xTaskGetTickCount();

                        while (s_rec_page.page_alive &&
                               s_rec_page.task_running &&
                               !s_rec_page.stop_requested &&
                               recorded_bytes < total_bytes) {
                            const size_t remaining = total_bytes - recorded_bytes;
                            const size_t to_read = (chunk_size < remaining) ? chunk_size : remaining;
                            const int read_ret =
                                esp_codec_dev_read(record_dev, chunk_buffer, static_cast<int>(to_read));
                            if (read_ret != 0) {
                                ret = ESP_FAIL;
                                break;
                            }

                            // 录音期间只写 PSRAM，STOP/到时后再一次性写入 flash。
                            std::memcpy(rec_buffer + recorded_bytes, chunk_buffer, to_read);
                            recorded_bytes += to_read;

                            const audio_mic_channel_t target_mask =
                                static_cast<audio_mic_channel_t>(AUDIO_MIC_CHANNEL_1 |
                                                                  AUDIO_MIC_CHANNEL_2 |
                                                                  AUDIO_MIC_CHANNEL_3);
                            const audio_mic_channel_t eff_mask =
                                static_cast<audio_mic_channel_t>(mic_ch & target_mask);
                            const bool update_spectrum =
                                (s_rec_page.display_mode == RecDisplayMode::SPECTRUM);

                            update_latest_audio_metrics_from_raw(chunk_buffer,
                                                                 to_read,
                                                                 fs,
                                                                 is_tdm,
                                                                 eff_mask,
                                                                 update_spectrum,
                                                                 raw_metrics);
                        }

                        heap_caps_free(raw_metrics);
                        free(chunk_buffer);
                        uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - start_tick);

                        if (ret == ESP_OK && s_rec_page.discard_record_requested) {
                            clear_record_file_state(true);
                            std::snprintf(result_buf, sizeof(result_buf), "REC cleared");
                            result_color = COLOR_SUCCESS;
                        } else if (ret == ESP_OK &&
                            recorded_bytes > 0 &&
                            (recorded_bytes >= total_bytes || s_rec_page.stop_requested)) {
                            size_t storage_total = 0;
                            size_t storage_used = 0;
                            ret = get_rec_storage_info(&storage_total, &storage_used);
                            const size_t storage_free =
                                (ret == ESP_OK && storage_total > storage_used)
                                    ? (storage_total - storage_used)
                                    : 0;
                            if (ret != ESP_OK) {
                                std::snprintf(result_buf, sizeof(result_buf),
                                              "REC failed: storage info %s",
                                              esp_err_to_name(ret));
                            } else if (storage_free < recorded_bytes + STORAGE_RESERVE_BYTES) {
                                std::snprintf(result_buf, sizeof(result_buf),
                                              "REC failed: storage full need %llu B free %llu B",
                                              static_cast<unsigned long long>(recorded_bytes),
                                              static_cast<unsigned long long>(storage_free));
                                ret = ESP_ERR_NO_MEM;
                            } else {
                                FILE* file = fopen(file_path, "wb");
                                if (!file) {
                                    const int open_errno = errno;
                                    std::snprintf(result_buf, sizeof(result_buf),
                                                  "REC failed: open %s fail (%d:%s)",
                                                  file_path,
                                                  open_errno,
                                                  std::strerror(open_errno));
                                    ret = ESP_FAIL;
                                } else {
                                    size_t bytes_saved = 0;
                                    while (bytes_saved < recorded_bytes) {
                                        const size_t remaining = recorded_bytes - bytes_saved;
                                        const size_t to_write =
                                            (chunk_size < remaining) ? chunk_size : remaining;
                                        const size_t written =
                                            fwrite(rec_buffer + bytes_saved, 1, to_write, file);
                                        if (written != to_write) {
                                            ret = ESP_FAIL;
                                            break;
                                        }
                                        bytes_saved += written;
                                    }
                                    fflush(file);
                                    fclose(file);

                                    if (ret == ESP_OK) {
                                        std::snprintf(s_rec_page.last_path,
                                                      sizeof(s_rec_page.last_path),
                                                      "%s",
                                                      file_path);
                                        s_rec_page.last_record_bytes = recorded_bytes;
                                        s_rec_page.last_record_sample_rate = sample_rate;
                                        s_rec_page.last_record_channels = rec_channels;
                                        s_rec_page.last_record_bits =
                                            tool->otool_tools.audio.get_bits_per_sample();
                                        s_rec_page.playback_duration_ms =
                                            record_duration_ms_from_bytes(s_rec_page.last_record_bytes,
                                                                          s_rec_page.last_record_sample_rate,
                                                                          s_rec_page.last_record_channels,
                                                                          s_rec_page.last_record_bits);
                                        const audio_mic_channel_t target_mask =
                                            static_cast<audio_mic_channel_t>(AUDIO_MIC_CHANNEL_1 |
                                                                              AUDIO_MIC_CHANNEL_2 |
                                                                              AUDIO_MIC_CHANNEL_3);
                                        const audio_mic_channel_t eff_mask =
                                            static_cast<audio_mic_channel_t>(mic_ch & target_mask);
                                        build_recorded_wave_preview_from_buffer(rec_buffer,
                                                                                recorded_bytes,
                                                                                sample_rate,
                                                                                s_rec_page.last_record_bits,
                                                                                is_tdm,
                                                                                eff_mask);
                                        std::snprintf(result_buf,
                                                      sizeof(result_buf),
                                                      "%s | %llu B | %s",
                                                      "REC done",
                                                      static_cast<unsigned long long>(recorded_bytes),
                                                      file_path);
                                        result_color = COLOR_SUCCESS;
                                    } else {
                                        std::snprintf(result_buf,
                                                      sizeof(result_buf),
                                                      "REC failed: save %llu/%llu B",
                                                      static_cast<unsigned long long>(bytes_saved),
                                                      static_cast<unsigned long long>(recorded_bytes));
                                    }
                                }
                            }
                        } else if (ret == ESP_OK && s_rec_page.stop_requested) {
                            std::snprintf(result_buf, sizeof(result_buf), "REC stopped: no data saved");
                            ret = ESP_ERR_INVALID_SIZE;
                        } else if (ret != ESP_OK) {
                            std::snprintf(result_buf,
                                          sizeof(result_buf),
                                          "REC failed: %lu ms",
                                          static_cast<unsigned long>(elapsed_ms));
                        } else {
                            std::snprintf(result_buf, sizeof(result_buf), "REC failed: no data saved");
                            ret = ESP_ERR_INVALID_SIZE;
                        }
                    }
                    heap_caps_free(rec_buffer);
                }
            }
        }
    }

    if (ret != ESP_OK) {
        clear_record_file_state(true);
        s_rec_page.session_state = RecSessionState::IDLE;
        ESP_LOGW(TAG, "REC task failed: %s", result_buf);
    } else {
        s_rec_page.session_state =
            (s_rec_page.last_record_bytes > 0 && s_rec_page.last_path[0] != '\0')
                ? RecSessionState::RECORDED
                : RecSessionState::IDLE;
        ESP_LOGI(TAG, "REC task done: %s", result_buf);
    }

    s_rec_page.task_running = false;
    s_rec_page.stop_requested = false;
    s_rec_page.discard_record_requested = false;
    update_rec_ui_from_task(result_buf, result_color);

    delete action;
    vTaskDelete(nullptr);
}

void update_rec_ui_from_play_task(const char* result_text, uint32_t result_color)
{
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock LVGL for REC play update");
        return;
    }

    if (s_rec_page.page_alive) {
        if (s_rec_page.last_record_bytes > 0 && s_rec_page.last_path[0] != '\0') {
            s_rec_page.session_state = RecSessionState::RECORDED;
            s_rec_page.playback_wave_active = false;
            render_recorded_waveform_view(0);
        } else {
            s_rec_page.session_state = RecSessionState::IDLE;
            clear_rec_waveforms();
            clear_rec_spectrogram(false);
        }
        refresh_rec_status_labels();
        set_record_buttons_enabled(!s_rec_page.task_running);
        if (s_rec_page.session_state == RecSessionState::IDLE) {
            set_rec_result_text("Ready.", COLOR_MUTED);
        } else {
            set_rec_result_text(result_text, result_color);
        }
    }

    lvgl_port_unlock();
}

void rec_play_task(void* /*param*/)
{
    char result_buf[160];
    uint32_t result_color = COLOR_ERROR;
    esp_err_t ret = ESP_FAIL;

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool || !tool->otool_tools.audio.is_system_initialized() ||
        !tool->otool_tools.audio.is_es8311_initialized()) {
        std::snprintf(result_buf, sizeof(result_buf), "PLAY failed: audio playback is not ready");
        ret = ESP_ERR_INVALID_STATE;
    } else if (s_rec_page.last_record_bytes == 0 || s_rec_page.last_path[0] == '\0') {
        std::snprintf(result_buf, sizeof(result_buf), "PLAY failed: no recording");
        ret = ESP_ERR_NOT_FOUND;
    } else {
        FILE* file = fopen(s_rec_page.last_path, "rb");
        if (!file) {
            const int open_errno = errno;
            std::snprintf(result_buf, sizeof(result_buf),
                          "PLAY failed: open %s (%d:%s)",
                          s_rec_page.last_path,
                          open_errno,
                          std::strerror(open_errno));
            ret = ESP_FAIL;
        } else {
            uint8_t* buffer = static_cast<uint8_t*>(
                heap_caps_malloc(s_rec_page.last_record_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!buffer) {
                buffer = static_cast<uint8_t*>(
                    heap_caps_malloc(s_rec_page.last_record_bytes, MALLOC_CAP_8BIT));
            }

            if (!buffer) {
                std::snprintf(result_buf, sizeof(result_buf),
                              "PLAY failed: no memory (%llu B)",
                              static_cast<unsigned long long>(s_rec_page.last_record_bytes));
                ret = ESP_ERR_NO_MEM;
            } else {
                const size_t bytes_read = fread(buffer, 1, s_rec_page.last_record_bytes, file);
                if (bytes_read == 0) {
                    std::snprintf(result_buf, sizeof(result_buf), "PLAY failed: empty file");
                    ret = ESP_ERR_INVALID_SIZE;
                } else if (s_rec_page.stop_play_requested) {
                    std::snprintf(result_buf, sizeof(result_buf), "PLAY stopped");
                    ret = ESP_ERR_INVALID_STATE;
                } else {
                    auto* pb = tool->otool_tools.audio.get_playback();
                    ret = pb ? pb->play_audio_buffer(buffer,
                                                     bytes_read,
                                                     s_rec_page.last_record_sample_rate,
                                                     s_rec_page.last_record_channels,
                                                     s_rec_page.last_record_bits,
                                                     AUDIO_PLAYBACK_ASYNC)
                             : ESP_ERR_INVALID_STATE;
                    if (ret == ESP_OK && pb) {
                        s_rec_page.playback_start_tick = xTaskGetTickCount();
                        s_rec_page.playback_wave_active = true;
                        while (pb->is_async_playback_running()) {
                            if (!s_rec_page.page_alive || s_rec_page.stop_play_requested) {
                                pb->stop_async_playback();
                                break;
                            }
                            vTaskDelay(pdMS_TO_TICKS(50));
                        }

                        if (s_rec_page.stop_play_requested) {
                            std::snprintf(result_buf, sizeof(result_buf), "PLAY stopped");
                            result_color = COLOR_WARNING;
                            ret = ESP_ERR_INVALID_STATE;
                        } else {
                            std::snprintf(result_buf, sizeof(result_buf),
                                          "PLAY done | %llu B",
                                          static_cast<unsigned long long>(bytes_read));
                            result_color = COLOR_SUCCESS;
                        }
                    } else {
                        std::snprintf(result_buf, sizeof(result_buf),
                                      "PLAY failed: %s",
                                      esp_err_to_name(ret));
                    }
                }
                heap_caps_free(buffer);
            }
            fclose(file);
        }
    }

    if (ret != ESP_OK && result_color != COLOR_WARNING) {
        ESP_LOGW(TAG, "REC playback failed: %s", result_buf);
    } else {
        ESP_LOGI(TAG, "REC playback done: %s", result_buf);
    }

    s_rec_page.play_running = false;
    s_rec_page.stop_play_requested = false;
    s_rec_page.play_task_handle = nullptr;
    s_rec_page.playback_wave_active = false;
    update_rec_ui_from_play_task(result_buf, result_color);
    vTaskDelete(nullptr);
}

void end_rec_to_initial_state()
{
    if (s_rec_page.task_running) {
        s_rec_page.discard_record_requested = true;
        s_rec_page.stop_requested = true;
        set_record_buttons_enabled(false);
        set_rec_result_text("Ending REC...", COLOR_WARNING);
        return;
    }

    if (s_rec_page.play_running) {
        s_rec_page.stop_play_requested = true;
        auto* tool = corep4_ui_sync_get_tool();
        auto* pb = tool ? tool->otool_tools.audio.get_playback() : nullptr;
        if (pb && pb->is_async_playback_running()) {
            pb->stop_async_playback();
        }
    }

    clear_record_file_state(true);
    s_rec_page.session_state = RecSessionState::IDLE;
    clear_rec_waveforms();
    clear_rec_spectrogram(false);
    refresh_rec_status_labels();
    set_record_buttons_enabled(!s_rec_page.play_running);
    set_rec_result_text(s_rec_page.play_running ? "Ending PLAY..." : "Ready.", COLOR_MUTED);
}

void start_rec_action(const RecAction& action)
{
    if (s_rec_page.task_running) {
        end_rec_to_initial_state();
        return;
    }
    if (s_rec_page.play_running) {
        end_rec_to_initial_state();
        return;
    }
    if (s_rec_page.session_state != RecSessionState::IDLE ||
        s_rec_page.last_record_bytes > 0) {
        end_rec_to_initial_state();
        return;
    }

    s_rec_page.task_running = true;
    s_rec_page.stop_requested = false;
    s_rec_page.discard_record_requested = false;
    s_rec_page.session_state = RecSessionState::RECORDING;
    s_rec_page.sequence++;
    clear_rec_spectrogram(true);
    clear_rec_waveforms();
    set_record_buttons_enabled(false);
    refresh_rec_status_labels();

    char running_buf[160];
    std::snprintf(running_buf, sizeof(running_buf),
                  "Recording raw PCM for %lu s...",
                  static_cast<unsigned long>(action.duration_seconds > REC_MAX_SECONDS
                                                 ? REC_MAX_SECONDS
                                                 : action.duration_seconds));
    set_label_text(s_rec_page.result_label, running_buf);
    if (s_rec_page.result_label) {
        lv_obj_set_style_text_color(s_rec_page.result_label, lv_color_hex(COLOR_WARNING), LV_PART_MAIN);
    }

    auto* task_action = new RecAction(action);
    BaseType_t task_ret = create_rec_audio_task(rec_task,
                                                "func_rec",
                                                8192,
                                                task_action,
                                                REC_WORKER_TASK_PRIORITY,
                                                nullptr);
    if (task_ret != pdPASS) {
        delete task_action;
        s_rec_page.task_running = false;
        s_rec_page.stop_requested = false;
        s_rec_page.discard_record_requested = false;
        s_rec_page.session_state = RecSessionState::IDLE;
        set_record_buttons_enabled(true);
        refresh_rec_status_labels();
        set_rec_result_text("REC failed: cannot create worker task", COLOR_ERROR);
    }
}

void rec_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    RecAction action = {REC_MAX_SECONDS};
    start_rec_action(action);
}

void play_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    if (s_rec_page.play_running) {
        s_rec_page.stop_play_requested = true;
        auto* tool = corep4_ui_sync_get_tool();
        auto* pb = tool ? tool->otool_tools.audio.get_playback() : nullptr;
        if (pb && pb->is_async_playback_running()) {
            pb->stop_async_playback();
        }
        s_rec_page.playback_wave_active = false;
        render_recorded_waveform_view(0);
        set_record_buttons_enabled(false);
        set_rec_result_text("Ending PLAY...", COLOR_WARNING);
        return;
    }

    if (s_rec_page.task_running ||
        s_rec_page.session_state != RecSessionState::RECORDED ||
        s_rec_page.last_record_bytes == 0 ||
        s_rec_page.last_path[0] == '\0') {
        return;
    }

    s_rec_page.play_running = true;
    s_rec_page.stop_play_requested = false;
    s_rec_page.session_state = RecSessionState::PLAYING;
    s_rec_page.playback_duration_ms =
        record_duration_ms_from_bytes(s_rec_page.last_record_bytes,
                                      s_rec_page.last_record_sample_rate,
                                      s_rec_page.last_record_channels,
                                      s_rec_page.last_record_bits);
    s_rec_page.playback_start_tick = xTaskGetTickCount();
    s_rec_page.playback_wave_active = true;
    s_rec_page.display_mode = RecDisplayMode::WAVEFORM;
    apply_rec_display_mode();
    render_recorded_waveform_view(0);
    set_record_buttons_enabled(false);
    set_rec_result_text("Playing last REC...", COLOR_WARNING);

    BaseType_t task_ret = create_rec_audio_task(rec_play_task,
                                                "rec_play",
                                                8192,
                                                nullptr,
                                                REC_WORKER_TASK_PRIORITY,
                                                &s_rec_page.play_task_handle);
    if (task_ret != pdPASS) {
        s_rec_page.play_running = false;
        s_rec_page.play_task_handle = nullptr;
        s_rec_page.playback_wave_active = false;
        s_rec_page.session_state = RecSessionState::RECORDED;
        set_record_buttons_enabled(true);
        set_rec_result_text("PLAY failed: cannot create worker task", COLOR_ERROR);
    }
}

void rec_settings_waveform_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    s_rec_page.display_mode = RecDisplayMode::WAVEFORM;
    apply_rec_display_mode();
    corep4_ui_close_function_dialog(&s_rec_page.settings_dialog);
    s_rec_page.settings_ref_button = nullptr;
}

void rec_settings_spectrum_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    s_rec_page.display_mode = RecDisplayMode::SPECTRUM;
    apply_rec_display_mode();
    corep4_ui_close_function_dialog(&s_rec_page.settings_dialog);
    s_rec_page.settings_ref_button = nullptr;
}

void rec_settings_close_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    corep4_ui_close_function_dialog(&s_rec_page.settings_dialog);
    s_rec_page.settings_ref_button = nullptr;
}

void rec_settings_ref_toggle_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    s_rec_page.show_ref = !s_rec_page.show_ref;
    apply_ref_visibility();
    refresh_settings_ref_button();
}

void rec_set_button_event_cb(lv_event_t* e)
{
    if (e && lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    corep4_ui_close_function_dialog(&s_rec_page.settings_dialog);
    s_rec_page.settings_ref_button = nullptr;

    char message[192];
    std::snprintf(message, sizeof(message),
                  "Current: %s\n"
                  "Waveform is the default REC view.\n"
                  "Spectrum stays blank until REC starts.\n"
                  "REF is MIC3 and is hidden by default.",
                  rec_display_mode_name(s_rec_page.display_mode));

    const bool waveform_active = (s_rec_page.display_mode == RecDisplayMode::WAVEFORM);
    const CoreP4FunctionDialogButtonConfig buttons[] = {
        {"Waveform",
         rec_settings_waveform_cb,
         nullptr,
         waveform_active ? COLOR_PRIMARY : COLOR_BUTTON_BG,
         waveform_active ? 0xFFFFFF : COLOR_TEXT,
         false},
        {"Spectrum",
         rec_settings_spectrum_cb,
         nullptr,
         waveform_active ? COLOR_BUTTON_BG : COLOR_PRIMARY,
         waveform_active ? COLOR_TEXT : 0xFFFFFF,
         false},
        {"Close",
         rec_settings_close_cb,
         nullptr,
         COLOR_BUTTON_BG,
         COLOR_TEXT,
         false},
    };

    s_rec_page.settings_dialog = corep4_ui_open_function_dialog("REC Set",
                                                                 message,
                                                                 CoreP4FunctionDialogSize::MEDIUM,
                                                                 buttons,
                                                                 sizeof(buttons) / sizeof(buttons[0]));
    lv_obj_t* content = lv_msgbox_get_content(s_rec_page.settings_dialog.box);
    if (content) {
        lv_obj_t* row = add_row(content, 42);
        lv_obj_set_style_margin_top(row, 8, LV_PART_MAIN);
        add_label(row, "Reference channel", COLOR_MUTED, 190);
        s_rec_page.settings_ref_button = add_button(row,
                                                    s_rec_page.show_ref ? "REF: ON" : "REF: OFF",
                                                    s_rec_page.show_ref ? COLOR_PRIMARY : COLOR_BUTTON_BG,
                                                    s_rec_page.show_ref ? 0xFFFFFF : COLOR_TEXT,
                                                    132,
                                                    34);
        lv_obj_add_event_cb(s_rec_page.settings_ref_button,
                            rec_settings_ref_toggle_cb,
                            LV_EVENT_CLICKED,
                            nullptr);
    }
}

void rec_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    s_rec_page.page_alive = false;
    s_rec_page.monitor_active = false;
    s_rec_page.stop_requested = true;
    s_rec_page.stop_play_requested = true;
    corep4_ui_close_function_dialog(&s_rec_page.settings_dialog);
    s_rec_page.settings_ref_button = nullptr;

    // 停止波形刷新定时器
    if (s_rec_page.waveform_timer) {
        lv_timer_delete(s_rec_page.waveform_timer);
        s_rec_page.waveform_timer = nullptr;
    }

    // 清空 UI 对象指针
    s_rec_page.status_label      = nullptr;
    s_rec_page.format_label      = nullptr;
    s_rec_page.result_label      = nullptr;
    s_rec_page.rec_button        = nullptr;
    s_rec_page.play_button       = nullptr;
    s_rec_page.duration_slider   = nullptr;
    s_rec_page.duration_label    = nullptr;
    s_rec_page.record_button_card = nullptr;
    s_rec_page.waveform_card     = nullptr;
    s_rec_page.spectrum_card     = nullptr;
    for (int ch = 0; ch < 3; ch++) {
        s_rec_page.waveform_rows[ch] = nullptr;
        s_rec_page.waveform_charts[ch] = nullptr;
        s_rec_page.waveform_series[ch] = nullptr;
        s_rec_page.waveform_rms_series[ch] = nullptr;
        s_rec_page.waveform_markers[ch] = nullptr;
        s_rec_page.db_labels[ch]       = nullptr;
    }
    for (int ch = 0; ch < 3; ch++) {
        s_rec_page.spec_rows[ch] = nullptr;
        s_rec_page.spec_canvas[ch] = nullptr;
        s_rec_page.spec_buf[ch] = nullptr;
        for (int bin = 0; bin < N_SPEC_BINS; bin++) {
            s_rec_page.latest_spectrum[ch][bin] = 0;
        }
    }
    s_rec_page.spectrum_active = false;
}

void build_content(lv_obj_t* parent, const CoreP4FunctionWindowConfig& config)
{
    (void)config;

    prepare_content(parent);
    lv_obj_set_style_pad_all(parent, REC_CONTENT_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, REC_CONTENT_ROW_GAP, LV_PART_MAIN);
    lv_obj_set_style_pad_column(parent, 0, LV_PART_MAIN);

    s_rec_page.page_alive = true;
    s_rec_page.display_mode = RecDisplayMode::WAVEFORM;
    s_rec_page.show_ref = false;
    s_rec_page.stop_requested = false;
    s_rec_page.stop_play_requested = false;
    lv_obj_add_event_cb(parent, rec_content_delete_event_cb, LV_EVENT_DELETE, nullptr);

    auto* tool = corep4_ui_sync_get_tool();
    ensure_rec_storage_mounted();
    if (s_rec_page.task_running) {
        s_rec_page.session_state = RecSessionState::RECORDING;
    } else if (s_rec_page.play_running) {
        s_rec_page.session_state = RecSessionState::PLAYING;
    } else {
        restore_record_file_from_storage();
    }

    // ---- 默认显示：MIC Monitor 单屏波形 ----
    lv_obj_t* wave_card = add_card(parent, REC_DISPLAY_CARD_H);
    s_rec_page.waveform_card = wave_card;
    lv_obj_set_style_pad_all(wave_card, REC_DISPLAY_CARD_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(wave_card, REC_CONTENT_ROW_GAP, LV_PART_MAIN);

    // 标题行
    lv_obj_t* wave_title_row = add_row(wave_card, WAVE_TITLE_ROW_H);
    add_label(wave_title_row, "MIC Monitor", COLOR_MUTED, 260);
    lv_obj_t* wave_hint = add_label(wave_title_row, "Waveform", COLOR_PRIMARY, 110);
    lv_obj_set_style_text_align(wave_hint, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    // 通道名称与颜色映射
    static const char* const CH_NAMES[3]   = {"MIC1", "MIC2", "REF"};
    static const uint32_t    CH_COLORS[3]  = {WAVE_COLOR_MIC1, WAVE_COLOR_MIC2, WAVE_COLOR_MIC3};

    for (int ch = 0; ch < 3; ch++) {
        // 每通道一行：[MIC1/2/3 标签][图表填充][dBFS 标签]
        lv_obj_t* row = add_row(wave_card, WAVE_ROW_H_DEFAULT);
        s_rec_page.waveform_rows[ch] = row;
        lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);

        // 通道名标签（固定 42 px）
        lv_obj_t* ch_lbl = add_label(row, CH_NAMES[ch], COLOR_MUTED, 42);
        lv_obj_set_style_text_align(ch_lbl, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);

        // lv_chart 波形图（自动填充剩余宽度）
        lv_obj_t* chart = lv_chart_create(row);
        lv_obj_set_size(chart, LV_SIZE_CONTENT, WAVE_ROW_H_DEFAULT - 10);
        lv_obj_set_flex_grow(chart, 1);
        lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
        lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
        lv_chart_set_point_count(chart, N_WAVE_PTS);
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
        lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR); // 隐藏数据点标记
        // 图表背景与边框样式
        lv_obj_set_style_bg_color(chart, lv_color_hex(WAVE_BG_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(chart, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(chart, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_pad_all(chart, 1, LV_PART_MAIN);
        lv_chart_set_div_line_count(chart, 5, 5);
        lv_obj_set_style_line_color(chart, lv_color_hex(0x334155), LV_PART_MAIN);
        lv_obj_set_style_line_opa(chart, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_bg_color(chart, lv_color_hex(WAVE_BG_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t* marker = lv_obj_create(chart);
        lv_obj_remove_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(marker, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(marker, lv_color_hex(WAVE_MARKER_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(marker, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(marker, 0, LV_PART_MAIN);
        lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);

        // 添加数据序列并设置颜色
        lv_chart_series_t* ser = lv_chart_add_series(chart,
                                                       lv_color_hex(CH_COLORS[ch]),
                                                       LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_series_t* rms_ser = lv_chart_add_series(chart,
                                                          lv_color_hex(0xF8FAFC),
                                                          LV_CHART_AXIS_PRIMARY_Y);
        // 初始化到零点，让图表一打开就有清楚的零位参考
        lv_chart_set_all_values(chart, ser, WAVE_ZERO_Y);
        lv_chart_set_all_values(chart, rms_ser, WAVE_ZERO_Y);
        clear_latest_wave_points(ch);

        s_rec_page.waveform_charts[ch] = chart;
        s_rec_page.waveform_series[ch] = ser;
        s_rec_page.waveform_rms_series[ch] = rms_ser;
        s_rec_page.waveform_markers[ch] = marker;

        // dBFS 标签（固定 48 px，右对齐）
        lv_obj_t* db_lbl = add_label(row, "-inf", COLOR_MUTED, 48);
        lv_obj_set_style_text_align(db_lbl, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
        s_rec_page.db_labels[ch] = db_lbl;
    }
    apply_ref_visibility();

    // ---- 可切换显示：MIC1/MIC2 录音频谱图 ----
    lv_obj_t* spec_card = add_card(parent, REC_DISPLAY_CARD_H);
    s_rec_page.spectrum_card = spec_card;
    lv_obj_set_style_pad_all(spec_card, REC_DISPLAY_CARD_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(spec_card, REC_CONTENT_ROW_GAP, LV_PART_MAIN);

    lv_obj_t* spec_title_row = add_row(spec_card, SPEC_TITLE_ROW_H);
    add_label(spec_title_row, "Spectrum", COLOR_MUTED, 260);
    lv_obj_t* spec_hint = add_label(spec_title_row, "Live", COLOR_PRIMARY, 110);
    lv_obj_set_style_text_align(spec_hint, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    static const char* const SPEC_NAMES[3] = {"M\nI\nC\n1", "M\nI\nC\n2", "R\nE\nF"};
    for (int ch = 0; ch < 3; ch++) {
        lv_obj_t* row = add_row(spec_card, SPEC_ROW_H_DEFAULT);
        s_rec_page.spec_rows[ch] = row;
        lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);

        lv_obj_t* name = add_label(row, SPEC_NAMES[ch], COLOR_MUTED, 20);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

        lv_obj_t* axis = add_label(row, "8k\n6k\n4k\n2k\n1k", COLOR_MUTED, 28);
        lv_obj_set_style_text_align(axis, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

        const size_t spec_buf_size = SPEC_CANVAS_W * SPEC_CANVAS_H * sizeof(uint16_t);
        s_rec_page.spec_buf[ch] = static_cast<uint16_t*>(alloc_rec_canvas_buffer(spec_buf_size));
        if (!s_rec_page.spec_buf[ch]) {
            ESP_LOGE(TAG, "Failed to allocate spectrum canvas buffer for MIC%d (%u bytes)",
                     ch + 1,
                     static_cast<unsigned>(spec_buf_size));
            continue;
        }
        std::memset(s_rec_page.spec_buf[ch], 0, spec_buf_size);

        lv_obj_t* canvas = lv_canvas_create(row);
        lv_canvas_set_buffer(canvas,
                             s_rec_page.spec_buf[ch],
                             SPEC_CANVAS_W,
                             SPEC_CANVAS_H,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(canvas, SPEC_CANVAS_W, SPEC_CANVAS_H);
        lv_obj_set_style_border_width(canvas, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(canvas, lv_color_hex(0x334155), LV_PART_MAIN);
        s_rec_page.spec_canvas[ch] = canvas;
        lv_obj_add_event_cb(canvas,
                            rec_spectrum_canvas_delete_event_cb,
                            LV_EVENT_DELETE,
                            s_rec_page.spec_buf[ch]);
    }
    clear_rec_spectrogram(false);
    apply_rec_display_mode();

    // ---- 录音按钮卡片 (置于底部，按键加大) ----
    lv_obj_t* btn_card = add_card(parent, REC_BUTTON_CARD_H);
    s_rec_page.record_button_card = btn_card;
    lv_obj_set_style_pad_all(btn_card, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_row(btn_card, REC_CONTENT_ROW_GAP, LV_PART_MAIN);

    lv_obj_t* btn_row = add_row(btn_card, REC_BUTTON_H);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_rec_page.rec_button = add_button(btn_row, "REC", COLOR_PRIMARY, 0xFFFFFF, 404, REC_BUTTON_H);
    lv_obj_add_event_cb(s_rec_page.rec_button, rec_button_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* play_row = add_row(btn_card, PLAY_BUTTON_H);
    lv_obj_set_flex_align(play_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_rec_page.play_button = add_button(play_row, "PLAY LAST", COLOR_BUTTON_BG, COLOR_TEXT, 404, PLAY_BUTTON_H);
    lv_obj_add_event_cb(s_rec_page.play_button, play_button_event_cb, LV_EVENT_CLICKED, nullptr);

    refresh_rec_status_labels();
    set_record_buttons_enabled(!s_rec_page.task_running);
    if (s_rec_page.task_running) {
        set_label_text(s_rec_page.result_label, "Recording is still running in background...");
        if (s_rec_page.result_label) {
            lv_obj_set_style_text_color(s_rec_page.result_label, lv_color_hex(COLOR_WARNING), LV_PART_MAIN);
        }
    } else if (s_rec_page.session_state == RecSessionState::RECORDED &&
               s_rec_page.last_path[0] != '\0') {
        render_recorded_waveform_view(0);
        char last_buf[160];
        std::snprintf(last_buf, sizeof(last_buf), "Last REC ready: %s", s_rec_page.last_path);
        set_rec_result_text(last_buf, COLOR_SUCCESS);
    } else {
        clear_rec_waveforms();
        set_rec_result_text("Ready.", COLOR_MUTED);
    }

    // ---- 确保 MIC1 和 MIC2 增益相同 ----
    if (tool && tool->otool_tools.audio.is_es7210_initialized()) {
        tool->otool_tools.audio.es7210_set_mic_channel_gain(AUDIO_MIC_CHANNEL_1, ES7210_MIC_GAIN_27DB);
        tool->otool_tools.audio.es7210_set_mic_channel_gain(AUDIO_MIC_CHANNEL_2, ES7210_MIC_GAIN_27DB);
    }

    s_rec_page.monitor_active = true;
    if (!s_rec_page.monitor_task_handle) {
        BaseType_t mon_ret = create_rec_audio_task(monitor_task,
                                                   "rec_mon",
                                                   6144,
                                                   nullptr,
                                                   REC_MONITOR_TASK_PRIORITY,
                                                   &s_rec_page.monitor_task_handle);
        if (mon_ret != pdPASS) {
            ESP_LOGW(TAG, "Failed to create monitor_task");
            s_rec_page.monitor_active = false;
            s_rec_page.monitor_task_handle = nullptr;
        }
    }

    s_rec_page.waveform_timer = lv_timer_create(waveform_timer_cb, MONITOR_PERIOD_MS, nullptr);
    lv_timer_set_repeat_count(s_rec_page.waveform_timer, -1); // 持续运行
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "REC",
    "click on ui_func1rec1con",
    build_content,
    "Set",
    rec_set_button_event_cb,
    nullptr,
    COLOR_PRIMARY,
    0xFFFFFF,
};

} // namespace

extern "C" void func2rec2clicked2func(lv_event_t* e)
{
    corep4_ui_open_function_window(e, WINDOW_CONFIG);
}
