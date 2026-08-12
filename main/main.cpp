#include "audio_tools.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tools.h"

#include <cstdint>

m5_corep4_tool corep4;

static const char *TAG = "main_app";

static const char *p4_raw_reset_reason_name(soc_reset_reason_t reason)
{
    switch (reason) {
        case RESET_REASON_CHIP_POWER_ON:
            return "CHIP_POWER_ON";
        case RESET_REASON_CORE_SW:
            return "CORE_SW";
        case RESET_REASON_CORE_DEEP_SLEEP:
            return "CORE_DEEP_SLEEP";
        case RESET_REASON_CORE_MWDT:
            return "CORE_MWDT";
        case RESET_REASON_CORE_RWDT:
            return "CORE_RWDT";
        case RESET_REASON_CPU_MWDT:
            return "CPU_MWDT";
        case RESET_REASON_CPU_SW:
            return "CPU_SW";
        case RESET_REASON_CPU_RWDT:
            return "CPU_RWDT";
        case RESET_REASON_SYS_BROWN_OUT:
            return "SYS_BROWN_OUT";
        case RESET_REASON_SYS_RWDT:
            return "SYS_RWDT";
        case RESET_REASON_SYS_SUPER_WDT:
            return "SYS_SUPER_WDT";
        case RESET_REASON_CORE_PWR_GLITCH:
            return "CORE_PWR_GLITCH";
        case RESET_REASON_CORE_EFUSE_CRC:
            return "CORE_EFUSE_CRC";
        case RESET_REASON_CORE_USB_JTAG:
            return "CORE_USB_JTAG";
        case RESET_REASON_CORE_USB_UART:
            return "CORE_USB_UART";
        case RESET_REASON_CPU_JTAG:
            return "CPU_JTAG";
        case RESET_REASON_CPU_LOCKUP:
            return "CPU_LOCKUP";
        case RESET_REASON_SYS_PSDET:
            return "SYS_PSDET";
        default:
            return "UNKNOWN";
    }
}

extern "C" void app_main(void)
{
    const soc_reset_reason_t raw_reset_reason = esp_rom_get_reset_reason(0);
    ESP_LOGI(TAG, "P4 raw reset reason: %s (0x%02x)", p4_raw_reset_reason_name(raw_reset_reason),
             static_cast<unsigned int>(raw_reset_reason));

    // Keep ordinary allocations above 1 KiB out of scarce internal/DMA RAM.
    // Apply this at runtime as well as through sdkconfig.defaults so an older
    // version-specific sdkconfig cannot silently retain the previous limit.
    heap_caps_malloc_extmem_enable(1024);

    log_firmware_metadata(TAG);

    corep4.init();  // 初始化corep4并配置为L3A电源模式

    // 测试代码 起点 --> （参考"测试说明书"中的代码片段）

    // -------------------- 电源管理 初始化 ----------------
    // 打开 PYB -> L3B电源
    corep4.set_status(StatusParam::POWER_PYB_L3B, true);

    // 设置 PYB -> CORE-P4 USBA 5v输出为开启
    corep4.set_status(StatusParam::POWER_PYB_USBA, false);
    vTaskDelay(200 / portTICK_PERIOD_MS);
    corep4.set_status(StatusParam::POWER_PYB_USBA, true);

    // 打开  PYB -> PYB_AU_PWR_EN 电源
    corep4.set_status(StatusParam::POWER_AUDIO_CHIP, true);

    // 先关闭功放，待 DAC 初始化稳定后异步开启，避免启动噪声
    corep4.otool_tools.audio.set_pa_power(false);

    // 打开  PYB -> PYB_LCD_PWR_EN 电源
    corep4.set_status(StatusParam::POWER_LCD, true);

    // 打开 PYB -> PYB_GROVE_PWR_EN 电源
    corep4.set_status(StatusParam::POWER_GROVE, true);

    // 配置 PYB -> Pin_PYB_P4USBC_OUT_EN 电源为开启
    corep4.set_status(StatusParam::POWER_P4USBC, false);
    vTaskDelay(200 / portTICK_PERIOD_MS);
    corep4.set_status(StatusParam::POWER_P4USBC, true);

    // 打开充电 -> PYB 供电，并设置充电电流为180mA
    corep4.set_status(StatusParam::POWER_CHARGE_CURRENT, static_cast<uint8_t>(CHARGE_CURRENT_180MA));

    // ESP-Hosted WiFi 模块复位
    esp_err_t wifi_reset_ret = corep4.esp_hosted_manual_reset();
    if (wifi_reset_ret != ESP_OK) {
        ESP_LOGW(TAG, "Startup WiFi module reset failed: %s", esp_err_to_name(wifi_reset_ret));
    }

    // -------------------- 外设 初始化 ----------------
    // 初始化 SD卡
    // vTaskDelay(pdMS_TO_TICKS(300)); // 等待L3B电源稳定
    // corep4.pyb.pinMode(Pin_PYB_SD_DECT, INPUT_PULLUP);
    // vTaskDelay(pdMS_TO_TICKS(100)); // 等待上拉电阻生效
    int sd_detect = corep4.pyb.digitalRead(Pin_PYB_SD_DECT);
    ESP_LOGI(TAG, "SD Card Detect: %s", sd_detect == 0 ? "Card Present" : "No Card");
    if (sd_detect == 0) {
        // 使用SPI模式，避免与ESP-Hosted WiFi共享SDMMC host。
        corep4.otool_tools.sd_card.set_mode(SD_MODE_SPI, Pin_P4_SD_SPI_CS, Pin_P4_SD_SPI_MOSI, Pin_P4_SD_SPI_MISO,
                                            Pin_P4_SD_SPI_CLK);
        corep4.otool_tools.sd_card.set_mount_point("/sdcard");
        esp_err_t sd_ret = corep4.otool_tools.sd_card.init();
        if (sd_ret != ESP_OK) {
            ESP_LOGE(TAG, "SD card init failed: %s", esp_err_to_name(sd_ret));
            corep4.set_status(StatusParam::SD_DETECTED, true, StatusConfigFlag::UPDATE_STATUS);
            corep4.set_status(StatusParam::SD_INITIALIZED, false, StatusConfigFlag::UPDATE_STATUS);
        } else {
            ESP_LOGI(TAG, "SD card initialized successfully");
            uint64_t sd_total = 0, sd_free = 0;
            corep4.otool_tools.sd_card.get_capacity_info(&sd_total, &sd_free);
            corep4.set_status(StatusParam::SD_DETECTED, true, StatusConfigFlag::UPDATE_STATUS);
            corep4.set_status(StatusParam::SD_INITIALIZED, true, StatusConfigFlag::UPDATE_STATUS);
            corep4.set_status(StatusParam::SD_TOTAL_BYTES, sd_total, StatusConfigFlag::UPDATE_STATUS);
            corep4.set_status(StatusParam::SD_FREE_BYTES, sd_free, StatusConfigFlag::UPDATE_STATUS);
        }
    } else {
        // 启动时无卡，确保状态为 Not Detected
        corep4.set_status(StatusParam::SD_DETECTED, false, StatusConfigFlag::UPDATE_STATUS);
        corep4.set_status(StatusParam::SD_INITIALIZED, false, StatusConfigFlag::UPDATE_STATUS);
    }

    // 初始化 RTC (RX8130)
    auto rtc_device_handle = corep4.otool_tools.i2c_tools.get_i2c_device_handle(I2C_ADDR_RTC);
    bool rtc_ok = corep4.rtc.init(corep4.otool_tools.i2c_tools.get_i2c_bus_handle(), &rtc_device_handle, GPIO_NUM_NC);
    if (rtc_ok) {
        corep4.rtc.initBat();        // 初始化电池备份
        corep4.rtc.clearIrqFlags();  // 清除中断标志
        repair_rtc_time_if_needed(corep4);
        ESP_LOGI(TAG, "RTC (RX8130) initialized successfully");
    } else {
        ESP_LOGE(TAG, "RTC init failed");
        if (auto *status = corep4.get_device_status()) {
            status->rtc.rtc_initialized = false;
        }
    }

    // 初始化 AUDIO (ES8311 DAC + ES7210 ADC)
    corep4.otool_tools.audio.set_i2s_pin_config(Pin_P4_I2S_BCLK_PIN, Pin_P4_I2S_MCLK_PIN, Pin_P4_I2S_DATA_IN_PIN,
                                                Pin_P4_I2S_DATA_OUT_PIN, Pin_P4_I2S_DATA_WS_PIN, Pin_P4_I2S_PWR_EN);
    esp_err_t audio_ret =
        corep4.otool_tools.audio.audio_system_init(corep4.otool_tools.i2c_tools.get_i2c_master_bus_handle(), I2S_NUM_0,
                                                   AUDIO_SAMPLE_RATE_16K, I2S_DATA_BIT_WIDTH_32BIT);
    if (audio_ret == ESP_OK) {
        corep4.otool_tools.audio.es8311_init(AUDIO_CHANNELS_STEREO, ES8311_MODE_DAC);
        // ES7210 必须启用 TDM 模式，使用单麦克风通道减少 RX DMA 负载
        corep4.otool_tools.audio.es7210_init(AUDIO_CHANNELS_STEREO, AUDIO_MIC_CHANNEL_ALL, ES7210_TDM_ENABLED);
        corep4.otool_tools.audio.set_volume(80.0);
        corep4.otool_tools.audio.set_record_gain(30.0);
        // DAC 输出稳定后再异步开功放，消除启动噪声
        corep4.otool_tools.audio.enable_pa_after_delay(500);
        // 增益
        corep4.otool_tools.audio.es7210_set_mic_channel_gain(AUDIO_MIC_CHANNEL_1, ES7210_MIC_GAIN_27DB);
        corep4.otool_tools.audio.es7210_set_mic_channel_gain(AUDIO_MIC_CHANNEL_2, ES7210_MIC_GAIN_27DB);
        corep4.otool_tools.audio.es7210_set_mic_channel_gain(AUDIO_MIC_CHANNEL_3, ES7210_MIC_GAIN_33DB);
        corep4.otool_tools.audio.es7210_set_mic_channel_gain(AUDIO_MIC_CHANNEL_4, ES7210_MIC_GAIN_27DB);

        // 禁音
        corep4.otool_tools.audio.es7210_set_mic_channel_mute(AUDIO_MIC_CHANNEL_1, false);
        corep4.otool_tools.audio.es7210_set_mic_channel_mute(AUDIO_MIC_CHANNEL_2, false);
        corep4.otool_tools.audio.es7210_set_mic_channel_mute(AUDIO_MIC_CHANNEL_3, false);
        corep4.otool_tools.audio.es7210_set_mic_channel_mute(AUDIO_MIC_CHANNEL_4, false);
        ESP_LOGI(TAG, "Audio system initialized successfully");

        // Startup 提示音
        corep4.set_startup_prompt_audio_delay_ms(1000);
        play_startup_prompt_sound(corep4, corep4.get_startup_prompt_audio_delay_ms());
    } else {
        ESP_LOGE(TAG, "Audio system init failed: %s", esp_err_to_name(audio_ret));
    }

    // -------------------- 以太网 初始化 ----------------
    // // 检测以太网PHY芯片 (IP101)
    ESP_LOGI(TAG, "Detecting Ethernet PHY chip...");

    // 确保L3B电源已经稳定（额外等待）
    ESP_LOGI(TAG, "Waiting for L3B power to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(200));

    // 手动复位PHY芯片 (GPIO52)
    ESP_LOGI(TAG, "Performing PHY reset sequence...");
    gpio_config_t io_conf = {};
    io_conf.intr_type     = GPIO_INTR_DISABLE;
    io_conf.mode          = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask  = (1ULL << 52);
    io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en    = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // PHY复位时序: 低电平至少10ms
    gpio_set_level(GPIO_NUM_52, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(GPIO_NUM_52, 1);
    vTaskDelay(pdMS_TO_TICKS(100));  // 等待PHY启动

    ESP_LOGI(TAG, "PHY reset sequence completed");

    esp_err_t eth_ret = corep4.ethernet_init(true);
    if (eth_ret == ESP_OK) {
        ESP_LOGI(TAG, "Ethernet initialized, starting background wait task...");
        corep4.set_status(StatusParam::ETH_INITIALIZED, true,
                          StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);

        // 后台持续等待网线连接和 IP 就绪；事件处理器会同步更新状态和 UI。
        start_ethernet_wait_task(corep4);
    } else {
        ESP_LOGE(TAG, "Ethernet initialization failed: %s", esp_err_to_name(eth_ret));
        corep4.set_status(StatusParam::ETH_INITIALIZED, false,
                          StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
    }

    // -------------------- LCD 和触摸屏 初始化 ----------------
    // LCD 初始化和测试
    corep4.set_lcd_power(true);            // 打开 LCD 电源
    corep4.reset_lcd();                    // LCD 复位序列
    corep4.reset_touch();                  // 触摸复位序列
    corep4.lcd_init();                     // 初始化LCD
    corep4.touch_init();                   // 初始化触摸屏
    vTaskDelay(100 / portTICK_PERIOD_MS);  // 等待LCD稳定
    corep4.set_status(StatusParam::POWER_LCD_BRIGHTNESS, static_cast<uint8_t>(100));
    corep4.lcd_clear(false);  // 清屏为黑色

    // corep4.touch_test_demo(); // 触摸测试 - 支持多点触摸轨迹显示
    // corep4.lcd_draw_rainbow_stripes(corep4.get_lcd_panel_handle(), 480, 480, true); //
    // 显示彩虹条测试屏，并全屏显示每种颜色

    corep4.lvgl_init();                                 // 初始化LVGL
    corep4.set_lvgl_rotation(LV_DISPLAY_ROTATION_180);  // 设置LVGL旋转180度

    // corep4.start_lvgl_benchmark_demo(); // 启动LVGL基准测试演示
    // corep4.start_lvgl_music_demo(); // 启动LVGL音乐播放器演示
    corep4.start_lvgl_custom_demo();  // 启动自定义LVGL界面（内部已初始化UI同步模块）

    // //检查测试音频文件是否可用，播放 AUDIO_FILE_CANDY_WIND_2CH_16K_16B_9S
    // auto* pb = corep4.otool_tools.audio.get_playback();
    // if (pb && pb->is_audio_file_available(AUDIO_FILE_CANDY_WIND_2CH_16K_16B_9S)) {
    //     ESP_LOGI(TAG, "Playing AUDIO_FILE_CANDY_WIND_2CH_16K_16B_9S audio file...");
    //     pb->play_audio_file(AUDIO_FILE_CANDY_WIND_2CH_16K_16B_9S, AUDIO_PLAYBACK_ASYNC, 0); // 异步播放
    //     vTaskDelay(pdMS_TO_TICKS(1000));
    // }
    // else {
    //     ESP_LOGE(TAG, "AUDIO_FILE_CANDY_WIND_2CH_16K_16B_9S audio file not available");
    // }

    // // 循环播放测试音频，若不存在则定期重试
    // auto* pb2 = corep4.otool_tools.audio.get_playback();
    // while (pb2) {
    //     if (pb2->is_audio_file_available(AUDIO_FILE_SINE_440HZ_2CH_16K_16B_10S)) {
    //         ESP_LOGI(TAG, "Playing AUDIO_FILE_SINE_440HZ_2CH_16K_16B_10S audio file...");
    //         pb2->play_audio_file(AUDIO_FILE_SINE_440HZ_2CH_16K_16B_10S, AUDIO_PLAYBACK_BLOCKING, 0);
    //         vTaskDelay(pdMS_TO_TICKS(500));
    //     } else {
    //         ESP_LOGE(TAG, "AUDIO_FILE_SINE_440HZ_2CH_16K_16B_10S audio file not available");
    //         vTaskDelay(pdMS_TO_TICKS(5000));
    //     }
    // }

    // 录音播放测试
    //  corep4.otool_tools.audio.get_recorder()->record_and_play_test_with_channel_select(10, AUDIO_MIC_CHANNEL_3); //
    //  10秒录音，播放MIC3
    // 录音到文件测试
    //  corep4.otool_tools.audio.get_recorder()->record_all_channel_to_files(10, "/sdcard/mic_test", "micdump"); //
    //  录音10秒，保存所有麦克风通道到文件

    // -------------------- 流式 AEC 测试 ----------------
    // 初始化 AEC → 启动流式会话 → 边播放边录制消回声 → 读取净化音频 → 停止
    // {
    //     auto* afe = corep4.otool_tools.audio.get_sr_afe();
    //     if (afe) {
    //         // 1) 初始化 AEC (MIC1=期望信号, MIC3=硬件回采参考)
    //         esp_err_t rc = afe->aec_init(AUDIO_MIC_CHANNEL_1, AUDIO_MIC_CHANNEL_3, 4, AFE_MODE_LOW_COST);
    //         if (rc == ESP_OK) {
    //             // 2) 启动流式会话 (后台任务持续消回声, 系数跨 session 保留)
    //             rc = afe->aec_session_start(64 * 1024);
    //             if (rc == ESP_OK) {
    //                 // 3) 异步播放测试音频 (AEC 将实时消除此音频的回声)
    //                 auto* pb_aec = corep4.otool_tools.audio.get_playback();
    //                 if (pb_aec && pb_aec->is_audio_file_available(AUDIO_FILE_CANDY_WIND_2CH_16K_16B_9S)) {
    //                     pb_aec->play_audio_file(AUDIO_FILE_CANDY_WIND_2CH_16K_16B_9S, AUDIO_PLAYBACK_ASYNC, 0);
    //                 }
    //                 // 4) 从 RingBuffer 读取 AEC 净化后的音频 (16kHz/16bit/mono)
    //                 const size_t buf_size = 16000 * 2;  // 1秒的数据
    //                 int16_t* aec_buf = (int16_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    //                 if (aec_buf) {
    //                     for (int i = 0; i < 10; ++i) {  // 读取约10秒
    //                         int bytes_read = afe->aec_session_read(aec_buf, buf_size, 1000);
    //                         if (bytes_read > 0) {
    //                             ESP_LOGI(TAG, "AEC session read: %d bytes (iter %d)", bytes_read, i);
    //                             // 可以将 aec_buf 喂给语音识别、保存到文件等
    //                         }
    //                     }
    //                     heap_caps_free(aec_buf);
    //                 }
    //                 // 5) 停止播放和会话 (AEC 系数保留, 下次 start 直接接续)
    //                 if (pb_aec) pb_aec->stop_async_playback();
    //                 afe->aec_session_stop();
    //             }
    //             // 6) 完全退出时才销毁 AEC handle
    //             // afe->aec_deinit();
    //         }
    //     }
    // }

    // // 手动复位ESP-Hosted设备（如ESP32-C6）
    // corep4.esp_hosted_manual_reset();
    // // 连接到指定WiFi网络
    // corep4.esp32c6_wifi_connect("M5Stack", "m5office888");
    // // 扫描附近WiFi网络
    // ESP_LOGI(TAG, "Attempting WiFi scan with ESP32-C6 optimized parameters...");
    // esp_err_t wifi_scan_result = corep4.esp32c6_wifi_scan(32, 60000, true);  // 增加超时时间，启用隐藏网络扫描

    // if (wifi_scan_result != ESP_OK) {
    //     ESP_LOGE(TAG, "WiFi scan failed with error: %s", esp_err_to_name(wifi_scan_result));
    //     ESP_LOGI(TAG, "This may be due to RPC communication issues with ESP32-C6");
    //     ESP_LOGI(TAG, "System will continue with other functionality...");
    // } else {
    //     ESP_LOGI(TAG, "WiFi scan completed successfully");
    // }

    // 测试代码 终点 <--

    // 主循环
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));  // 每5秒打印一次主循环状态
    }
}
