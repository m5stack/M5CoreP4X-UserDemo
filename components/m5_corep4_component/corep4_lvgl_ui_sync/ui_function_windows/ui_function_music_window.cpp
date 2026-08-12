#include "ui_function_widgets.h"

#include "audio_playback.h"
#include "audio_tools.h"
#include "audio_types.h"

#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdint>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_MUSIC";

constexpr uint32_t MUSIC_REFRESH_MS = 50;
constexpr uint32_t PREFERRED_TRACK_DURATION_MS = 45000;

constexpr float VOLUME_MIN  = 0.0f;
constexpr float VOLUME_MAX  = 100.0f;

constexpr uint32_t PLAYER_BG = 0xF8FAFC;
constexpr uint32_t PLAYER_PANEL = 0xFFFFFF;
constexpr uint32_t PLAYER_PANEL_2 = 0xEFF6FF;
constexpr uint32_t PLAYER_TEXT = 0x111827;
constexpr uint32_t PLAYER_MUTED = 0x64748B;
constexpr uint32_t PLAYER_LINE = 0xCBD5E1;
constexpr uint32_t PLAYER_ACCENT = 0x2563EB;
constexpr uint32_t PLAYER_ACCENT_2 = 0x0EA5E9;
constexpr uint32_t PLAYER_DANGER = 0xDC2626;
constexpr uint32_t PLAYER_READY = 0x16A34A;
constexpr uint32_t PLAYER_DISABLED = 0xE5E7EB;

constexpr int32_t SPECTRUM_W = 418;
constexpr int32_t SPECTRUM_PANEL_H = 230;
constexpr int32_t SPECTRUM_BAR_MAX_H = 122;
constexpr int32_t SPECTRUM_BAR_COUNT = 24;
constexpr int32_t FFT_SIZE = 128;
constexpr int32_t FFT_BINS = FFT_SIZE / 2;
constexpr size_t FFT_PCM_WINDOW_CAPACITY =
    FFT_SIZE * 2 * sizeof(int32_t);
constexpr float FFT_PI = 3.14159265358979323846f;

struct CandyWindTrack {
    audio_file_type_t type = AUDIO_FILE_CANDY_WIND_1CH_44K_16B_45S;
    uint32_t duration_ms = PREFERRED_TRACK_DURATION_MS;
    uint32_t sample_rate_hz = 44100;
    size_t channel_count = 1;
    const char* hint_text = "Mono 44.1 kHz 45 s";
    const char* format_text = "128 FFT  44.1 kHz";
};

struct MusicPageState {
    lv_obj_t* status_label = nullptr;
    lv_obj_t* track_hint_label = nullptr;
    lv_obj_t* elapsed_label = nullptr;
    lv_obj_t* duration_label = nullptr;
    lv_obj_t* format_label = nullptr;
    lv_obj_t* volume_label = nullptr;
    lv_obj_t* volume_slider = nullptr;
    lv_obj_t* progress_bar = nullptr;
    lv_obj_t* play_button = nullptr;
    lv_obj_t* spectrum_bars[SPECTRUM_BAR_COUNT] = {};
    lv_timer_t* timer = nullptr;

    volatile bool page_alive = false;
    volatile bool task_running = false;
    volatile bool play_started_known = false;
    volatile uint32_t play_started_ms = 0;
    volatile bool owns_playback = false;
    volatile bool track_available_known = false;
    volatile bool track_available = false;

    bool was_playing = false;
    bool stop_requested = false;
    bool paused_by_user = false;
    bool finished = false;
};

MusicPageState s_music_page;
float s_fft_re[FFT_SIZE] = {};
float s_fft_im[FFT_SIZE] = {};
float s_hann_window[FFT_SIZE] = {};
float s_spectrum_display_heights[SPECTRUM_BAR_COUNT] = {};
bool s_fft_window_ready = false;
bool s_spectrum_display_ready = false;

uint32_t now_ms()
{
    return static_cast<uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
}

bool fill_track_info(CandyWindTrack& track)
{
    track.type = AUDIO_FILE_CANDY_WIND_1CH_44K_16B_45S;
    track.duration_ms = 45000;
    track.sample_rate_hz = 44100;
    track.channel_count = 1;
    track.hint_text = "Mono 44.1 kHz 45 s";
    track.format_text = "128 FFT  44.1 kHz";

#if !defined(USE_AUDIO_CANDY_WIND_1CH_44K_16BIT_45S)
    return false;
#else
    return true;
#endif
}

bool get_candy_wind_track(const audio_playback* pb, CandyWindTrack& track)
{
    if (!fill_track_info(track)) {
        return false;
    }
    return !pb || pb->is_audio_file_available(AUDIO_FILE_CANDY_WIND_1CH_44K_16B_45S);
}

bool audio_playback_ready(const audio_tools& audio)
{
    return audio.is_system_initialized() &&
           audio.is_es8311_initialized() &&
           audio.get_play_device_handle() != nullptr;
}

uint32_t playback_elapsed_ms(const audio_playback* pb,
                             uint32_t duration_ms,
                             bool* progress_valid = nullptr,
                             uint32_t* actual_duration_ms = nullptr)
{
    if (progress_valid) {
        *progress_valid = false;
    }

    if (pb) {
        uint32_t audio_elapsed = 0;
        uint32_t audio_duration = 0;
        if (pb->get_playback_progress(audio_elapsed, audio_duration) && audio_duration > 0) {
            if (progress_valid) {
                *progress_valid = true;
            }
            if (actual_duration_ms) {
                *actual_duration_ms = audio_duration;
            }
            return audio_elapsed > audio_duration ? audio_duration : audio_elapsed;
        }
    }

    if (!s_music_page.play_started_known) {
        return 0;
    }

    uint32_t elapsed = now_ms() - s_music_page.play_started_ms;
    return elapsed > duration_ms ? duration_ms : elapsed;
}

void format_time(uint32_t ms, char* buf, size_t len)
{
    uint32_t seconds = ms / 1000;
    std::snprintf(buf, len, "%u:%02u",
                  static_cast<unsigned>(seconds / 60),
                  static_cast<unsigned>(seconds % 60));
}

void style_plain_obj(lv_obj_t* obj, uint32_t bg_color, int32_t radius)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
}

lv_obj_t* make_label(lv_obj_t* parent,
                     const char* text,
                     uint32_t color,
                     const lv_font_t* font,
                     int32_t width = lv_pct(100))
{
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    return label;
}

lv_obj_t* make_transparent_row(lv_obj_t* parent, int32_t height)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

lv_obj_t* make_player_button(lv_obj_t* parent,
                             const char* text,
                             uint32_t bg_color,
                             uint32_t text_color,
                             int32_t width,
                             int32_t height)
{
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(PLAYER_LINE), LV_PART_MAIN);
    lv_obj_set_style_radius(button, height / 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(button, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(button, lv_color_hex(0x0F172A), LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(button, 3, LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

void set_status_text(const char* text, uint32_t color)
{
    set_label_text(s_music_page.status_label, text);
    if (s_music_page.status_label) {
        lv_obj_set_style_text_color(s_music_page.status_label,
                                    lv_color_hex(color), LV_PART_MAIN);
    }
}

int16_t read_i16_le(const uint8_t* p)
{
    uint16_t value = static_cast<uint16_t>(p[0]) |
                     (static_cast<uint16_t>(p[1]) << 8);
    return static_cast<int16_t>(value);
}

void ensure_fft_window()
{
    if (s_fft_window_ready) {
        return;
    }

    for (int32_t i = 0; i < FFT_SIZE; ++i) {
        s_hann_window[i] =
            0.5f - 0.5f * std::cos((2.0f * FFT_PI * i) / (FFT_SIZE - 1));
    }
    s_fft_window_ready = true;
}

void fft_radix2(float* re, float* im, int32_t n)
{
    for (int32_t i = 1, j = 0; i < n; ++i) {
        int32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;

        if (i < j) {
            float tr = re[i];
            re[i] = re[j];
            re[j] = tr;

            float ti = im[i];
            im[i] = im[j];
            im[j] = ti;
        }
    }

    for (int32_t len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * FFT_PI / static_cast<float>(len);
        const float wlen_re = std::cos(angle);
        const float wlen_im = std::sin(angle);

        for (int32_t i = 0; i < n; i += len) {
            float w_re = 1.0f;
            float w_im = 0.0f;

            for (int32_t j = 0; j < len / 2; ++j) {
                const int32_t even = i + j;
                const int32_t odd = even + len / 2;
                const float odd_re = re[odd] * w_re - im[odd] * w_im;
                const float odd_im = re[odd] * w_im + im[odd] * w_re;

                re[odd] = re[even] - odd_re;
                im[odd] = im[even] - odd_im;
                re[even] += odd_re;
                im[even] += odd_im;

                const float next_w_re = w_re * wlen_re - w_im * wlen_im;
                w_im = w_re * wlen_im + w_im * wlen_re;
                w_re = next_w_re;
            }
        }
    }
}

void compute_fft_band_heights(const audio_playback* pb,
                              const CandyWindTrack& track,
                              uint32_t elapsed_ms,
                              int32_t* out_heights)
{
    constexpr int32_t band_edges_bin[SPECTRUM_BAR_COUNT + 1] = {
        1, 2, 3, 4, 5, 6, 7, 8,
        10, 12, 14, 16, 19, 22, 25, 29,
        33, 37, 42, 47, 52, 57, 60, 62, FFT_BINS
    };

    uint8_t pcm_window[FFT_PCM_WINDOW_CAPACITY] = {};
    size_t frames_read = 0;
    uint32_t sample_rate_hz = 0;
    audio_channels_t channels = AUDIO_CHANNELS_MONO;
    i2s_data_bit_width_t bits = I2S_DATA_BIT_WIDTH_16BIT;
    const size_t frame_pos =
        (static_cast<size_t>(elapsed_ms) * track.sample_rate_hz) / 1000U;
    const size_t start_frame =
        frame_pos > FFT_SIZE / 2 ? frame_pos - FFT_SIZE / 2 : 0;
    const esp_err_t read_ret =
        pb ? pb->read_audio_file_pcm_frames(track.type,
                                            start_frame,
                                            FFT_SIZE,
                                            pcm_window,
                                            sizeof(pcm_window),
                                            frames_read,
                                            sample_rate_hz,
                                            channels,
                                            bits)
           : ESP_ERR_INVALID_STATE;
    const size_t channel_count = static_cast<size_t>(channels);
    const size_t bytes_per_frame = channel_count * sizeof(int16_t);
    if (read_ret != ESP_OK || frames_read == 0 ||
        bits != I2S_DATA_BIT_WIDTH_16BIT ||
        channel_count == 0 || bytes_per_frame == 0) {
        for (int32_t i = 0; i < SPECTRUM_BAR_COUNT; ++i) {
            out_heights[i] = 18;
        }
        return;
    }

    ensure_fft_window();

    float mean = 0.0f;
    for (int32_t i = 0; i < FFT_SIZE; ++i) {
        const size_t idx = static_cast<size_t>(i);
        float sample = 0.0f;
        if (idx < frames_read) {
            const uint8_t* frame = pcm_window + idx * bytes_per_frame;
            int32_t mixed = 0;
            for (size_t ch = 0; ch < channel_count; ++ch) {
                mixed += read_i16_le(frame + ch * sizeof(int16_t));
            }
            sample = static_cast<float>(mixed) /
                     (32768.0f * static_cast<float>(channel_count));
        }
        s_fft_re[i] = sample;
        mean += sample;
    }

    mean /= static_cast<float>(FFT_SIZE);
    for (int32_t i = 0; i < FFT_SIZE; ++i) {
        s_fft_re[i] = (s_fft_re[i] - mean) * s_hann_window[i];
        s_fft_im[i] = 0.0f;
    }

    fft_radix2(s_fft_re, s_fft_im, FFT_SIZE);

    for (int32_t band = 0; band < SPECTRUM_BAR_COUNT; ++band) {
        int32_t start_bin = band_edges_bin[band];
        int32_t end_bin = band_edges_bin[band + 1];
        if (start_bin < 1) {
            start_bin = 1;
        }
        if (end_bin <= start_bin) {
            end_bin = start_bin + 1;
        }
        if (end_bin > FFT_BINS) {
            end_bin = FFT_BINS;
        }

        float energy = 0.0f;
        int32_t count = 0;
        for (int32_t bin = start_bin; bin < end_bin; ++bin) {
            const float mag_sq = s_fft_re[bin] * s_fft_re[bin] +
                                 s_fft_im[bin] * s_fft_im[bin];
            energy += mag_sq;
            ++count;
        }

        const float rms = std::sqrt(energy / static_cast<float>(count > 0 ? count : 1)) /
                          (FFT_SIZE * 0.5f);
        const float db = 20.0f * std::log10(rms + 0.000001f);
        float norm = (db + 68.0f) / 48.0f;
        if (norm < 0.0f) {
            norm = 0.0f;
        } else if (norm > 1.0f) {
            norm = 1.0f;
        }

        out_heights[band] =
            18 + static_cast<int32_t>(norm * static_cast<float>(SPECTRUM_BAR_MAX_H - 18));
    }
}

void update_spectrum_bars(const audio_playback* pb,
                          bool playing,
                          uint32_t elapsed_ms,
                          const CandyWindTrack* track)
{
    constexpr int32_t bar_w = 8;
    constexpr int32_t gap = 7;
    constexpr int32_t base_y = SPECTRUM_PANEL_H - 24;
    constexpr int32_t first_x =
        (SPECTRUM_W - (SPECTRUM_BAR_COUNT * bar_w + (SPECTRUM_BAR_COUNT - 1) * gap)) / 2;
    int32_t heights[SPECTRUM_BAR_COUNT] = {};
    if (playing && pb && track) {
        compute_fft_band_heights(pb, *track, elapsed_ms, heights);
    } else {
        for (int32_t i = 0; i < SPECTRUM_BAR_COUNT; ++i) {
            heights[i] = 18;
        }
    }

    for (int32_t i = 0; i < SPECTRUM_BAR_COUNT; ++i) {
        lv_obj_t* bar = s_music_page.spectrum_bars[i];
        if (!bar) {
            continue;
        }

        int32_t target_h = playing ? heights[i] : (18 + (heights[i] - 18) / 4);
        if (target_h > SPECTRUM_BAR_MAX_H) {
            target_h = SPECTRUM_BAR_MAX_H;
        } else if (target_h < 18) {
            target_h = 18;
        }

        if (!s_spectrum_display_ready) {
            s_spectrum_display_heights[i] = static_cast<float>(target_h);
        } else {
            const float current = s_spectrum_display_heights[i];
            const float coeff = target_h > current ? 0.55f : 0.22f;
            s_spectrum_display_heights[i] =
                current + (static_cast<float>(target_h) - current) * coeff;
        }
        int32_t h = static_cast<int32_t>(s_spectrum_display_heights[i] + 0.5f);

        lv_obj_set_size(bar, bar_w, h);
        lv_obj_set_pos(bar, first_x + i * (bar_w + gap), base_y - h);
        lv_obj_set_style_bg_color(bar,
                                  lv_color_hex((i % 4 == 1 || i % 4 == 2)
                                                   ? PLAYER_ACCENT_2
                                                   : PLAYER_ACCENT),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, playing ? LV_OPA_COVER : LV_OPA_60, LV_PART_MAIN);
    }
    s_spectrum_display_ready = true;
}

void set_controls_enabled(bool enabled)
{
    set_button_enabled(s_music_page.play_button, enabled);
    if (s_music_page.volume_slider) {
        if (enabled) {
            lv_obj_clear_state(s_music_page.volume_slider, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_music_page.volume_slider, LV_STATE_DISABLED);
        }
    }
}

void refresh_music_status()
{
    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        set_status_text("Audio tool not ready", PLAYER_DANGER);
        set_controls_enabled(false);
        update_spectrum_bars(nullptr, false, 0, nullptr);
        return;
    }

    auto& audio = tool->otool_tools.audio;
    auto* pb = audio.get_playback();
    const bool audio_ready = audio_playback_ready(audio);
    CandyWindTrack track;
    const bool configured = fill_track_info(track);
    const bool async_running = pb && pb->is_async_playback_running();
    const bool playing = async_running && s_music_page.owns_playback;
    const bool other_audio_running =
        async_running && !s_music_page.owns_playback;
    bool available = false;
    if (configured && pb) {
        if (s_music_page.owns_playback) {
            available = true;
        } else if (s_music_page.track_available_known) {
            available = s_music_page.track_available;
        } else if (!s_music_page.task_running) {
            available = pb->is_audio_file_available(track.type);
            s_music_page.track_available = available;
            s_music_page.track_available_known = true;
        }
    }

    if (s_music_page.track_hint_label) {
        set_label_text(s_music_page.track_hint_label, track.hint_text);
    }
    if (s_music_page.format_label) {
        set_label_text(s_music_page.format_label, track.format_text);
    }

    if (playing && !s_music_page.was_playing) {
        if (!s_music_page.play_started_known) {
            s_music_page.play_started_ms = now_ms();
            s_music_page.play_started_known = true;
        }
        s_music_page.stop_requested = false;
        s_music_page.paused_by_user = false;
        s_music_page.finished = false;
    }

    uint32_t duration_ms = track.duration_ms > 0 ? track.duration_ms
                                                 : PREFERRED_TRACK_DURATION_MS;
    bool audio_progress_valid = false;
    uint32_t audio_duration_ms = duration_ms;
    uint32_t elapsed = playback_elapsed_ms(pb, duration_ms,
                                           &audio_progress_valid,
                                           &audio_duration_ms);
    if (audio_progress_valid && audio_duration_ms > 0) {
        duration_ms = audio_duration_ms;
    }
    if (!playing && s_music_page.was_playing) {
        const bool natural_finish =
            !s_music_page.stop_requested &&
            (audio_progress_valid || s_music_page.play_started_known) &&
            elapsed + MUSIC_REFRESH_MS >= duration_ms;

        s_music_page.finished = natural_finish;
        s_music_page.paused_by_user = !natural_finish;
        s_music_page.play_started_known = natural_finish;
        if (natural_finish) {
            elapsed = duration_ms;
        } else {
            elapsed = 0;
        }
        s_music_page.stop_requested = false;
    }
    if (!async_running && s_music_page.owns_playback) {
        s_music_page.owns_playback = false;
    }

    if (!playing && !s_music_page.finished && !s_music_page.paused_by_user) {
        elapsed = 0;
    }

    char elapsed_buf[12];
    char duration_buf[12];
    format_time(elapsed, elapsed_buf, sizeof(elapsed_buf));
    format_time(duration_ms, duration_buf, sizeof(duration_buf));
    set_label_text(s_music_page.elapsed_label, elapsed_buf);
    set_label_text(s_music_page.duration_label, duration_buf);

    const int32_t progress =
        static_cast<int32_t>((elapsed * 1000U) / duration_ms);
    if (s_music_page.progress_bar) {
        lv_bar_set_value(s_music_page.progress_bar, progress, LV_ANIM_OFF);
    }

    char vol_buf[24];
    std::snprintf(vol_buf, sizeof(vol_buf), "VOL %d%%",
                  static_cast<int>(audio.get_volume()));
    set_label_text(s_music_page.volume_label, vol_buf);
    if (s_music_page.volume_slider &&
        !lv_obj_has_state(s_music_page.volume_slider, LV_STATE_EDITED) &&
        !lv_obj_has_state(s_music_page.volume_slider, LV_STATE_FOCUSED)) {
        lv_slider_set_value(s_music_page.volume_slider,
                            static_cast<int32_t>(audio.get_volume()), LV_ANIM_OFF);
    }

    if (!audio_ready) {
        set_status_text("Audio playback not ready", PLAYER_DANGER);
        set_controls_enabled(false);
        set_button_text(s_music_page.play_button, "PLAY");
        set_button_colors(s_music_page.play_button, PLAYER_DISABLED, PLAYER_MUTED);
    } else if (!configured) {
        set_status_text("CandyWind is not configured", PLAYER_DANGER);
        set_controls_enabled(false);
        set_button_text(s_music_page.play_button, "PLAY");
        set_button_colors(s_music_page.play_button, PLAYER_DISABLED, PLAYER_MUTED);
    } else {
        set_button_enabled(s_music_page.play_button, !s_music_page.task_running);
        if (s_music_page.volume_slider) {
            lv_obj_clear_state(s_music_page.volume_slider, LV_STATE_DISABLED);
        }

        if (s_music_page.task_running) {
            set_status_text("Preparing audio", PLAYER_ACCENT_2);
            set_button_text(s_music_page.play_button, "WAIT");
            set_button_colors(s_music_page.play_button, PLAYER_DISABLED, PLAYER_MUTED);
        } else if (!available) {
            set_status_text("Embedded track unavailable", PLAYER_DANGER);
            set_button_text(s_music_page.play_button, "RETRY");
            set_button_colors(s_music_page.play_button, PLAYER_ACCENT, PLAYER_PANEL);
        } else if (other_audio_running) {
            set_status_text("Other audio is playing", PLAYER_ACCENT_2);
            set_button_text(s_music_page.play_button, "PLAY");
            set_button_colors(s_music_page.play_button, PLAYER_ACCENT, PLAYER_PANEL);
        } else if (playing) {
            set_status_text("Playing", PLAYER_READY);
            set_button_text(s_music_page.play_button, "PAUSE");
            set_button_colors(s_music_page.play_button, PLAYER_DANGER, PLAYER_PANEL);
        } else if (s_music_page.finished) {
            set_status_text("Finished", PLAYER_ACCENT);
            set_button_text(s_music_page.play_button, "PLAY");
            set_button_colors(s_music_page.play_button, PLAYER_ACCENT, PLAYER_PANEL);
        } else if (s_music_page.paused_by_user) {
            set_status_text("Paused", PLAYER_ACCENT_2);
            set_button_text(s_music_page.play_button, "PLAY");
            set_button_colors(s_music_page.play_button, PLAYER_ACCENT, PLAYER_PANEL);
        } else {
            set_status_text("Ready", PLAYER_MUTED);
            set_button_text(s_music_page.play_button, "PLAY");
            set_button_colors(s_music_page.play_button, PLAYER_ACCENT, PLAYER_PANEL);
        }
    }

    update_spectrum_bars(pb, playing, elapsed, available ? &track : nullptr);
    s_music_page.was_playing = playing;
}

void update_music_ui_from_task(const char* text, uint32_t color, bool started)
{
    if (started) {
        s_music_page.play_started_ms = now_ms();
        s_music_page.play_started_known = true;
        s_music_page.stop_requested = false;
        s_music_page.paused_by_user = false;
        s_music_page.finished = false;
    }

    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock LVGL for music update");
        return;
    }
    if (s_music_page.page_alive) {
        set_status_text(text, color);
        refresh_music_status();
    }
    lvgl_port_unlock();
}

void play_task(void* /*param*/)
{
    auto* tool = corep4_ui_sync_get_tool();
    char msg[96];
    uint32_t color = PLAYER_DANGER;
    bool started = false;

    if (!tool || !audio_playback_ready(tool->otool_tools.audio)) {
        std::snprintf(msg, sizeof(msg), "Audio not ready");
        color = PLAYER_DANGER;
    } else {
        auto& audio = tool->otool_tools.audio;
        auto* pb = audio.get_playback();
        CandyWindTrack track;
        const bool track_available =
            pb && get_candy_wind_track(pb, track);
        s_music_page.track_available = track_available;
        s_music_page.track_available_known = true;
        if (!track_available) {
            std::snprintf(msg, sizeof(msg), "CandyWind not available");
            color = PLAYER_DANGER;
        } else {
            esp_err_t ret = ESP_OK;
            if (pb->is_async_playback_running()) {
                ESP_LOGI(TAG, "Stopping existing audio before Music playback");
                ret = pb->stop_async_playback();
                s_music_page.owns_playback = false;
            }
            if (ret == ESP_OK) {
                ret = audio.set_pa_power(true);
            }
            if (ret == ESP_OK) {
                ret = pb->play_audio_file(track.type, AUDIO_PLAYBACK_ASYNC);
                started = ret == ESP_OK;
                s_music_page.owns_playback = started;
            }

            if (!started) {
                s_music_page.owns_playback = false;
                std::snprintf(msg, sizeof(msg), "Play failed: %s",
                              esp_err_to_name(ret));
                color = PLAYER_DANGER;
            } else {
                ESP_LOGI(TAG, "Music playback started: type=%d", track.type);
                std::snprintf(msg, sizeof(msg), "Playing");
                color = PLAYER_READY;
            }
        }
    }

    s_music_page.task_running = false;
    update_music_ui_from_task(msg, color, started);
    vTaskDelete(nullptr);
}

void music_timer_cb(lv_timer_t* /*timer*/)
{
    if (!s_music_page.page_alive) {
        return;
    }
    refresh_music_status();
}

void play_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_music_page.task_running) {
        return;
    }

    auto* tool = corep4_ui_sync_get_tool();
    auto* pb = tool ? tool->otool_tools.audio.get_playback() : nullptr;
    if (pb && pb->is_async_playback_running() &&
        s_music_page.owns_playback) {
        s_music_page.stop_requested = true;
        s_music_page.finished = false;
        set_status_text("Pausing", PLAYER_ACCENT_2);
        set_button_enabled(s_music_page.play_button, false);
        const esp_err_t stop_ret = pb->stop_async_playback();
        if (stop_ret == ESP_OK) {
            s_music_page.owns_playback = false;
            s_music_page.play_started_known = false;
            s_music_page.paused_by_user = true;
            s_music_page.was_playing = false;
        } else {
            s_music_page.stop_requested = false;
            set_status_text("Pause failed", PLAYER_DANGER);
        }
        refresh_music_status();
        return;
    }

    s_music_page.task_running = true;
    s_music_page.play_started_known = false;
    s_music_page.paused_by_user = false;
    s_music_page.finished = false;
    set_status_text("Preparing audio", PLAYER_ACCENT_2);
    set_button_enabled(s_music_page.play_button, false);

    BaseType_t ret = xTaskCreate(play_task, "func_music_play", 4096,
                                 nullptr, configMAX_PRIORITIES - 4, nullptr);
    if (ret != pdPASS) {
        s_music_page.task_running = false;
        set_status_text("Cannot create play task", PLAYER_DANGER);
        refresh_music_status();
    }
}

void volume_slider_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        return;
    }

    lv_obj_t* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    float vol = static_cast<float>(lv_slider_get_value(slider));
    if (vol < VOLUME_MIN) {
        vol = VOLUME_MIN;
    }
    if (vol > VOLUME_MAX) {
        vol = VOLUME_MAX;
    }

    tool->otool_tools.audio.set_volume(vol);
    refresh_music_status();
}

void music_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    if (s_music_page.timer) {
        lv_timer_delete(s_music_page.timer);
        s_music_page.timer = nullptr;
    }

    s_music_page.page_alive = false;
    s_music_page.owns_playback = false;
    s_music_page.track_available_known = false;
    s_music_page.track_available = false;
    s_music_page.status_label = nullptr;
    s_music_page.track_hint_label = nullptr;
    s_music_page.elapsed_label = nullptr;
    s_music_page.duration_label = nullptr;
    s_music_page.format_label = nullptr;
    s_music_page.volume_label = nullptr;
    s_music_page.volume_slider = nullptr;
    s_music_page.progress_bar = nullptr;
    s_music_page.play_button = nullptr;
    s_spectrum_display_ready = false;
    for (int32_t i = 0; i < SPECTRUM_BAR_COUNT; ++i) {
        s_music_page.spectrum_bars[i] = nullptr;
        s_spectrum_display_heights[i] = 18.0f;
    }
}

void build_spectrum(lv_obj_t* parent)
{
    lv_obj_t* panel = lv_obj_create(parent);
    style_plain_obj(panel, PLAYER_PANEL_2, 22);
    lv_obj_set_size(panel, SPECTRUM_W, SPECTRUM_PANEL_H);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(0xBFDBFE), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(panel, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(PLAYER_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(panel, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "CandyWind");
    lv_obj_set_style_text_color(title, lv_color_hex(PLAYER_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 18, 14);

    s_music_page.track_hint_label = lv_label_create(panel);
    lv_label_set_text(s_music_page.track_hint_label, "Mono 44.1 kHz 45 s");
    lv_obj_set_style_text_color(s_music_page.track_hint_label,
                                lv_color_hex(PLAYER_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_music_page.track_hint_label,
                               &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(s_music_page.track_hint_label, LV_ALIGN_TOP_LEFT, 20, 48);

    for (int32_t i = 0; i < SPECTRUM_BAR_COUNT; ++i) {
        lv_obj_t* bar = lv_obj_create(panel);
        style_plain_obj(bar, PLAYER_ACCENT, 4);
        s_music_page.spectrum_bars[i] = bar;
    }
}

void build_content(lv_obj_t* parent, const CoreP4FunctionWindowConfig& /*config*/)
{
    prepare_content(parent);
    s_music_page.page_alive = true;
    s_music_page.was_playing = false;
    s_music_page.stop_requested = false;
    s_music_page.paused_by_user = false;
    s_music_page.finished = false;
    s_music_page.play_started_known = false;
    s_music_page.owns_playback = false;
    s_music_page.track_available_known = false;
    s_music_page.track_available = false;
    s_music_page.status_label = nullptr;
    s_music_page.track_hint_label = nullptr;
    s_music_page.elapsed_label = nullptr;
    s_music_page.duration_label = nullptr;
    s_music_page.format_label = nullptr;
    s_music_page.volume_label = nullptr;
    s_music_page.volume_slider = nullptr;
    s_music_page.progress_bar = nullptr;
    s_music_page.play_button = nullptr;
    for (int32_t i = 0; i < SPECTRUM_BAR_COUNT; ++i) {
        s_music_page.spectrum_bars[i] = nullptr;
    }

    lv_obj_add_event_cb(parent, music_content_delete_event_cb, LV_EVENT_DELETE, nullptr);
    lv_obj_set_style_bg_color(parent, lv_color_hex(PLAYER_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(parent, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(parent, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    build_spectrum(parent);

    s_music_page.progress_bar = lv_bar_create(parent);
    lv_obj_set_size(s_music_page.progress_bar, lv_pct(100), 8);
    lv_bar_set_range(s_music_page.progress_bar, 0, 1000);
    lv_bar_set_value(s_music_page.progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_music_page.progress_bar,
                              lv_color_hex(PLAYER_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_music_page.progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_music_page.progress_bar,
                              lv_color_hex(PLAYER_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_music_page.progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_music_page.progress_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(s_music_page.progress_bar, 5, LV_PART_INDICATOR);

    lv_obj_t* time_row = make_transparent_row(parent, 20);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_music_page.elapsed_label =
        make_label(time_row, "0:00", PLAYER_MUTED, &lv_font_montserrat_16, 80);
    lv_obj_set_style_text_align(s_music_page.elapsed_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    s_music_page.duration_label =
        make_label(time_row, "0:45", PLAYER_MUTED, &lv_font_montserrat_16, 80);
    lv_obj_set_style_text_align(s_music_page.duration_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    lv_obj_t* info_row = make_transparent_row(parent, 30);
    lv_obj_set_flex_align(info_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_music_page.status_label =
        make_label(info_row, "Ready", PLAYER_MUTED, &lv_font_montserrat_16, 160);
    lv_obj_set_style_text_align(s_music_page.status_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    s_music_page.format_label =
        make_label(info_row, "128 FFT  44.1 kHz", PLAYER_MUTED, &lv_font_montserrat_16, 190);
    lv_obj_set_style_text_align(s_music_page.format_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    lv_obj_t* control_row = make_transparent_row(parent, 82);
    lv_obj_set_flex_align(control_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(control_row, 12, LV_PART_MAIN);
    s_music_page.play_button =
        make_player_button(control_row, "PLAY", PLAYER_ACCENT, PLAYER_PANEL, 184, 60);
    lv_obj_add_event_cb(s_music_page.play_button, play_button_event_cb,
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t* volume_box = lv_obj_create(control_row);
    style_plain_obj(volume_box, PLAYER_PANEL, 14);
    lv_obj_set_size(volume_box, 228, 68);
    lv_obj_set_style_border_width(volume_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(volume_box, lv_color_hex(PLAYER_LINE), LV_PART_MAIN);
    lv_obj_set_style_pad_top(volume_box, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(volume_box, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_left(volume_box, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_right(volume_box, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_row(volume_box, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(volume_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(volume_box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    s_music_page.volume_label =
        make_label(volume_box, "VOL --%", PLAYER_MUTED, &lv_font_montserrat_16);
    lv_obj_set_style_text_align(s_music_page.volume_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);

    s_music_page.volume_slider = lv_slider_create(volume_box);
    lv_obj_set_size(s_music_page.volume_slider, 176, 18);
    lv_slider_set_range(s_music_page.volume_slider, 0, 100);
    lv_obj_set_style_bg_color(s_music_page.volume_slider,
                              lv_color_hex(PLAYER_DISABLED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_music_page.volume_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_music_page.volume_slider, 9, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_music_page.volume_slider,
                              lv_color_hex(PLAYER_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_music_page.volume_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_music_page.volume_slider, 9, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_music_page.volume_slider,
                              lv_color_hex(PLAYER_PANEL), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_music_page.volume_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(s_music_page.volume_slider, 14, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_music_page.volume_slider, 6, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_music_page.volume_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_music_page.volume_slider,
                                  lv_color_hex(PLAYER_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(s_music_page.volume_slider, volume_slider_event_cb,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    refresh_music_status();
    s_music_page.timer = lv_timer_create(music_timer_cb, MUSIC_REFRESH_MS, nullptr);
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "Music",
    "click on ui_func1music1con",
    build_content,
    nullptr,
    nullptr,
};

} // namespace

extern "C" void func2music2clicked2func(lv_event_t* e)
{
    corep4_ui_open_function_window(e, WINDOW_CONFIG);
}
