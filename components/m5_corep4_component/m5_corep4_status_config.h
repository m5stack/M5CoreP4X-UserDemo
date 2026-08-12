/**
 * @file m5_corep4_status_config.h
 * @brief M5 CoreP4 设备状态通用配置系统
 * 
 * 使用枚举ID + std::variant实现类型安全的通用配置接口
 * 
 * @author AI Assistant
 * @date 2025-12-31
 */

#ifndef __M5_COREP4_STATUS_CONFIG_H__
#define __M5_COREP4_STATUS_CONFIG_H__

#include <variant>
#include <functional>
#include <cstdint>
#include "esp_err.h"

// ==================== 状态参数枚举 ====================

/**
 * @brief 设备状态参数ID枚举
 * 
 * 每个枚举值对应device_realtime_status_t中的一个字段
 * 命名规则: 分类_字段名
 */
enum class StatusParam : uint16_t {
    // ========== 电源相关 (0x0100 - 0x01FF) ==========
    POWER_LCD = 0x0100,              ///< LCD电源状态 (bool)
    POWER_AUDIO_CHIP,                ///< 音频芯片电源状态 (bool)
    POWER_AUDIO_PA,                  ///< 音频功放电源状态 (bool)
    POWER_PYB_L3B,                   ///< PYB L3B电源状态 (bool)
    POWER_PYB_USBA,                  ///< PYB USBA电源状态 (bool)
    POWER_GROVE,                     ///< Grove电源状态 (bool)
    POWER_P4USBC,                    ///< P4USBC电源状态 (bool)
    POWER_M5BUS,                     ///< M5BUS电源状态 (bool)
    POWER_LCD_BRIGHTNESS,            ///< LCD亮度 (uint8_t, 0-100)
    POWER_CHARGE_CURRENT,            ///< 充电电流 (charge_current_t)
    POWER_PM1_INITIALIZED,           ///< PM1初始化状态 (bool)
    POWER_PYB_INITIALIZED,           ///< PYB初始化状态 (bool)
    POWER_PYB_FW_VERSION,            ///< PYB固件版本 (uint8_t)

    // ========== 屏幕相关 (0x0200 - 0x02FF) ==========
    SCREEN_LCD_INITIALIZED = 0x0200, ///< LCD初始化状态 (bool)
    SCREEN_LCD_POWER_ON,             ///< LCD电源开启状态 (bool)
    SCREEN_LCD_H_RES,                ///< LCD水平分辨率 (uint16_t)
    SCREEN_LCD_V_RES,                ///< LCD垂直分辨率 (uint16_t)
    SCREEN_LCD_BRIGHTNESS,           ///< LCD当前亮度 (uint8_t)
    SCREEN_ROTATION,                 ///< 屏幕旋转角度 (uint8_t -> lv_disp_rotation_t)
    SCREEN_COLOR_DEPTH,              ///< LCD颜色深度 (uint32_t)

    // ========== 触摸相关 (0x0300 - 0x03FF) ==========
    TOUCH_INITIALIZED = 0x0300,      ///< 触摸初始化状态 (bool)
    TOUCH_POWER_ON,                  ///< 触摸电源开启状态 (bool)
    TOUCH_SLEEP_MODE,                ///< 触摸休眠模式 (bool)
    TOUCH_POINTS,                    ///< 当前触摸点数量 (uint8_t)
    TOUCH_LAST_X,                    ///< 最后触摸X坐标 (uint16_t)
    TOUCH_LAST_Y,                    ///< 最后触摸Y坐标 (uint16_t)

    // ========== WiFi相关 (0x0400 - 0x04FF) ==========
    WIFI_INITIALIZED = 0x0400,       ///< WiFi初始化状态 (bool)
    WIFI_CONNECTED,                  ///< WiFi连接状态 (bool)
    WIFI_RSSI,                       ///< 信号强度 (int8_t)
    WIFI_CHANNEL,                    ///< 信道 (uint8_t)
    WIFI_AUTH_MODE,                  ///< 认证模式 (uint8_t)

    // ========== 以太网相关 (0x0500 - 0x05FF) ==========
    ETH_INITIALIZED = 0x0500,        ///< 以太网初始化状态 (bool)
    ETH_CONNECTED,                   ///< 以太网连接状态 (bool)
    ETH_GOT_IP,                      ///< 获取到IP状态 (bool)

    // ========== SD卡相关 (0x0600 - 0x06FF) ==========
    SD_DETECTED = 0x0600,            ///< SD卡检测状态 (bool)
    SD_INITIALIZED,                  ///< SD卡初始化状态 (bool)
    SD_TOTAL_BYTES,                  ///< SD卡总容量 (uint64_t)
    SD_FREE_BYTES,                   ///< SD卡可用容量 (uint64_t)

    // ========== 音频相关 (0x0700 - 0x07FF) ==========
    AUDIO_ES8311_INITIALIZED = 0x0700, ///< ES8311初始化状态 (bool)
    AUDIO_ES7210_INITIALIZED,        ///< ES7210初始化状态 (bool)
    AUDIO_POWER_ON,                  ///< 音频电源开启状态 (bool)
    AUDIO_VOLUME,                    ///< 音量 (uint8_t, 0-100)
    AUDIO_IS_PLAYING,                ///< 正在播放状态 (bool)

    // ========== IMU相关 (0x0800 - 0x08FF) ==========
    IMU_BMI270_INITIALIZED = 0x0800, ///< BMI270初始化状态 (bool)
    IMU_ACC_ENABLED,                 ///< 加速度计启用状态 (bool)
    IMU_GYR_ENABLED,                 ///< 陀螺仪启用状态 (bool)
    IMU_MAG_ENABLED,                 ///< 磁力计启用状态 (bool)
    IMU_ACC_X,                       ///< 加速度计X轴 (float)
    IMU_ACC_Y,                       ///< 加速度计Y轴 (float)
    IMU_ACC_Z,                       ///< 加速度计Z轴 (float)
    IMU_GYR_X,                       ///< 陀螺仪X轴 (float)
    IMU_GYR_Y,                       ///< 陀螺仪Y轴 (float)
    IMU_GYR_Z,                       ///< 陀螺仪Z轴 (float)
    IMU_MAG_X,                       ///< 磁力计X轴 (int16_t)
    IMU_MAG_Y,                       ///< 磁力计Y轴 (int16_t)
    IMU_MAG_Z,                       ///< 磁力计Z轴 (int16_t)

    // ========== USB相关 (0x0900 - 0x09FF) ==========
    USB_HOST_INITIALIZED = 0x0900,   ///< USB主机初始化状态 (bool)
    USB_MOUSE_CONNECTED,             ///< 鼠标连接状态 (bool)

    // ========== RTC相关 (0x0A00 - 0x0AFF) ==========
    RTC_INITIALIZED = 0x0A00,        ///< RTC初始化状态 (bool)
    RTC_TIMER_IRQ_ENABLED,           ///< 定时器中断启用状态 (bool)

    // ========== IR相关 (0x0B00 - 0x0BFF) ==========
    IR_INITIALIZED = 0x0B00,         ///< IR初始化状态 (bool)
    IR_CURRENT_FORMAT,               ///< 当前IR格式 (uint8_t -> ir_format_t)

    // ========== 睡眠/唤醒相关 (0x0C00 - 0x0CFF) ==========
    SLEEP_BTN_ENABLE = 0x0C00,       ///< 单击休眠使能 (bool) - 默认 true（允许单击进入休眠）
    SLEEP_WAKE_RTC,                  ///< RTC定时器唤醒使能 (bool) - 默认 false
    SLEEP_WAKE_TOUCH,                ///< 触摸唤醒使能 (bool) - 默认 false (ESP32-P4 GPIO1)
    SLEEP_WAKE_BMI,                  ///< BMI运动唤醒使能 (bool) - 默认 false (PM1 GPIO0)
    SLEEP_WAKE_RTC_SEC,              ///< RTC唤醒时间秒数 (uint32_t) - 默认 60

    // ========== 系统相关 (0x0000 - 0x00FF) ==========
    SYS_UPTIME = 0x0000,             ///< 设备运行时间秒 (uint64_t)
    SYS_RESET_REASON,                ///< 上次重启原因 (uint32_t -> esp_reset_reason_t)
};

// ==================== 状态值类型 ====================

/**
 * @brief 状态值联合类型
 * 
 * 支持所有可能的状态值类型
 */
using StatusValue = std::variant<
    bool,                   // 布尔类型
    int8_t,                 // 有符号8位
    uint8_t,                // 无符号8位
    int16_t,                // 有符号16位
    uint16_t,               // 无符号16位
    int32_t,                // 有符号32位
    uint32_t,               // 无符号32位
    int64_t,                // 有符号64位
    uint64_t,               // 无符号64位
    float                   // 浮点数
>;

// ==================== 配置标志 ====================

/**
 * @brief 状态配置标志
 */
enum class StatusConfigFlag : uint8_t {
    NONE             = 0x00,   ///< 无特殊标志
    UPDATE_STATUS    = 0x01,   ///< 仅更新状态结构体
    APPLY_HARDWARE   = 0x02,   ///< 应用到硬件
    SYNC_UI          = 0x04,   ///< 同步到UI
    ALL              = 0x07    ///< 全部操作
};

// 位运算支持
inline StatusConfigFlag operator|(StatusConfigFlag a, StatusConfigFlag b) {
    return static_cast<StatusConfigFlag>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline bool operator&(StatusConfigFlag a, StatusConfigFlag b) {
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}

// ==================== UI同步回调类型 ====================

/**
 * @brief UI同步回调函数类型
 * 
 * @param param 状态参数ID
 * @param value 状态值
 */
using StatusUISyncCallback = std::function<void(StatusParam param, const StatusValue& value)>;

// ==================== 辅助宏 ====================

/**
 * @brief 获取StatusValue中的值（带类型检查）
 */
template<typename T>
inline bool get_status_value(const StatusValue& value, T& out) {
    if (auto* p = std::get_if<T>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

/**
 * @brief 获取参数分类（高8位）
 */
inline uint8_t get_param_category(StatusParam param) {
    return static_cast<uint16_t>(param) >> 8;
}

/**
 * @brief 检查是否为电源相关参数
 */
inline bool is_power_param(StatusParam param) {
    return get_param_category(param) == 0x01;
}

/**
 * @brief 检查是否为屏幕相关参数
 */
inline bool is_screen_param(StatusParam param) {
    return get_param_category(param) == 0x02;
}

/**
 * @brief 检查是否为睡眠/唤醒相关参数
 */
inline bool is_sleep_param(StatusParam param) {
    return get_param_category(param) == 0x0C;
}

/**
 * @brief 检查是否为SD卡相关参数
 */
inline bool is_sd_param(StatusParam param) {
    return get_param_category(param) == 0x06;
}

/**
 * @brief 检查是否为以太网相关参数
 */
inline bool is_eth_param(StatusParam param) {
    return get_param_category(param) == 0x05;
}

#endif // __M5_COREP4_STATUS_CONFIG_H__
