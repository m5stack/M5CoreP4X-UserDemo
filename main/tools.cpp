#include "tools.h"
#include "audio_tools.h"

#if __has_include("app_fw_version.h")
#include "app_fw_version.h"
#endif

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifndef APP_FW_NAME
#define APP_FW_NAME "M5CoreP4X-UserDemo"
#endif
#ifndef APP_FW_VERSION
#define APP_FW_VERSION "V0.2-20260812"
#endif
#ifndef APP_FW_POWERED_BY
#define APP_FW_POWERED_BY "Powered by onexs."
#endif
#ifndef APP_FW_SUPPORT_TEXT
#define APP_FW_SUPPORT_TEXT "Supported by M5Stack Technology Co., Ltd"
#endif

static const char *TAG = "tools";

namespace {

int compile_month_to_number(const char *mon)
{
    if (!mon) return 1;
    if (std::strcmp(mon, "Jan") == 0) return 1;
    if (std::strcmp(mon, "Feb") == 0) return 2;
    if (std::strcmp(mon, "Mar") == 0) return 3;
    if (std::strcmp(mon, "Apr") == 0) return 4;
    if (std::strcmp(mon, "May") == 0) return 5;
    if (std::strcmp(mon, "Jun") == 0) return 6;
    if (std::strcmp(mon, "Jul") == 0) return 7;
    if (std::strcmp(mon, "Aug") == 0) return 8;
    if (std::strcmp(mon, "Sep") == 0) return 9;
    if (std::strcmp(mon, "Oct") == 0) return 10;
    if (std::strcmp(mon, "Nov") == 0) return 11;
    if (std::strcmp(mon, "Dec") == 0) return 12;
    return 1;
}

bool firmware_compile_date_midnight(struct tm &out)
{
    char mon[4] = {};
    int day     = 1;
    int year    = 2025;
    if (std::sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) {
        return false;
    }

    out          = {};
    out.tm_year  = year - 1900;
    out.tm_mon   = compile_month_to_number(mon) - 1;
    out.tm_mday  = day;
    out.tm_hour  = 0;
    out.tm_min   = 0;
    out.tm_sec   = 0;
    out.tm_isdst = -1;

    struct tm copy = out;
    if (mktime(&copy) != static_cast<time_t>(-1)) {
        out.tm_wday = copy.tm_wday;
    }
    return true;
}

bool rtc_time_needs_restore(const struct tm &t)
{
    const int year = t.tm_year + 1900;
    if (year == 2099 || year < 2020 || year > 2099) {
        return true;
    }
    if (t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 || t.tm_mday > 31) {
        return true;
    }
    if (t.tm_hour < 0 || t.tm_hour > 23 || t.tm_min < 0 || t.tm_min > 59 || t.tm_sec < 0 || t.tm_sec > 59) {
        return true;
    }
    return false;
}

void ethernet_wait_task(void *arg)
{
    auto *tool = static_cast<m5_corep4_tool *>(arg);
    if (tool == nullptr) {
        ESP_LOGE(TAG, "Ethernet wait task got null tool");
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Ethernet wait task started; waiting until cable is connected and IP is ready");
    esp_err_t conn_ret = tool->wait_for_ethernet_connection(0);
    if (conn_ret == ESP_OK) {
        ESP_LOGI(TAG, "Ethernet connected successfully; wait task exiting");
    } else {
        ESP_LOGW(TAG, "Ethernet wait task exited: %s", esp_err_to_name(conn_ret));
    }

    vTaskDelete(nullptr);
}

struct StartupPromptSoundTaskContext {
    m5_corep4_tool *tool;
    uint32_t delay_ms;
};

void play_startup_prompt_sound_now(m5_corep4_tool &tool)
{
    if (!tool.get_startup_prompt_audio_enabled()) {
        ESP_LOGI(TAG, "Startup prompt sound disabled");
        return;
    }

    auto *playback = tool.otool_tools.audio.get_playback();
    if (!playback) {
        ESP_LOGW(TAG, "Startup prompt sound skipped: playback is not ready");
        return;
    }

    if (!playback->is_audio_file_available(AUDIO_FILE_STARTUP_2CH_16K_16B_4S)) {
        ESP_LOGW(TAG, "Startup prompt sound skipped: startup PCM is not embedded");
        return;
    }

    esp_err_t ret = playback->play_audio_file(AUDIO_FILE_STARTUP_2CH_16K_16B_4S, AUDIO_PLAYBACK_ASYNC, 0);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Startup prompt sound playback started");
    } else {
        ESP_LOGW(TAG, "Startup prompt sound playback failed: %s", esp_err_to_name(ret));
    }
}

void startup_prompt_sound_task(void *arg)
{
    auto *ctx = static_cast<StartupPromptSoundTaskContext *>(arg);
    if (!ctx || !ctx->tool) {
        std::free(ctx);
        vTaskDelete(nullptr);
        return;
    }

    m5_corep4_tool *tool = ctx->tool;
    uint32_t delay_ms    = ctx->delay_ms;
    std::free(ctx);

    if (delay_ms > 0) {
        ESP_LOGI(TAG, "Startup prompt sound delayed by %lu ms", static_cast<unsigned long>(delay_ms));
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    play_startup_prompt_sound_now(*tool);
    vTaskDelete(nullptr);
}

/**
 * @brief USB Serial JTAG 写入回调函数
 *
 * @param data 要写入的数据缓冲区
 * @param length 数据长度
 * @param timeout_ms 超时时间（毫秒）
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t usb_serial_jtag_write_callback(const uint8_t *data, size_t length, uint32_t timeout_ms)
{
    if (data == nullptr || length == 0) {
        return ESP_OK;
    }

    uint32_t written       = 0;
    uint32_t start_tick    = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (written < length) {
        int result = usb_serial_jtag_write_bytes((const char *)(data + written), length - written, pdMS_TO_TICKS(100));
        if (result > 0) {
            written += result;
        }

        // 检查超时
        if ((xTaskGetTickCount() - start_tick) > timeout_ticks) {
            ESP_LOGW(TAG, "USB-JTAG write timeout: written %lu/%zu bytes", written, length);
            return ESP_ERR_TIMEOUT;
        }

        // 避免忙等待
        if (result <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    return ESP_OK;
}

/**
 * @brief USB Serial JTAG 读取回调函数
 *
 * @param data 接收数据的缓冲区
 * @param length 要读取的数据长度
 * @param timeout_ms 超时时间（毫秒）
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t usb_serial_jtag_read_callback(uint8_t *data, size_t length, uint32_t timeout_ms)
{
    if (data == nullptr || length == 0) {
        return ESP_OK;
    }

    uint32_t read_count    = 0;
    uint32_t start_tick    = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    uint32_t no_data_count = 0;  // 连续无数据计数

    while (read_count < length) {
        int result = usb_serial_jtag_read_bytes((char *)(data + read_count), length - read_count, pdMS_TO_TICKS(10));
        if (result > 0) {
            read_count += result;
            start_tick    = xTaskGetTickCount();  // 读到数据后重置超时起点
            no_data_count = 0;                    // 重置无数据计数
        } else {
            no_data_count++;
        }

        // 检查总超时
        if ((xTaskGetTickCount() - start_tick) > timeout_ticks) {
            // 完全没读到数据是正常的，不输出日志（避免刷屏）
            // 只在读到部分数据但不完整时才警告
            if (read_count > 0) {
                ESP_LOGW(TAG, "USB-JTAG read incomplete: got %lu/%zu bytes", read_count, length);
            }
            return ESP_ERR_TIMEOUT;
        }

        // 避免忙等待：连续多次无数据时增加延迟
        if (result <= 0) {
            if (no_data_count > 5) {
                vTaskDelay(pdMS_TO_TICKS(20));  // 长时间无数据，增加延迟减少CPU占用
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
    }

    return ESP_OK;
}

/**
 * @brief USB Serial JTAG 可用数据量查询回调函数（可选）
 *
 * @return size_t 可用字节数
 */
size_t usb_serial_jtag_available_callback()
{
    // USB Serial JTAG 不提供直接的 available() 函数
    // 可以通过尝试读取来判断，这里返回 0 表示不支持
    return 0;
}

}  // anonymous namespace

void log_firmware_metadata(const char *tag)
{
    const char *log_tag = tag ? tag : TAG;
    ESP_LOGI(log_tag, "Firmware: %s", APP_FW_NAME);
    ESP_LOGI(log_tag, "Version: %s", APP_FW_VERSION);
    ESP_LOGI(log_tag, "%s", APP_FW_POWERED_BY);
    ESP_LOGI(log_tag, "%s", APP_FW_SUPPORT_TEXT);
}

void repair_rtc_time_if_needed(m5_corep4_tool &tool)
{
    struct tm current = {};
    tool.rtc.getTime(&current);

    if (rtc_time_needs_restore(current)) {
        struct tm restored = {};
        if (firmware_compile_date_midnight(restored)) {
            ESP_LOGW(TAG,
                     "RTC time is unreasonable (%04d-%02d-%02d %02d:%02d:%02d), "
                     "restoring to firmware date %04d-%02d-%02d 00:00:00",
                     current.tm_year + 1900, current.tm_mon + 1, current.tm_mday, current.tm_hour, current.tm_min,
                     current.tm_sec, restored.tm_year + 1900, restored.tm_mon + 1, restored.tm_mday);
            tool.rtc.setTime(&restored);
            current = restored;
        } else {
            ESP_LOGW(TAG, "RTC time is unreasonable, but firmware compile date parse failed");
        }
    }

    if (auto *status = tool.get_device_status()) {
        status->rtc.rtc_initialized   = true;
        status->rtc.timer_irq_enabled = false;
        status->rtc.current_time      = current;
    }
}

void start_ethernet_wait_task(m5_corep4_tool &tool)
{
    BaseType_t task_ret = xTaskCreate(ethernet_wait_task, "eth_wait", 4096, &tool, 5, nullptr);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Ethernet wait task");
    }
}

void play_startup_prompt_sound(m5_corep4_tool &tool, uint32_t delay_ms)
{
    if (!tool.get_startup_prompt_audio_enabled()) {
        ESP_LOGI(TAG, "Startup prompt sound disabled");
        return;
    }

    if (delay_ms == 0) {
        play_startup_prompt_sound_now(tool);
        return;
    }

    auto *ctx = static_cast<StartupPromptSoundTaskContext *>(std::malloc(sizeof(StartupPromptSoundTaskContext)));
    if (!ctx) {
        ESP_LOGW(TAG, "Startup prompt sound skipped: no memory for delay task");
        return;
    }

    ctx->tool     = &tool;
    ctx->delay_ms = delay_ms;

    BaseType_t task_ret = xTaskCreate(startup_prompt_sound_task, "startup_snd", 3072, ctx, 5, nullptr);
    if (task_ret != pdPASS) {
        std::free(ctx);
        ESP_LOGW(TAG, "Startup prompt sound skipped: failed to create delay task");
    }
}

esp_err_t setup_context_bus_with_usb_serial_jtag(m5::py32bin::ContextBusAdapter &adapter)
{
    ESP_LOGI(TAG, "Setting up ContextBusAdapter with USB Serial JTAG (CDC)...");

    // 初始化 USB Serial JTAG 驱动（增大缓冲区以容纳大固件）
    usb_serial_jtag_driver_config_t usb_serial_config = {
        .tx_buffer_size = 4096,   // 发送缓冲区 4KB
        .rx_buffer_size = 32768,  // 接收缓冲区 32KB（足够容纳 20KB 固件 + MD5）
    };

    esp_err_t ret = usb_serial_jtag_driver_install(&usb_serial_config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install USB Serial JTAG driver: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "USB Serial JTAG driver already installed");
    } else {
        ESP_LOGI(TAG, "USB Serial JTAG driver installed successfully");
    }

    // 配置 ContextBusAdapter
    m5::py32bin::Py32ContextBusConfig bridge_config;
    bridge_config.write_func         = usb_serial_jtag_write_callback;
    bridge_config.read_func          = usb_serial_jtag_read_callback;
    bridge_config.available_func     = usb_serial_jtag_available_callback;
    bridge_config.default_timeout_ms = 1000;

    ret = adapter.configure(bridge_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ContextBusAdapter: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = adapter.begin();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin ContextBusAdapter: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "ContextBusAdapter with USB Serial JTAG setup completed successfully");
    ESP_LOGI(TAG, "Ready to communicate via USB CDC");

    return ESP_OK;
}

// ============================================================================
// 屏幕显示及触摸综合测试
// 功能：
//   1. 循环显示纯色画面：白框黑屏、红、绿、蓝、白、黑
//   2. 任意画面手指上滑提高亮度，下滑降低亮度
//   3. 任意画面2秒内在屏幕中央240x240区域连续点击5次进入触摸测试
//   4. 触摸测试：5横5纵网格线 + 随机彩色像素轨迹（不清除历史）
// ============================================================================

// ---------- 辅助：带重试的draw_bitmap（等待上一次DMA传输完成） ----------
static esp_err_t lcd_draw_bitmap_wait(esp_lcd_panel_handle_t panel, int x0, int y0, int x1, int y1, const void *data)
{
    esp_err_t ret;
    for (int retry = 0; retry < 200; retry++) {
        ret = esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, data);
        if (ret == ESP_OK) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(1));  // 等待1ms后重试
    }
    ESP_LOGE(TAG, "lcd_draw_bitmap_wait timeout at (%d,%d)-(%d,%d)", x0, y0, x1, y1);
    return ret;
}

// ---------- 辅助：用RGB888填充整屏 ----------
static void fill_screen_rgb888(esp_lcd_panel_handle_t panel, uint8_t r, uint8_t g, uint8_t b)
{
    const uint16_t H = 480, V = 480;
    // 逐行绘制以减少内存使用
    const uint16_t ROWS = 20;  // 每次20行
    size_t buf_bytes    = H * ROWS * 3;
    uint8_t *buf        = (uint8_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!buf) return;

    for (int i = 0; i < (int)(H * ROWS); i++) {
        buf[i * 3 + 0] = r;
        buf[i * 3 + 1] = g;
        buf[i * 3 + 2] = b;
    }

    for (uint16_t y = 0; y < V; y += ROWS) {
        uint16_t h = (y + ROWS <= V) ? ROWS : (V - y);
        lcd_draw_bitmap_wait(panel, 0, y, H, y + h, buf);
    }
    free(buf);
}

// ---------- 辅助：绘制白框黑底画面 ----------
static void draw_white_border_black(esp_lcd_panel_handle_t panel)
{
    const uint16_t H = 480, V = 480;
    const uint16_t BORDER = 4;  // 边框宽度
    // 先填黑
    fill_screen_rgb888(panel, 0, 0, 0);

    // 上边框
    size_t row_bytes = H * 3;
    uint8_t *row     = (uint8_t *)heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM);
    if (!row) return;
    memset(row, 0xFF, row_bytes);  // 白色
    for (uint16_t y = 0; y < BORDER; y++) {
        lcd_draw_bitmap_wait(panel, 0, y, H, y + 1, row);
    }
    // 下边框
    for (uint16_t y = V - BORDER; y < V; y++) {
        lcd_draw_bitmap_wait(panel, 0, y, H, y + 1, row);
    }
    free(row);

    // 左右边框（逐列绘制，使用窄缓冲区）
    size_t col_bytes = V * 3;
    uint8_t *col     = (uint8_t *)heap_caps_malloc(col_bytes, MALLOC_CAP_SPIRAM);
    if (!col) return;
    memset(col, 0xFF, col_bytes);  // 白色
    // 左边框
    for (uint16_t x = 0; x < BORDER; x++) {
        lcd_draw_bitmap_wait(panel, x, 0, x + 1, V, col);
    }
    // 右边框
    for (uint16_t x = H - BORDER; x < H; x++) {
        lcd_draw_bitmap_wait(panel, x, 0, x + 1, V, col);
    }
    free(col);
}

// ---------- 辅助：绘制触摸测试网格（5横5纵，与边框等距） ----------
static void draw_touch_grid(esp_lcd_panel_handle_t panel)
{
    const uint16_t H = 480, V = 480;
    const uint16_t GRID = 6;  // 6等份 = 5条线

    // 先清黑屏
    fill_screen_rgb888(panel, 0, 0, 0);

    uint8_t line_r = 80, line_g = 80, line_b = 80;  // 灰色网格线

    // 横线缓冲区（1行 x H像素）
    size_t row_bytes = H * 3;
    uint8_t *row_buf = (uint8_t *)heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM);
    if (row_buf) {
        for (int i = 0; i < H; i++) {
            row_buf[i * 3 + 0] = line_r;
            row_buf[i * 3 + 1] = line_g;
            row_buf[i * 3 + 2] = line_b;
        }
        for (int i = 1; i < (int)GRID; i++) {
            uint16_t y = (V * i) / GRID;
            lcd_draw_bitmap_wait(panel, 0, y, H, y + 1, row_buf);
        }
        free(row_buf);
    }

    // 竖线缓冲区（V像素 x 1列）
    size_t col_bytes = V * 3;
    uint8_t *col_buf = (uint8_t *)heap_caps_malloc(col_bytes, MALLOC_CAP_SPIRAM);
    if (col_buf) {
        for (int i = 0; i < V; i++) {
            col_buf[i * 3 + 0] = line_r;
            col_buf[i * 3 + 1] = line_g;
            col_buf[i * 3 + 2] = line_b;
        }
        for (int i = 1; i < (int)GRID; i++) {
            uint16_t x = (H * i) / GRID;
            lcd_draw_bitmap_wait(panel, x, 0, x + 1, V, col_buf);
        }
        free(col_buf);
    }
}

// ---------- 触摸测试主循环（随机彩色轨迹） ----------
static void touch_draw_loop(esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t tp)
{
    ESP_LOGI(TAG, "Touch draw mode: touch to draw random color pixels.");
    corep4.set_lcd_brightness(100);  // 触摸测试始终最高亮度

    // 预定义鲜艳颜色(RGB888)
    struct C3 {
        uint8_t r, g, b;
    };
    const C3 palette[] = {{255, 0, 0},   {0, 255, 0},     {0, 0, 255},   {255, 255, 0}, {255, 0, 255}, {0, 255, 255},
                          {255, 128, 0}, {255, 255, 255}, {128, 255, 0}, {0, 128, 255}, {255, 0, 128}, {128, 0, 255}};
    const int palette_size = sizeof(palette) / sizeof(palette[0]);

    // 3x3像素缓冲区（更易看清轨迹）
    const int DOT_SIZE = 3;
    uint8_t dot_buf[DOT_SIZE * DOT_SIZE * 3];

    while (true) {
        uint16_t tx[5], ty[5], ts[5];
        uint8_t cnt = 0;

        if (esp_lcd_touch_read_data(tp) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        bool touched = esp_lcd_touch_get_coordinates(tp, tx, ty, ts, &cnt, 5);

        if (touched && cnt > 0) {
            // 绘制每个触摸点
            for (int i = 0; i < cnt && i < 5; i++) {
                int16_t x = tx[i], y = ty[i];
                if (x < 0 || x >= 480 || y < 0 || y >= 480) continue;

                // 随机颜色
                const C3 &c = palette[esp_random() % palette_size];
                for (int p = 0; p < DOT_SIZE * DOT_SIZE; p++) {
                    dot_buf[p * 3 + 0] = c.r;
                    dot_buf[p * 3 + 1] = c.g;
                    dot_buf[p * 3 + 2] = c.b;
                }

                int16_t x0 = x - DOT_SIZE / 2;
                int16_t y0 = y - DOT_SIZE / 2;
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                int16_t x1 = x0 + DOT_SIZE;
                int16_t y1 = y0 + DOT_SIZE;
                if (x1 > 480) {
                    x1 = 480;
                    x0 = 480 - DOT_SIZE;
                }
                if (y1 > 480) {
                    y1 = 480;
                    y0 = 480 - DOT_SIZE;
                }

                lcd_draw_bitmap_wait(panel, x0, y0, x1, y1, dot_buf);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ---------- 屏幕显示及触摸综合测试入口 ----------
void lcd_and_touch_test(void)
{
    esp_lcd_panel_handle_t panel = corep4.get_lcd_panel_handle();
    esp_lcd_touch_handle_t tp    = corep4.get_touch_handle();
    if (!panel || !tp) {
        ESP_LOGE(TAG, "LCD or Touch not initialized!");
        return;
    }

    ESP_LOGI(TAG, "=== LCD & Touch Test Start ===");
    ESP_LOGI(TAG, "Swipe up/down to adjust brightness");
    ESP_LOGI(TAG, "Tap 5 times in center (240x240) within 2s to enter touch draw mode");

    // 定义颜色页面：白框黑屏、红、绿、蓝、白、黑
    struct ColorPage {
        uint8_t r, g, b;
        bool has_border;  // 是否为白框黑屏
        const char *name;
    };
    const ColorPage pages[] = {
        {0, 0, 0, true, "WhiteBorder+Black"}, {255, 0, 0, false, "Red"},       {0, 255, 0, false, "Green"},
        {0, 0, 255, false, "Blue"},           {255, 255, 255, false, "White"}, {0, 0, 0, false, "Black"},
    };
    const int num_pages = sizeof(pages) / sizeof(pages[0]);
    int current_page    = 0;

    // 亮度
    int brightness = 100;

    // 绘制当前页面
    auto draw_page = [&]() {
        if (pages[current_page].has_border) {
            draw_white_border_black(panel);
        } else {
            fill_screen_rgb888(panel, pages[current_page].r, pages[current_page].g, pages[current_page].b);
        }
        ESP_LOGI(TAG, "Page %d/%d: %s", current_page + 1, num_pages, pages[current_page].name);
    };
    draw_page();

    // 5次点击检测状态
    int tap_count               = 0;
    int64_t tap_start_time      = 0;        // us
    const int64_t TAP_WINDOW_US = 2000000;  // 2秒
    const int16_t CENTER        = 240;
    const int16_t HALF_SIDE     = 120;  // 240/2

    // 滑动检测
    int16_t swipe_start_x  = -1;
    int16_t swipe_start_y  = -1;
    bool swiping           = false;
    bool swipe_h_triggered = false;  // 水平滑动已触发（切换页面）
    bool swipe_v_triggered = false;  // 垂直滑动已触发（亮度调节）

    // 点击检测：手指抬起才算一次tap
    bool was_touching    = false;
    int16_t touch_down_x = -1;
    int16_t touch_down_y = -1;

    while (true) {
        uint16_t tx[5], ty[5], ts[5];
        uint8_t cnt = 0;

        if (esp_lcd_touch_read_data(tp) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        bool touched = esp_lcd_touch_get_coordinates(tp, tx, ty, ts, &cnt, 5);

        if (touched && cnt > 0) {
            int16_t x = tx[0], y = ty[0];

            if (!was_touching) {
                // 手指刚按下
                touch_down_x      = x;
                touch_down_y      = y;
                swipe_start_x     = x;
                swipe_start_y     = y;
                swiping           = true;
                swipe_h_triggered = false;
                swipe_v_triggered = false;
            } else if (swiping) {
                // 持续按住 - 检测滑动
                int16_t dx = x - swipe_start_x;
                int16_t dy = y - swipe_start_y;

                // 上下滑动调节亮度
                if (!swipe_v_triggered && !swipe_h_triggered) {
                    if (dy < -60) {
                        brightness += 10;
                        if (brightness > 100) brightness = 100;
                        corep4.set_lcd_brightness((uint8_t)brightness);
                        ESP_LOGI(TAG, "Brightness UP: %d%%", brightness);
                        swipe_start_y     = y;
                        swipe_v_triggered = true;
                    } else if (dy > 60) {
                        brightness -= 10;
                        if (brightness < 5) brightness = 5;
                        corep4.set_lcd_brightness((uint8_t)brightness);
                        ESP_LOGI(TAG, "Brightness DOWN: %d%%", brightness);
                        swipe_start_y     = y;
                        swipe_v_triggered = true;
                    }
                }
                // 继续检测垂直滑动（允许连续调节）
                if (swipe_v_triggered) {
                    int16_t dy2 = y - swipe_start_y;
                    if (dy2 < -60) {
                        brightness += 10;
                        if (brightness > 100) brightness = 100;
                        corep4.set_lcd_brightness((uint8_t)brightness);
                        ESP_LOGI(TAG, "Brightness UP: %d%%", brightness);
                        swipe_start_y = y;
                    } else if (dy2 > 60) {
                        brightness -= 10;
                        if (brightness < 5) brightness = 5;
                        corep4.set_lcd_brightness((uint8_t)brightness);
                        ESP_LOGI(TAG, "Brightness DOWN: %d%%", brightness);
                        swipe_start_y = y;
                    }
                }

                // 左右滑动切换页面
                if (!swipe_h_triggered && !swipe_v_triggered) {
                    if (dx < -80) {
                        current_page = (current_page + 1) % num_pages;
                        draw_page();
                        swipe_h_triggered = true;
                    } else if (dx > 80) {
                        current_page = (current_page - 1 + num_pages) % num_pages;
                        draw_page();
                        swipe_h_triggered = true;
                    }
                }
            }
            was_touching = true;
        } else {
            // 手指抬起
            if (was_touching) {
                // 判断是否为tap（移动距离小）
                int16_t move_x = (touch_down_x >= 0) ? abs(tx[0] - touch_down_x) : 999;
                int16_t move_y = (touch_down_y >= 0) ? abs(ty[0] - touch_down_y) : 999;

                // 使用记录的按下坐标判断
                if (!swipe_h_triggered && !swipe_v_triggered && move_x < 30 && move_y < 30) {
                    // 是tap - 检查是否在中心区域
                    if (touch_down_x >= (CENTER - HALF_SIDE) && touch_down_x <= (CENTER + HALF_SIDE) &&
                        touch_down_y >= (CENTER - HALF_SIDE) && touch_down_y <= (CENTER + HALF_SIDE)) {
                        int64_t now = esp_timer_get_time();
                        if (tap_count == 0) {
                            tap_start_time = now;
                        }
                        tap_count++;
                        ESP_LOGI(TAG, "Center tap %d/5", tap_count);

                        if (tap_count >= 5) {
                            if ((now - tap_start_time) <= TAP_WINDOW_US) {
                                ESP_LOGI(TAG, "5 taps detected! Entering touch draw mode...");
                                // 进入触摸测试界面
                                draw_touch_grid(panel);
                                touch_draw_loop(panel, tp);
                                return;  // touch_draw_loop是死循环，不会到这里
                            }
                            tap_count = 0;  // 超时重置
                        }

                        // 检查超时
                        if ((now - tap_start_time) > TAP_WINDOW_US) {
                            tap_count      = 1;  // 重新开始计数
                            tap_start_time = now;
                        }
                    } else {
                        // 不在中心区域，重置
                        tap_count = 0;
                    }
                }
            }
            was_touching = false;
            swiping      = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
