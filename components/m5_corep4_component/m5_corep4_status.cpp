/**
 * @file m5_corep4_status.cpp
 * @brief M5 CoreP4 设备状态管理模块实现
 * 
 * 包含设备状态初始化、获取、设置、打印等功能
 * 从 m5_corep4_component.cpp 拆分出来以减轻主文件负担
 * 
 * @author AI Assistant
 * @date 2025-12-31
 */

#include "m5_corep4_component.h"
#include "esp_timer.h"

static const char *TAG = "m5_corep4_status";

// ==================== 辅助函数 ====================

/**
 * @brief 将重启原因转换为字符串
 * 
 * @param reason 重启原因枚举值
 * @return const char* 对应的字符串描述
 */
static const char* __attribute__((unused)) reset_reason_to_string(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_UNKNOWN:   return "Unknown";
        case ESP_RST_POWERON:   return "Power-on";
        case ESP_RST_EXT:       return "External";
        case ESP_RST_SW:        return "Software";
        case ESP_RST_PANIC:     return "Exception/Panic";
        case ESP_RST_INT_WDT:   return "Interrupt Watchdog";
        case ESP_RST_TASK_WDT:  return "Task Watchdog";
        case ESP_RST_WDT:       return "Other Watchdog";
        case ESP_RST_DEEPSLEEP: return "Deep Sleep";
        case ESP_RST_BROWNOUT:  return "Brownout";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "Unknown";
    }
}

// ==================== 设备状态初始化 ====================

esp_err_t m5_corep4_tool::init_device_status(void)
{
    ESP_LOGI(TAG, "=== Initializing Device Real-time Status ===");

    // 清零整个状态结构体
    memset(&device_status, 0, sizeof(device_realtime_status_t));

    // 设置魔数和版本号
    device_status.magic = DEVICE_STATUS_MAGIC;
    device_status.version = DEVICE_STATUS_VERSION;

    // 记录启动时间戳
    device_status.uptime_seconds = 0;

    // 记录重启原因
    device_status.last_reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %s", reset_reason_to_string(device_status.last_reset_reason));

    // 初始化电源状态 (默认值)
    device_status.power.lcd_power = false;
    device_status.power.audio_chip_power = false;
    device_status.power.audio_pa_power = false;
    device_status.power.pyb_l3b_power = false;
    device_status.power.pyb_usba_power = false;
    device_status.power.grove_power = false;
    device_status.power.p4usbc_power = false;
    device_status.power.lcd_brightness = 0;
    device_status.power.charge_current = CHARGE_CURRENT_180MA;
    device_status.power.pm1_initialized = false;
    device_status.power.pyb_initialized = false;
    device_status.power.pyb_firmware_version = 0;

    // 初始化屏幕状态
    device_status.screen.lcd_initialized = false;
    device_status.screen.lcd_power_on = false;
    device_status.screen.lcd_h_res = LCD_MIPI_DSI_LCD_H_RES;
    device_status.screen.lcd_v_res = LCD_MIPI_DSI_LCD_V_RES;
    device_status.screen.lcd_brightness = 0;
    device_status.screen.rotation = LV_DISPLAY_ROTATION_0;
    device_status.screen.lcd_color_depth = LCD_MIPI_DSI_COLOR_DEPTH;

    // 初始化触摸状态
    device_status.touch.touch_initialized = false;
    device_status.touch.touch_power_on = false;
    device_status.touch.touch_sleep_mode = false;
    device_status.touch.touch_points = 0;
    device_status.touch.last_touch_x = 0;
    device_status.touch.last_touch_y = 0;

    // 初始化WiFi状态
    device_status.wifi.wifi_initialized = false;
    device_status.wifi.wifi_connected = false;
    memset(device_status.wifi.ssid, 0, sizeof(device_status.wifi.ssid));
    device_status.wifi.rssi = 0;
    device_status.wifi.channel = 0;
    device_status.wifi.auth_mode = 0;
    memset(device_status.wifi.wifi_mac_addr, 0, sizeof(device_status.wifi.wifi_mac_addr));

    // 初始化以太网状态
    device_status.ethernet.ethernet_initialized = false;
    device_status.ethernet.ethernet_connected = false;
    device_status.ethernet.got_ip = false;
    memset(device_status.ethernet.eth_mac_addr, 0, sizeof(device_status.ethernet.eth_mac_addr));
    memset(device_status.ethernet.ip_address, 0, sizeof(device_status.ethernet.ip_address));
    memset(device_status.ethernet.netmask, 0, sizeof(device_status.ethernet.netmask));
    memset(device_status.ethernet.gateway, 0, sizeof(device_status.ethernet.gateway));

    // 初始化SD卡状态
    device_status.sdcard.sd_detected = false;
    device_status.sdcard.sd_initialized = false;
    device_status.sdcard.sd_total_bytes = 0;
    device_status.sdcard.sd_free_bytes = 0;
    memset(device_status.sdcard.sd_card_type, 0, sizeof(device_status.sdcard.sd_card_type));

    // 初始化音频状态
    device_status.audio.es8311_initialized = false;
    device_status.audio.es7210_initialized = false;
    device_status.audio.audio_power_on = false;
    device_status.audio.volume = 50; // 默认音量50%
    device_status.audio.is_playing = false;

    // 初始化IMU状态
    device_status.imu.bmi270_initialized = false;
    device_status.imu.acc_enabled = false;
    device_status.imu.gyr_enabled = false;
    device_status.imu.mag_enabled = false;
    device_status.imu.acc_x = 0.0f;
    device_status.imu.acc_y = 0.0f;
    device_status.imu.acc_z = 0.0f;
    device_status.imu.gyr_x = 0.0f;
    device_status.imu.gyr_y = 0.0f;
    device_status.imu.gyr_z = 0.0f;
    device_status.imu.mag_x = 0;
    device_status.imu.mag_y = 0;
    device_status.imu.mag_z = 0;

    // 初始化USB鼠标状态
    device_status.usb_mouse.usb_host_initialized = false;
    device_status.usb_mouse.mouse_connected = false;
    device_status.usb_mouse.mouse_x = 0;
    device_status.usb_mouse.mouse_y = 0;
    device_status.usb_mouse.left_button = false;
    device_status.usb_mouse.right_button = false;
    device_status.usb_mouse.middle_button = false;

    // 初始化RTC状态
    device_status.rtc.rtc_initialized = false;
    memset(&device_status.rtc.current_time, 0, sizeof(device_status.rtc.current_time));
    device_status.rtc.timer_irq_enabled = false;

    // 初始化IR状态
    device_status.ir.ir_initialized = false;
    device_status.ir.current_format = ir_format_t::NEC; // 默认NEC格式

    // 初始化睡眠/唤醒状态
    device_status.sleep.btn_sleep_enable = true;     // 默认启用单击休眠
    device_status.sleep.wake_rtc_enable = false;     // 默认禁用RTC唤醒
    device_status.sleep.wake_touch_enable = false;   // 默认禁用触摸唤醒
    device_status.sleep.wake_bmi_enable = false;     // 默认禁用BMI唤醒
    device_status.sleep.wake_rtc_seconds = 60;       // 默认60秒

    // 清空保留字段
    memset(device_status.reserved, 0, sizeof(device_status.reserved));

    ESP_LOGI(TAG, "Device status initialized:");
    ESP_LOGI(TAG, "  Magic: 0x%08X", static_cast<unsigned>(device_status.magic));
    ESP_LOGI(TAG, "  Version: 0x%08X", static_cast<unsigned>(device_status.version));
    ESP_LOGI(TAG, "  Reset Reason: %s", reset_reason_to_string(device_status.last_reset_reason));
    ESP_LOGI(TAG, "  Boot Timestamp: %llu ms", static_cast<unsigned long long>(boot_timestamp_ms));

    ESP_LOGI(TAG, "=== Device Real-time Status Init Complete ===");
    return ESP_OK;
}

// ==================== 设备状态获取 ====================

device_realtime_status_t* m5_corep4_tool::get_device_status(void)
{
    // 验证魔数
    if (device_status.magic != DEVICE_STATUS_MAGIC) {
        ESP_LOGW(TAG, "Device status magic mismatch: 0x%08X (expected 0x%08X)",
                 static_cast<unsigned>(device_status.magic),
                 static_cast<unsigned>(DEVICE_STATUS_MAGIC));
    }
    return &device_status;
}

// ==================== 设备状态打印 ====================

void m5_corep4_tool::print_device_status(esp_log_level_t level)
{
    const char* level_tag = "";
    switch (level) {
        case ESP_LOG_ERROR:   level_tag = "E"; break;
        case ESP_LOG_WARN:    level_tag = "W"; break;
        case ESP_LOG_INFO:    level_tag = "I"; break;
        case ESP_LOG_DEBUG:   level_tag = "D"; break;
        case ESP_LOG_VERBOSE: level_tag = "V"; break;
        default:              level_tag = "I"; break;
    }

    ESP_LOG_LEVEL(level, TAG, "========== Device Real-time Status ==========");
    ESP_LOG_LEVEL(level, TAG, "Magic: 0x%08X, Version: 0x%08X, Uptime: %llu sec",
                  static_cast<unsigned>(device_status.magic),
                  static_cast<unsigned>(device_status.version),
                  static_cast<unsigned long long>(device_status.uptime_seconds));
    ESP_LOG_LEVEL(level, TAG, "Reset Reason: %s", reset_reason_to_string(device_status.last_reset_reason));

    ESP_LOG_LEVEL(level, TAG, "[Power]");
    ESP_LOG_LEVEL(level, TAG, "  LCD_PWR=%d AU_CHIP_PWR=%d AU_PA_PWR=%d L3B_PWR=%d USBA_PWR=%d",
                  device_status.power.lcd_power, device_status.power.audio_chip_power,
                  device_status.power.audio_pa_power, device_status.power.pyb_l3b_power,
                  device_status.power.pyb_usba_power);
    ESP_LOG_LEVEL(level, TAG, "  Grove=%d P4USBC=%d Brightness=%d%% ChargeCurrent=%dmA",
                  device_status.power.grove_power, device_status.power.p4usbc_power,
                  device_status.power.lcd_brightness,
                  device_status.power.charge_current == CHARGE_CURRENT_650MA ? 650 :
                      (device_status.power.charge_current == CHARGE_CURRENT_OFF ? 0 : 180));
    ESP_LOG_LEVEL(level, TAG, "  PM1_Init=%d PYB_Init=%d PYB_FWVer=%d",
                  device_status.power.pm1_initialized,
                  device_status.power.pyb_initialized,
                  device_status.power.pyb_firmware_version);

    ESP_LOG_LEVEL(level, TAG, "[Screen]");
    ESP_LOG_LEVEL(level, TAG, "  Init=%d PowerOn=%d Res=%dx%d Brightness=%d%% Rotation=%d ColorDepth=%d",
                  device_status.screen.lcd_initialized, device_status.screen.lcd_power_on,
                  static_cast<int>(device_status.screen.lcd_h_res),
                  static_cast<int>(device_status.screen.lcd_v_res),
                  static_cast<int>(device_status.screen.lcd_brightness),
                  static_cast<int>(device_status.screen.rotation),
                  static_cast<int>(device_status.screen.lcd_color_depth));

    ESP_LOG_LEVEL(level, TAG, "[Touch]");
    ESP_LOG_LEVEL(level, TAG, "  Init=%d PowerOn=%d Sleep=%d Points=%d LastPos=(%d,%d)",
                  device_status.touch.touch_initialized, device_status.touch.touch_power_on,
                  device_status.touch.touch_sleep_mode, device_status.touch.touch_points,
                  device_status.touch.last_touch_x, device_status.touch.last_touch_y);

    ESP_LOG_LEVEL(level, TAG, "[WiFi]");
    ESP_LOG_LEVEL(level, TAG, "  Init=%d Connected=%d SSID=%s RSSI=%d Ch=%d",
                  device_status.wifi.wifi_initialized, device_status.wifi.wifi_connected,
                  device_status.wifi.ssid, device_status.wifi.rssi, device_status.wifi.channel);

    ESP_LOG_LEVEL(level, TAG, "[Ethernet]");
    ESP_LOG_LEVEL(level, TAG, "  Init=%d Connected=%d GotIP=%d IP=%s",
                  device_status.ethernet.ethernet_initialized,
                  device_status.ethernet.ethernet_connected,
                  device_status.ethernet.got_ip,
                  device_status.ethernet.ip_address);

    ESP_LOG_LEVEL(level, TAG, "[SDCard]");
    ESP_LOG_LEVEL(level, TAG, "  Detected=%d Init=%d Type=%s Total=%llu MB Free=%llu MB",
                  device_status.sdcard.sd_detected,
                  device_status.sdcard.sd_initialized,
                  device_status.sdcard.sd_card_type,
                  static_cast<unsigned long long>(device_status.sdcard.sd_total_bytes / (1024*1024)),
                  static_cast<unsigned long long>(device_status.sdcard.sd_free_bytes / (1024*1024)));

    ESP_LOG_LEVEL(level, TAG, "[Audio]");
    ESP_LOG_LEVEL(level, TAG, "  ES8311=%d ES7210=%d PowerOn=%d Volume=%d%% Playing=%d",
                  device_status.audio.es8311_initialized,
                  device_status.audio.es7210_initialized,
                  device_status.audio.audio_power_on,
                  device_status.audio.volume,
                  device_status.audio.is_playing);

    ESP_LOG_LEVEL(level, TAG, "[IMU]");
    ESP_LOG_LEVEL(level, TAG, "  BMI270=%d ACC=%d GYR=%d MAG=%d",
                  device_status.imu.bmi270_initialized,
                  device_status.imu.acc_enabled,
                  device_status.imu.gyr_enabled,
                  device_status.imu.mag_enabled);
    ESP_LOG_LEVEL(level, TAG, "  ACC: (%.2f, %.2f, %.2f) g",
                  device_status.imu.acc_x, device_status.imu.acc_y, device_status.imu.acc_z);
    ESP_LOG_LEVEL(level, TAG, "  GYR: (%.2f, %.2f, %.2f) dps",
                  device_status.imu.gyr_x, device_status.imu.gyr_y, device_status.imu.gyr_z);
    ESP_LOG_LEVEL(level, TAG, "  MAG: (%d, %d, %d)",
                  device_status.imu.mag_x, device_status.imu.mag_y, device_status.imu.mag_z);

    ESP_LOG_LEVEL(level, TAG, "[USB Mouse]");
    ESP_LOG_LEVEL(level, TAG, "  USBHost=%d Connected=%d Pos=(%d,%d) L=%d R=%d M=%d",
                  device_status.usb_mouse.usb_host_initialized,
                  device_status.usb_mouse.mouse_connected,
                  device_status.usb_mouse.mouse_x,
                  device_status.usb_mouse.mouse_y,
                  device_status.usb_mouse.left_button,
                  device_status.usb_mouse.right_button,
                  device_status.usb_mouse.middle_button);

    ESP_LOG_LEVEL(level, TAG, "[RTC]");
    ESP_LOG_LEVEL(level, TAG, "  Init=%d TimerIRQ=%d Time: %04d-%02d-%02d %02d:%02d:%02d",
                  device_status.rtc.rtc_initialized,
                  device_status.rtc.timer_irq_enabled,
                  device_status.rtc.current_time.tm_year + 1900,
                  device_status.rtc.current_time.tm_mon + 1,
                  device_status.rtc.current_time.tm_mday,
                  device_status.rtc.current_time.tm_hour,
                  device_status.rtc.current_time.tm_min,
                  device_status.rtc.current_time.tm_sec);

    ESP_LOG_LEVEL(level, TAG, "[IR]");
    ESP_LOG_LEVEL(level, TAG, "  Init=%d Format=%s",
                  device_status.ir.ir_initialized,
                  device_status.ir.current_format == ir_format_t::NEC ? "NEC" : "RC5");

    ESP_LOG_LEVEL(level, TAG, "===============================================");
}

// ==================== 运行时间更新 ====================

esp_err_t m5_corep4_tool::update_uptime(void)
{
    uint64_t current_time_ms = esp_timer_get_time() / 1000;
    device_status.uptime_seconds = (current_time_ms - boot_timestamp_ms) / 1000;
    return ESP_OK;
}

// ==================== 通用状态配置函数实现 ====================

void m5_corep4_tool::register_ui_sync_callback(StatusUISyncCallback callback)
{
    if (callback) {
        ui_sync_callbacks.push_back(callback);
        ESP_LOGI(TAG, "UI sync callback registered, total: %d", (int)ui_sync_callbacks.size());
    }
}

esp_err_t m5_corep4_tool::set_status(StatusParam param, const StatusValue& value, 
                                     StatusConfigFlag flags)
{
    esp_err_t ret = ESP_OK;
    
    // 根据参数类型执行不同操作
    switch (param) {
        // ========== 电源相关参数 ==========
        case StatusParam::POWER_LCD: {
            bool on;
            if (!get_status_value(value, on)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::APPLY_HARDWARE) {
                ret = set_lcd_power(on);
            }
            if (ret == ESP_OK && (flags & StatusConfigFlag::UPDATE_STATUS)) {
                device_status.power.lcd_power = on;
            }
            break;
        }
        
        case StatusParam::POWER_AUDIO_CHIP: {
            bool on;
            if (!get_status_value(value, on)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::APPLY_HARDWARE) {
                ret = set_audio_chip_power(on);
            }
            if (ret == ESP_OK && (flags & StatusConfigFlag::UPDATE_STATUS)) {
                device_status.power.audio_chip_power = on;
            }
            break;
        }
        
        case StatusParam::POWER_AUDIO_PA: {
            bool on;
            if (!get_status_value(value, on)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::APPLY_HARDWARE) {
                ret = set_audio_pa_power(on);
            }
            if (ret == ESP_OK && (flags & StatusConfigFlag::UPDATE_STATUS)) {
                device_status.power.audio_pa_power = on;
            }
            break;
        }
        
        case StatusParam::POWER_PYB_L3B: {
            bool on;
            if (!get_status_value(value, on)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::APPLY_HARDWARE) {
                ret = set_pyb_l3b_power(on);
            }
            if (ret == ESP_OK && (flags & StatusConfigFlag::UPDATE_STATUS)) {
                device_status.power.pyb_l3b_power = on;
            }
            break;
        }
        
        case StatusParam::POWER_PYB_USBA: {
            bool on;
            if (!get_status_value(value, on)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::APPLY_HARDWARE) {
                ret = set_pyb_usba_power(on);
            }
            if (ret == ESP_OK && (flags & StatusConfigFlag::UPDATE_STATUS)) {
                device_status.power.pyb_usba_power = on;
            }
            break;
        }
        
        case StatusParam::POWER_GROVE: {
            bool on;
            if (!get_status_value(value, on)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::APPLY_HARDWARE) {
                ret = set_grove_power(on);
            }
            if (ret == ESP_OK && (flags & StatusConfigFlag::UPDATE_STATUS)) {
                device_status.power.grove_power = on;
            }
            break;
        }
        
        case StatusParam::POWER_P4USBC: {
            bool on;
            if (!get_status_value(value, on)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::APPLY_HARDWARE) {
                ret = set_p4usbc_power(on);
            }
            if (ret == ESP_OK && (flags & StatusConfigFlag::UPDATE_STATUS)) {
                device_status.power.p4usbc_power = on;
            }
            break;
        }

        case StatusParam::POWER_M5BUS: {
            bool on;
            if (!get_status_value(value, on)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::APPLY_HARDWARE) {
                ret = set_m5bus_power(on);
            }
            if (ret == ESP_OK && (flags & StatusConfigFlag::UPDATE_STATUS)) {
                device_status.power.m5bus_power = on;
            }
            break;
        }

        case StatusParam::POWER_LCD_BRIGHTNESS: {
            uint8_t brightness;
            if (!get_status_value(value, brightness)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::APPLY_HARDWARE) {
                ret = set_lcd_brightness(brightness);
            }
            if (ret == ESP_OK && (flags & StatusConfigFlag::UPDATE_STATUS)) {
                device_status.power.lcd_brightness = brightness;
            }
            break;
        }
        
        case StatusParam::POWER_CHARGE_CURRENT: {
            // 支持 uint8_t 作为 charge_current_t
            uint8_t current_val;
            if (!get_status_value(value, current_val)) {
                return ESP_ERR_INVALID_ARG;
            }
            charge_current_t current = static_cast<charge_current_t>(current_val);
            if (flags & StatusConfigFlag::APPLY_HARDWARE) {
                ret = set_charge_current(current);
            }
            if (ret == ESP_OK && (flags & StatusConfigFlag::UPDATE_STATUS)) {
                device_status.power.charge_current = current;
            }
            break;
        }

        case StatusParam::POWER_PM1_INITIALIZED: {
            bool initialized;
            if (!get_status_value(value, initialized)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.power.pm1_initialized = initialized;
            }
            break;
        }

        case StatusParam::POWER_PYB_INITIALIZED: {
            bool initialized;
            if (!get_status_value(value, initialized)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.power.pyb_initialized = initialized;
            }
            break;
        }

        // ========== 屏幕相关参数 ==========
        case StatusParam::SCREEN_ROTATION: {
            uint8_t rotation;
            if (!get_status_value(value, rotation)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::APPLY_HARDWARE) {
                ret = set_lvgl_rotation(static_cast<lv_disp_rotation_t>(rotation));
            }
            if (ret == ESP_OK && (flags & StatusConfigFlag::UPDATE_STATUS)) {
                device_status.screen.rotation = static_cast<lv_disp_rotation_t>(rotation);
            }
            break;
        }
        
        // ========== 睡眠/唤醒相关参数 ==========
        case StatusParam::SLEEP_BTN_ENABLE: {
            bool enable;
            if (!get_status_value(value, enable)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.sleep.btn_sleep_enable = enable;
            }
            // 注意: 硬件应用在进入休眠时执行，此处仅更新状态
            break;
        }
        
        case StatusParam::SLEEP_WAKE_RTC: {
            bool enable;
            if (!get_status_value(value, enable)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.sleep.wake_rtc_enable = enable;
            }
            break;
        }
        
        case StatusParam::SLEEP_WAKE_TOUCH: {
            bool enable;
            if (!get_status_value(value, enable)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.sleep.wake_touch_enable = enable;
            }
            break;
        }
        
        case StatusParam::SLEEP_WAKE_BMI: {
            bool enable;
            if (!get_status_value(value, enable)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.sleep.wake_bmi_enable = enable;
            }
            break;
        }
        
        case StatusParam::SLEEP_WAKE_RTC_SEC: {
            uint32_t seconds;
            if (!get_status_value(value, seconds)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.sleep.wake_rtc_seconds = seconds;
            }
            break;
        }

        // ========== SD卡相关参数 ==========
        case StatusParam::SD_DETECTED: {
            bool detected;
            if (!get_status_value(value, detected)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.sdcard.sd_detected = detected;
            }
            break;
        }

        case StatusParam::SD_INITIALIZED: {
            bool init;
            if (!get_status_value(value, init)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.sdcard.sd_initialized = init;
            }
            break;
        }

        case StatusParam::SD_TOTAL_BYTES: {
            uint64_t total;
            if (!get_status_value(value, total)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.sdcard.sd_total_bytes = total;
            }
            break;
        }

        case StatusParam::SD_FREE_BYTES: {
            uint64_t free_bytes;
            if (!get_status_value(value, free_bytes)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.sdcard.sd_free_bytes = free_bytes;
            }
            break;
        }

        // ========== 以太网相关参数 ==========
        case StatusParam::WIFI_INITIALIZED: {
            bool initialized;
            if (!get_status_value(value, initialized)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.wifi.wifi_initialized = initialized;
            }
            break;
        }

        case StatusParam::WIFI_CONNECTED: {
            bool connected;
            if (!get_status_value(value, connected)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.wifi.wifi_connected = connected;
                if (!connected) {
                    device_status.wifi.ssid[0] = '\0';
                    device_status.wifi.rssi = 0;
                    device_status.wifi.channel = 0;
                    device_status.wifi.auth_mode = 0;
                }
            }
            break;
        }

        case StatusParam::WIFI_RSSI: {
            int8_t rssi;
            if (!get_status_value(value, rssi)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.wifi.rssi = rssi;
            }
            break;
        }

        case StatusParam::WIFI_CHANNEL: {
            uint8_t channel;
            if (!get_status_value(value, channel)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.wifi.channel = channel;
            }
            break;
        }

        case StatusParam::WIFI_AUTH_MODE: {
            uint8_t auth_mode;
            if (!get_status_value(value, auth_mode)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.wifi.auth_mode = auth_mode;
            }
            break;
        }

        case StatusParam::ETH_INITIALIZED: {
            bool initialized;
            if (!get_status_value(value, initialized)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.ethernet.ethernet_initialized = initialized;
            }
            break;
        }

        case StatusParam::ETH_CONNECTED: {
            bool connected;
            if (!get_status_value(value, connected)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.ethernet.ethernet_connected = connected;
            }
            break;
        }

        case StatusParam::ETH_GOT_IP: {
            bool got_ip;
            if (!get_status_value(value, got_ip)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.ethernet.got_ip = got_ip;
            }
            break;
        }

        // ========== IMU related parameters ==========
        case StatusParam::IMU_BMI270_INITIALIZED: {
            bool initialized;
            if (!get_status_value(value, initialized)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.imu.bmi270_initialized = initialized;
            }
            break;
        }

        case StatusParam::IMU_ACC_ENABLED: {
            bool enabled;
            if (!get_status_value(value, enabled)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.imu.acc_enabled = enabled;
            }
            break;
        }

        case StatusParam::IMU_GYR_ENABLED: {
            bool enabled;
            if (!get_status_value(value, enabled)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.imu.gyr_enabled = enabled;
            }
            break;
        }

        case StatusParam::IMU_MAG_ENABLED: {
            bool enabled;
            if (!get_status_value(value, enabled)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                device_status.imu.mag_enabled = enabled;
            }
            break;
        }

        case StatusParam::IMU_ACC_X:
        case StatusParam::IMU_ACC_Y:
        case StatusParam::IMU_ACC_Z:
        case StatusParam::IMU_GYR_X:
        case StatusParam::IMU_GYR_Y:
        case StatusParam::IMU_GYR_Z: {
            float sensor_value = 0.0f;
            if (!get_status_value(value, sensor_value)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                switch (param) {
                    case StatusParam::IMU_ACC_X: device_status.imu.acc_x = sensor_value; break;
                    case StatusParam::IMU_ACC_Y: device_status.imu.acc_y = sensor_value; break;
                    case StatusParam::IMU_ACC_Z: device_status.imu.acc_z = sensor_value; break;
                    case StatusParam::IMU_GYR_X: device_status.imu.gyr_x = sensor_value; break;
                    case StatusParam::IMU_GYR_Y: device_status.imu.gyr_y = sensor_value; break;
                    case StatusParam::IMU_GYR_Z: device_status.imu.gyr_z = sensor_value; break;
                    default: break;
                }
            }
            break;
        }

        case StatusParam::IMU_MAG_X:
        case StatusParam::IMU_MAG_Y:
        case StatusParam::IMU_MAG_Z: {
            int16_t sensor_value = 0;
            if (!get_status_value(value, sensor_value)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (flags & StatusConfigFlag::UPDATE_STATUS) {
                switch (param) {
                    case StatusParam::IMU_MAG_X: device_status.imu.mag_x = sensor_value; break;
                    case StatusParam::IMU_MAG_Y: device_status.imu.mag_y = sensor_value; break;
                    case StatusParam::IMU_MAG_Z: device_status.imu.mag_z = sensor_value; break;
                    default: break;
                }
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "Unsupported status param: 0x%04X", static_cast<unsigned>(param));
            return ESP_ERR_NOT_SUPPORTED;
    }
    
    // 如果设置成功且需要同步UI
    if (ret == ESP_OK && (flags & StatusConfigFlag::SYNC_UI) && !ui_sync_callbacks.empty()) {
        for (auto& cb : ui_sync_callbacks) {
            if (cb) cb(param, value);
        }
    }
    
    return ret;
}

esp_err_t m5_corep4_tool::get_status(StatusParam param, StatusValue& value)
{
    switch (param) {
        // ========== 电源相关参数 ==========
        case StatusParam::POWER_LCD:
            value = device_status.power.lcd_power;
            break;
        case StatusParam::POWER_AUDIO_CHIP:
            value = device_status.power.audio_chip_power;
            break;
        case StatusParam::POWER_AUDIO_PA:
            value = device_status.power.audio_pa_power;
            break;
        case StatusParam::POWER_PYB_L3B:
            value = device_status.power.pyb_l3b_power;
            break;
        case StatusParam::POWER_PYB_USBA:
            value = device_status.power.pyb_usba_power;
            break;
        case StatusParam::POWER_GROVE:
            value = device_status.power.grove_power;
            break;
        case StatusParam::POWER_P4USBC:
            value = device_status.power.p4usbc_power;
            break;
        case StatusParam::POWER_LCD_BRIGHTNESS:
            value = device_status.power.lcd_brightness;
            break;
        case StatusParam::POWER_CHARGE_CURRENT:
            value = static_cast<uint8_t>(device_status.power.charge_current);
            break;
        case StatusParam::POWER_PM1_INITIALIZED:
            value = device_status.power.pm1_initialized;
            break;
        case StatusParam::POWER_PYB_INITIALIZED:
            value = device_status.power.pyb_initialized;
            break;
        case StatusParam::POWER_PYB_FW_VERSION:
            value = device_status.power.pyb_firmware_version;
            break;
            
        // ========== 屏幕相关参数 ==========
        case StatusParam::SCREEN_LCD_INITIALIZED:
            value = device_status.screen.lcd_initialized;
            break;
        case StatusParam::SCREEN_LCD_POWER_ON:
            value = device_status.screen.lcd_power_on;
            break;
        case StatusParam::SCREEN_LCD_H_RES:
            value = device_status.screen.lcd_h_res;
            break;
        case StatusParam::SCREEN_LCD_V_RES:
            value = device_status.screen.lcd_v_res;
            break;
        case StatusParam::SCREEN_LCD_BRIGHTNESS:
            value = device_status.screen.lcd_brightness;
            break;
        case StatusParam::SCREEN_ROTATION:
            value = static_cast<uint8_t>(device_status.screen.rotation);
            break;
        case StatusParam::SCREEN_COLOR_DEPTH:
            value = device_status.screen.lcd_color_depth;
            break;
            
        // ========== 系统相关参数 ==========
        case StatusParam::SYS_UPTIME:
            update_uptime();
            value = device_status.uptime_seconds;
            break;
        case StatusParam::SYS_RESET_REASON:
            value = static_cast<uint32_t>(device_status.last_reset_reason);
            break;
            
        // ========== 睡眠/唤醒相关参数 ==========
        case StatusParam::SLEEP_BTN_ENABLE:
            value = device_status.sleep.btn_sleep_enable;
            break;
        case StatusParam::SLEEP_WAKE_RTC:
            value = device_status.sleep.wake_rtc_enable;
            break;
        case StatusParam::SLEEP_WAKE_TOUCH:
            value = device_status.sleep.wake_touch_enable;
            break;
        case StatusParam::SLEEP_WAKE_BMI:
            value = device_status.sleep.wake_bmi_enable;
            break;
        case StatusParam::SLEEP_WAKE_RTC_SEC:
            value = device_status.sleep.wake_rtc_seconds;
            break;

        // ========== SD卡相关参数 ==========
        case StatusParam::SD_DETECTED:
            value = device_status.sdcard.sd_detected;
            break;
        case StatusParam::SD_INITIALIZED:
            value = device_status.sdcard.sd_initialized;
            break;
        case StatusParam::SD_TOTAL_BYTES:
            value = device_status.sdcard.sd_total_bytes;
            break;
        case StatusParam::SD_FREE_BYTES:
            value = device_status.sdcard.sd_free_bytes;
            break;

        // ========== 以太网相关参数 ==========
        case StatusParam::WIFI_INITIALIZED:
            value = device_status.wifi.wifi_initialized;
            break;
        case StatusParam::WIFI_CONNECTED:
            value = device_status.wifi.wifi_connected;
            break;
        case StatusParam::WIFI_RSSI:
            value = device_status.wifi.rssi;
            break;
        case StatusParam::WIFI_CHANNEL:
            value = device_status.wifi.channel;
            break;
        case StatusParam::WIFI_AUTH_MODE:
            value = device_status.wifi.auth_mode;
            break;

        case StatusParam::ETH_INITIALIZED:
            value = device_status.ethernet.ethernet_initialized;
            break;
        case StatusParam::ETH_CONNECTED:
            value = device_status.ethernet.ethernet_connected;
            break;
        case StatusParam::ETH_GOT_IP:
            value = device_status.ethernet.got_ip;
            break;

        // ========== IMU related parameters ==========
        case StatusParam::IMU_BMI270_INITIALIZED:
            value = device_status.imu.bmi270_initialized;
            break;
        case StatusParam::IMU_ACC_ENABLED:
            value = device_status.imu.acc_enabled;
            break;
        case StatusParam::IMU_GYR_ENABLED:
            value = device_status.imu.gyr_enabled;
            break;
        case StatusParam::IMU_MAG_ENABLED:
            value = device_status.imu.mag_enabled;
            break;
        case StatusParam::IMU_ACC_X:
            value = device_status.imu.acc_x;
            break;
        case StatusParam::IMU_ACC_Y:
            value = device_status.imu.acc_y;
            break;
        case StatusParam::IMU_ACC_Z:
            value = device_status.imu.acc_z;
            break;
        case StatusParam::IMU_GYR_X:
            value = device_status.imu.gyr_x;
            break;
        case StatusParam::IMU_GYR_Y:
            value = device_status.imu.gyr_y;
            break;
        case StatusParam::IMU_GYR_Z:
            value = device_status.imu.gyr_z;
            break;
        case StatusParam::IMU_MAG_X:
            value = device_status.imu.mag_x;
            break;
        case StatusParam::IMU_MAG_Y:
            value = device_status.imu.mag_y;
            break;
        case StatusParam::IMU_MAG_Z:
            value = device_status.imu.mag_z;
            break;

        default:
            ESP_LOGW(TAG, "Unsupported status param for get: 0x%04X", static_cast<unsigned>(param));
            return ESP_ERR_NOT_SUPPORTED;
    }
    
    return ESP_OK;
}

esp_err_t m5_corep4_tool::sync_power_status_to_ui(void)
{
    if (ui_sync_callbacks.empty()) {
        ESP_LOGD(TAG, "No UI sync callbacks registered");
        return ESP_OK;
    }
    
    // 同步所有电源相关状态到UI
    auto notify_all = [this](StatusParam param, const StatusValue& value) {
        for (auto& cb : ui_sync_callbacks) {
            if (cb) cb(param, value);
        }
    };
    
    notify_all(StatusParam::POWER_GROVE, device_status.power.grove_power);
    notify_all(StatusParam::POWER_PYB_USBA, device_status.power.pyb_usba_power);
    notify_all(StatusParam::POWER_PYB_L3B, device_status.power.pyb_l3b_power);
    notify_all(StatusParam::POWER_AUDIO_PA, device_status.power.audio_pa_power);
    notify_all(StatusParam::POWER_P4USBC, device_status.power.p4usbc_power);
    notify_all(StatusParam::POWER_AUDIO_CHIP, device_status.power.audio_chip_power);
    notify_all(StatusParam::POWER_LCD_BRIGHTNESS, device_status.power.lcd_brightness);
    
    return ESP_OK;
}
