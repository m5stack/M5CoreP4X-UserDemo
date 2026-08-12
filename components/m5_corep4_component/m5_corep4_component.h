#ifndef __M5_COREP4_COMPONENT_H__
#define __M5_COREP4_COMPONENT_H__


#include <string.h>
#include <math.h>
#include <vector>
#include <ctime>    // struct tm

// 状态配置系统
#include "m5_corep4_status_config.h"

#include "esp_sleep.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"

#include "m5_corep4_pinmap.h"

#include "otool_toolbox.h"
#include "bmi270_tools.h"
#include "M5PM1.h"
#include "M5IOE1.h"

//usb
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_mouse.h"

// lcd
#include "esp_lcd_st7102.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "hal/lcd_types.h"
#include "esp_ldo_regulator.h"
// IDF v6 使用 LCD_COLOR_FMT_* 表示 LCD 像素格式。
#ifndef LCD_COLOR_PIXEL_FORMAT_RGB565
#define LCD_COLOR_PIXEL_FORMAT_RGB565 LCD_COLOR_FMT_RGB565
#endif
#ifndef LCD_COLOR_PIXEL_FORMAT_RGB888
#define LCD_COLOR_PIXEL_FORMAT_RGB888 LCD_COLOR_FMT_RGB888
#endif

static constexpr lcd_color_format_t LCD_MIPI_DSI_COLOR_DEPTH = LCD_COLOR_PIXEL_FORMAT_RGB565;  // RGB565, RGB888
#define LCD_MIPI_DSI_PHY_PWR_LDO_CHAN 3  // 原理图 ESP_LDO3
#define LCD_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define LCD_MIPI_DSI_LANE_NUM          2    // 2 data lanes
#define LCD_MIPI_DSI_LCD_H_RES         480  // h_size
#define LCD_MIPI_DSI_LCD_V_RES         480  // v_size
#define LCD_MIPI_DSI_LCD_HSYNC         2    // hsync_pulse_width
#define LCD_MIPI_DSI_LCD_HBP           40   // hsync_back_porch
#define LCD_MIPI_DSI_LCD_HFP           40   // hsync_front_porch
#define LCD_MIPI_DSI_LCD_VSYNC         4    // vsync_pulse_width
#define LCD_MIPI_DSI_LCD_VBP           8    // vsync_back_porch
#define LCD_MIPI_DSI_LCD_VFP           200  // vsync_front_porch
#define LCD_MIPI_DSI_DPI_CLK_MHZ       24   // dpi_clock_freq_mhz
#define LCD_MIPI_DSI_LANE_BITRATE_MBPS 700  // 700 Mbps
#define LCD_COLOR_SPACE                 LCD_RGB_ELEMENT_ORDER_RGB
#define LCD_BITS_PER_PIXEL              16
#define LCD_RST_PIN_NUM                 GPIO_NUM_NC

// touch
#include "esp_lcd_touch_cst3xx.h"


// lvgl
#include "esp_lvgl_port.h"
#include "lv_demos.h"
#include "ui.h"
#include "ui_events.h"

// ethernet
#include "third_party_component/p4_ethernet_init/p4_ethernet_init.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"

// wifi
#include "esp_wifi_remote.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "esp_hosted_transport_config.h"
#ifdef __cplusplus
}
#endif

// web
#include "third_party_component/uvc_web_server/uvc_web_server.h"
#include "third_party_component/uvc_web_server/uvc_app_task.h"
#include "esp_eth_netif_glue.h"

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_BITS (WIFI_CONNECTED_BIT | WIFI_FAIL_BIT)
#define WIFI_REMOTE_CONNECTED_BIT BIT2
#define WIFI_REMOTE_FAIL_BIT      BIT3
#define WIFI_REMOTE_BITS (WIFI_REMOTE_CONNECTED_BIT | WIFI_REMOTE_FAIL_BIT)

/* Ethernet event group bits */
#define ETH_CONNECTED_BIT BIT0
#define ETH_DISCONNECTED_BIT BIT1
#define ETH_GOT_IP_BIT BIT2
#define ETH_FAIL_BIT BIT3
#define ETH_BITS (ETH_CONNECTED_BIT | ETH_DISCONNECTED_BIT | ETH_GOT_IP_BIT | ETH_FAIL_BIT)

/**
 * @brief USB鼠标坐标数据结构
 */
typedef struct {
    int16_t x;          ///< X坐标 (相对位移)
    int16_t y;          ///< Y坐标 (相对位移)
    bool left_button;   ///< 左键状态
    bool right_button;  ///< 右键状态
    bool middle_button; ///< 中键状态
    int8_t wheel;       ///< 滚轮状态
    bool connected;     ///< 鼠标连接状态
} usb_mouse_data_t;

/**
 * @brief USB键盘最近一次Boot协议报告缓存
 */
typedef struct {
    uint8_t modifier;        ///< 修饰键位图
    uint8_t keycodes[6];     ///< 当前按下的最多6个普通键
    uint8_t last_key_code;   ///< 最近一个非零键码
    char last_key_name[24];  ///< 最近按键名称
    uint32_t sequence;       ///< 输入报告序号
    bool connected;          ///< 键盘连接状态
} usb_keyboard_data_t;

// ==================== 设备实时状态结构体 ====================

/**
 * @brief 充电电流枚举
 */
typedef enum {
    CHARGE_CURRENT_180MA = 0,   ///< 180mA 充电电流
    CHARGE_CURRENT_650MA = 1,   ///< 650mA 充电电流
    CHARGE_CURRENT_OFF = 2,     ///< 关闭充电
} charge_current_t;

/**
 * @brief 电源状态结构
 */
typedef struct {
    bool lcd_power;             ///< LCD电源状态
    bool audio_chip_power;      ///< 音频芯片电源状态
    bool audio_pa_power;        ///< 音频功放电源状态
    bool pyb_l3b_power;         ///< PYB L3B电源状态（外设主供电轨，touch/LCD/audio 等挂在其后）
    bool pyb_usba_power;        ///< PYB USBA电源状态
    bool grove_power;           ///< Grove电源状态
    bool p4usbc_power;          ///< P4USBC电源状态
    bool m5bus_power;           ///< M5BUS电源状态 (PM1 BOOST_EN)
    uint8_t lcd_brightness;     ///< LCD亮度 (0-100)
    charge_current_t charge_current; ///< 充电电流设置
    bool pm1_initialized;       ///< PM1初始化状态
    bool pyb_initialized;       ///< PYB初始化状态
    uint8_t pyb_firmware_version; ///< PYB固件版本
} power_status_t;

/**
 * @brief 屏幕状态结构
 */
typedef struct {
    bool lcd_initialized;       ///< LCD初始化状态
    bool lcd_power_on;          ///< LCD电源开启状态
    uint16_t lcd_h_res;         ///< LCD水平分辨率
    uint16_t lcd_v_res;         ///< LCD垂直分辨率
    uint8_t lcd_brightness;     ///< LCD当前亮度
    lv_disp_rotation_t rotation; ///< 屏幕旋转角度
    uint32_t lcd_color_depth;   ///< LCD颜色深度
} screen_status_t;

/**
 * @brief 触摸状态结构
 */
typedef struct {
    bool touch_initialized;     ///< 触摸初始化状态
    bool touch_power_on;        ///< 触摸电源开启状态
    bool touch_sleep_mode;      ///< 触摸休眠模式
    uint8_t touch_points;       ///< 当前触摸点数量 (0-5)
    uint16_t last_touch_x;      ///< 最后触摸X坐标
    uint16_t last_touch_y;      ///< 最后触摸Y坐标
} touch_status_t;

/**
 * @brief WiFi状态结构
 */
typedef struct {
    bool wifi_initialized;      ///< WiFi初始化状态
    bool wifi_connected;        ///< WiFi连接状态
    char ssid[33];              ///< 当前连接的SSID
    int8_t rssi;                ///< 信号强度
    uint8_t channel;            ///< 信道
    uint8_t auth_mode;          ///< 认证模式
    uint8_t wifi_mac_addr[6];   ///< WiFi MAC地址
} wifi_status_t;

/**
 * @brief 以太网状态结构
 */
typedef struct {
    bool ethernet_initialized;  ///< 以太网初始化状态
    bool ethernet_connected;    ///< 以太网连接状态
    bool got_ip;                ///< 获取到IP状态
    uint8_t eth_mac_addr[6];    ///< 以太网MAC地址
    char ip_address[16];        ///< IP地址
    char netmask[16];           ///< 子网掩码
    char gateway[16];           ///< 网关
} ethernet_status_t;

/**
 * @brief SD卡状态结构
 */
typedef struct {
    bool sd_detected;           ///< SD卡检测状态
    bool sd_initialized;        ///< SD卡初始化状态
    uint64_t sd_total_bytes;    ///< SD卡总容量
    uint64_t sd_free_bytes;     ///< SD卡可用容量
    char sd_card_type[16];      ///< SD卡类型 (SDSC/SDHC/SDXC)
} sd_status_t;

/**
 * @brief 音频状态结构
 */
typedef struct {
    bool es8311_initialized;    ///< ES8311编解码器初始化状态
    bool es7210_initialized;    ///< ES7210编解码器初始化状态
    bool audio_power_on;        ///< 音频电源开启状态
    uint8_t volume;             ///< 音量 (0-100)
    bool is_playing;            ///< 正在播放状态
} audio_status_t;

/**
 * @brief IMU状态结构
 */
typedef struct {
    bool bmi270_initialized;    ///< BMI270初始化状态
    bool acc_enabled;           ///< 加速度计启用状态
    bool gyr_enabled;           ///< 陀螺仪启用状态
    bool mag_enabled;           ///< 磁力计启用状态
    float acc_x;                ///< 加速度X轴 (g)
    float acc_y;                ///< 加速度Y轴 (g)
    float acc_z;                ///< 加速度Z轴 (g)
    float gyr_x;                ///< 陀螺仪X轴 (dps)
    float gyr_y;                ///< 陀螺仪Y轴 (dps)
    float gyr_z;                ///< 陀螺仪Z轴 (dps)
    int16_t mag_x;              ///< 磁力计X轴
    int16_t mag_y;              ///< 磁力计Y轴
    int16_t mag_z;              ///< 磁力计Z轴
} imu_status_t;

/**
 * @brief USB鼠标状态结构
 */
typedef struct {
    bool usb_host_initialized;  ///< USB主机初始化状态
    bool mouse_connected;       ///< 鼠标连接状态
    int16_t mouse_x;            ///< 鼠标X坐标
    int16_t mouse_y;            ///< 鼠标Y坐标
    bool left_button;           ///< 左键状态
    bool right_button;          ///< 右键状态
    bool middle_button;         ///< 中键状态
} usb_mouse_status_t;

/**
 * @brief RTC状态结构
 */
typedef struct {
    bool rtc_initialized;       ///< RTC初始化状态
    struct tm current_time;     ///< 当前时间
    bool timer_irq_enabled;     ///< 定时器中断启用状态
} rtc_status_t;

/**
 * @brief IR状态结构
 */
typedef struct {
    bool ir_initialized;        ///< IR初始化状态
    ir_format_t current_format; ///< 当前IR格式
} ir_status_t;

/**
 * @brief 睡眠/唤醒状态结构
 */
typedef struct {
    bool btn_sleep_enable;      ///< 单击休眠使能 (true=允许单击进入休眠)
    bool wake_rtc_enable;       ///< RTC定时器唤醒使能
    bool wake_touch_enable;     ///< 触摸唤醒使能（P4 GPIO1 只是一根 INT 线，触摸芯片本体在 PYB/L3B 侧供电）
    bool wake_bmi_enable;       ///< BMI运动唤醒使能 (PM1 GPIO0)
    uint32_t wake_rtc_seconds;  ///< RTC唤醒时间秒数
} sleep_status_t;

/**
 * @brief 设备实时状态结构体 (可扩展)
 *
 * 此结构体用于记录M5CoreP4设备的全部实时状态
 * 设计为可扩展，新增功能时只需在相应子结构中添加字段
 */
typedef struct {
    uint32_t magic;             ///< 魔数，用于验证结构体有效性 (0x50344554 = "P4ET")
    uint32_t version;           ///< 结构体版本号，便于兼容性管理
    uint64_t uptime_seconds;    ///< 设备运行时间(秒)
    esp_reset_reason_t last_reset_reason; ///< 上次重启原因

    power_status_t power;       ///< 电源状态
    screen_status_t screen;     ///< 屏幕状态
    touch_status_t touch;       ///< 触摸状态
    wifi_status_t wifi;         ///< WiFi状态
    ethernet_status_t ethernet; ///< 以太网状态
    sd_status_t sdcard;         ///< SD卡状态
    audio_status_t audio;       ///< 音频状态
    imu_status_t imu;           ///< IMU状态
    usb_mouse_status_t usb_mouse; ///< USB鼠标状态
    rtc_status_t rtc;           ///< RTC状态
    ir_status_t ir;             ///< IR状态
    sleep_status_t sleep;       ///< 睡眠/唤醒状态

    // 保留字段用于未来扩展
    uint8_t reserved[64];       ///< 保留字段
} device_realtime_status_t;

// 魔数和版本号定义
#define DEVICE_STATUS_MAGIC     0x50344554UL  // "P4ET"
#define DEVICE_STATUS_VERSION   0x00010001UL  // v1.0.0.1

/**
 * @brief m5_corep4_tool
 * 
 */
class m5_corep4_tool 
{
public:
    otool_toolbox otool_tools;      ///< 工具箱子模块
    M5IOE1 pyb;       ///< PY32 IO Expander 对象 (M5IOE1)
    bmi270_tools bmi270;            ///< BMI270 6轴传感器对象
    rx8130_tools rtc;            ///< RX8130 RTC对象
    M5PM1 pm1;                      ///< PM1 电源管理对象
    
    /**
     * @brief 构造函数
     * 
     */
    m5_corep4_tool();

    /**
     * @brief 析构函数
     * 
     */
    ~m5_corep4_tool();

    /**
     * @brief 初始化函数
     *
     * @return esp_err_t 返回初始化结果
     */
    esp_err_t init();

    // ==================== IMU shared service ====================

    /**
     * @brief Initialize BMI270, optional BMM150, and shared data service.
     * @param configure_motion_interrupt 是否配置 any-motion 中断（含 500ms 稳定延迟）。
     *        仅睡眠唤醒场景需要；常规界面（如 IMU 指南针）传 false 以避免无用开销。
     */
    esp_err_t imu_init(bool enable_magnetometer = true,
                       bmi270_tools::bmi270_mode_t mode = bmi270_tools::MODE_BASE,
                       bool configure_motion_interrupt = false);

    /**
     * @brief Configure BMI270 any-motion detection and INT1 mapping.
     */
    esp_err_t imu_configure_motion_interrupt(void);

    /**
     * @brief Start periodic IMU sampling into device_status/latest cache.
     */
    esp_err_t imu_start_service(uint32_t period_ms = 50);

    /**
     * @brief Stop periodic IMU sampling task.
     */
    esp_err_t imu_stop_service(void);

    /**
     * @brief 睡前停止IMU任务，并收敛BMI270/BMM150与PM1 GPIO0状态。
     */
    esp_err_t imu_prepare_for_sleep(bool keep_motion_wake = false);

    /**
     * @brief Read the sensor directly and refresh the shared cache/status.
     */
    esp_err_t imu_read_sensor_data(bmi270_tools::sensor_data_t& data, bool update_status = true);

    /**
     * @brief Get the latest cached IMU sample without touching I2C.
     */
    esp_err_t imu_get_latest_sensor_data(bmi270_tools::sensor_data_t& data);

    /**
     * @brief Clear BMI270 interrupt status.
     */
    esp_err_t imu_clear_interrupt(void);

    bool imu_is_initialized(void) const;

    // ==================== 设备状态相关方法 ====================

    /**
     * @brief 初始化设备实时状态结构体
     *
     * 在init()函数开始时调用，初始化设备状态结构体
     *
     * @return esp_err_t ESP_OK 成功, ESP_FAIL 失败
     */
    esp_err_t init_device_status(void);

    /**
     * @brief 获取设备实时状态结构体指针
     *
     * @return device_realtime_status_t* 设备状态结构体指针
     */
    device_realtime_status_t* get_device_status(void);

    /**
     * @brief 打印设备实时状态信息
     *
     * @param level 日志级别 (ESP_LOG_INFO, ESP_LOG_DEBUG, etc.)
     */
    void print_device_status(esp_log_level_t level = ESP_LOG_INFO);

    /**
     * @brief 更新设备运行时间
     *
     * @return esp_err_t ESP_OK 成功
     */
    esp_err_t update_uptime(void);

    // ==================== 通用状态配置方法 ====================

    /**
     * @brief 通用设备状态设置接口
     * 
     * 根据参数ID设置相应的状态值，自动执行硬件操作并更新状态结构体
     * 
     * @param param 状态参数ID (StatusParam枚举)
     * @param value 状态值 (StatusValue类型)
     * @param flags 配置标志 (StatusConfigFlag枚举，默认ALL)
     * @return esp_err_t ESP_OK 成功, ESP_ERR_INVALID_ARG 参数类型不匹配, ESP_ERR_NOT_SUPPORTED 不支持的参数
     */
    esp_err_t set_status(StatusParam param, const StatusValue& value, 
                        StatusConfigFlag flags = StatusConfigFlag::ALL);

    /**
     * @brief 通用设备状态获取接口
     * 
     * 根据参数ID获取相应的状态值
     * 
     * @param param 状态参数ID (StatusParam枚举)
     * @param[out] value 输出状态值 (StatusValue类型)
     * @return esp_err_t ESP_OK 成功, ESP_ERR_NOT_SUPPORTED 不支持的参数
     */
    esp_err_t get_status(StatusParam param, StatusValue& value);

    /**
     * @brief 注册UI同步回调函数
     * 
     * 当状态发生变化时，会调用此回调函数同步UI显示
     * 
     * @param callback UI同步回调函数
     */
    void register_ui_sync_callback(StatusUISyncCallback callback);

    /**
     * @brief 同步所有电源状态到UI
     * 
     * 读取当前所有电源状态，调用UI同步回调更新显示
     * 
     * @return esp_err_t ESP_OK 成功
     */
    esp_err_t sync_power_status_to_ui(void);

    /**
     * @brief 初始化GPIO引脚
     * 
     * @return esp_err_t 返回初始化结果
     */
    esp_err_t gpio_init();

    /**
     * @brief 将 CoreP4 M5BUS 中可用 GPIO 引脚统一配置为输出并设置电平
     *
     * @param level true 输出高电平，false 输出低电平
     * @return esp_err_t ESP_OK 表示成功
     */
    esp_err_t set_m5bus_gpio_output_level(bool level);

    /**
     * @brief 进入深度休眠
     * 
     */
    esp_err_t enter_deep_sleep();

    /**
     * @brief 休眠前处理
     * 
     * 在进入深度休眠之前执行必要的准备工作，包括：
     * - 关闭屏幕背光
     * - 关闭触摸屏（进入休眠模式）
    * - 按唤醒配置收敛BMI270/BMM150：需要BMI唤醒时保留ACC+INT1，不需要时完全关闭
     * - 禁用RTC中断输出
     * - 配置PM1和PYB I2C休眠参数
     * 
     * @return esp_err_t ESP_OK 成功, ESP_FAIL 失败
     */
    esp_err_t sleep_before_work();

    /**
     * @brief 测试和诊断PYB IO引脚功能
     * 
     */
    esp_err_t test_pyb_io_diagnostics();

    /**
     * @brief RTC时间设置测试函数
     * 
     * @param rtc RX8130 RTC对象引用
     * @param timestamp 时间戳格式: YYYYMMDDHHmmss (例如: 20251118142600 表示 2025-11-18 14:26:00)
     */
    void test_rx8130_time_setting(rx8130_tools& rtc, uint64_t timestamp);

    /**
     * @brief RX8130 RTC定时唤醒测试
     * 
     * 参数可以是秒数或时间戳:
     * - 当 >= 19000000000000 时识别为时间戳 (格式: YYYYMMDDHHmmss)
     * - 当 < 19000000000000 时识别为秒数
     * 
     * @param rtc RX8130 RTC对象引用
     * @param wake_time 唤醒时间（秒数或时间戳）
     */
    void test_rx8130_timed_wakeup(rx8130_tools& rtc, uint64_t wake_time);

    /**
     * @brief 低功耗测试函数
     * 
        * 关闭SD卡、BMI270、RTC中断、触摸屏等外设，配置PM1和PYB I2C休眠参数后进入ESP32深度休眠
     * 
     * 执行步骤:
        * 1. 卸载SD卡并释放SD GPIO/SD_DECT
        * 2. 关闭BMI270传感器（写PWR_CTRL寄存器进入休眠）
        * 3. 禁用RTC中断输出并清除中断标志
        * 4. 初始化触摸屏后发送CST3XX深度休眠命令
        * 5. 配置PM1 I2C休眠时间和PYB I2C配置
        * 6. 调用esp_deep_sleep_start()进入深度休眠
     * 
     * @note 此函数调用后不会返回，设备将进入深度休眠状态
     */
    void test_low_power_sleep(void);

    /**
     * @brief 启动红外发送任务
     * 
     * 创建一个任务，循环发送指定格式的红外遥控信号
     * 
     * @param format 红外格式 (ir_format_t::NEC 或 ir_format_t::RC5)
     * @return esp_err_t ESP_OK on success
     */
    esp_err_t start_ir_send_task(ir_format_t format);

    /**
     * @brief 初始化 显示屏
     * 
     */
    esp_err_t lcd_init(void);

    /**
     * @brief LCD清屏函数
     * 
     * 将整个屏幕填充为指定颜色，自动根据LCD_MIPI_DSI_COLOR_DEPTH调整
     * 
     * @param panel_handle LCD面板句柄
     * @param h_res 屏幕水平分辨率
     * @param v_res 屏幕垂直分辨率
     * @param is_white true=白屏，false=黑屏
     */
    void lcd_clear(esp_lcd_panel_handle_t panel_handle,
                   uint16_t h_res, uint16_t v_res,
                   bool is_white = false);

    /**
     * @brief LCD清屏函数 - 使用内部LCD句柄的简化版本
     * 
     * 自动使用对象内部存储的LCD面板句柄清屏
     * 
     * @param is_white true=白屏，false=黑屏
     * @return esp_err_t ESP_OK成功，ESP_ERR_INVALID_STATE如果LCD未初始化
     */
    esp_err_t lcd_clear(bool is_white = false);

    /**
     * @brief LCD绘制彩虹条纹 - 14色渐变铺满全屏
     * 
     * 绘制水平渐变条纹，包含14种颜色从黑色(0,0,0)到白色(255,255,255)铺满整个屏幕
     * 
     * @param panel_handle LCD面板句柄
     * @param h_res 屏幕水平分辨率
     * @param v_res 屏幕垂直分辨率
     * @param show_fullscreen_colors 是否在显示条纹后全屏显示每种颜色（并显示RGB888值）
     */
    void lcd_draw_rainbow_stripes(esp_lcd_panel_handle_t panel_handle,
                                  uint16_t h_res, uint16_t v_res,
                                  bool show_fullscreen_colors = false);

    /**
     * @brief 获取LCD面板句柄
     * 
     * @return esp_lcd_panel_handle_t LCD面板句柄，如果未初始化则返回nullptr
     */
    esp_lcd_panel_handle_t get_lcd_panel_handle(void);

    /**
     * @brief 获取MIPI DBI I/O句柄
     * 
     * @return esp_lcd_panel_io_handle_t MIPI DBI I/O句柄，如果未初始化则返回nullptr  
     */
    esp_lcd_panel_io_handle_t get_mipi_dbi_io_handle(void);

    /**
     * @brief 获取LVGL显示对象
     * 
     * @return lv_display_t* LVGL显示对象指针，如果未初始化则返回nullptr
     */
    lv_display_t* get_lvgl_display(void);

    /**
     * @brief 获取LVGL触摸输入设备
     * 
     * @return lv_indev_t* LVGL触摸输入设备指针，如果未初始化则返回nullptr
     */
    lv_indev_t* get_lvgl_touch_indev(void);

    /**
     * @brief 获取LVGL鼠标输入设备
     * 
     * @return lv_indev_t* LVGL鼠标输入设备指针，如果未初始化则返回nullptr
     */
    lv_indev_t* get_lvgl_mouse_indev(void);

    /**
     * @brief 获取LVGL鼠标光标对象
     * 
     * @return lv_obj_t* LVGL鼠标光标对象指针，如果未初始化则返回nullptr
     */
    lv_obj_t* get_lvgl_mouse_cursor(void);

    /**
     * @brief 创建或重新创建LVGL鼠标光标
     * 
     * @return esp_err_t 创建结果
     */
    esp_err_t create_lvgl_mouse_cursor(void);

    /**
     * @brief 动态创建LVGL鼠标输入设备
     * 
     * 在USB鼠标连接后动态创建鼠标输入设备，避免在LVGL初始化时强制依赖鼠标
     * 
     * @return esp_err_t ESP_OK 成功，ESP_FAIL 失败
     */
    esp_err_t create_lvgl_mouse_input_device(void);

    /**
     * @brief 获取LVGL触摸位置指示器对象
     * 
     * @return lv_obj_t* LVGL触摸位置指示器对象指针，如果未初始化则返回nullptr
     */
    lv_obj_t* get_lvgl_touch_indicator(void);

    /**
     * @brief 设置LVGL触摸位置指示器是否启用
     *
     * @param enabled true=允许显示，false=强制隐藏
     */
    void set_lvgl_touch_indicator_enabled(bool enabled);

    /**
     * @brief 设置并保存LVGL触摸位置指示器偏好
     *
     * @param enabled true=启用并写入NVS，false=关闭并写入NVS
     * @return esp_err_t ESP_OK 成功，其他值表示NVS写入失败
     */
    esp_err_t set_lvgl_touch_indicator_preference(bool enabled);

    /**
     * @brief 获取LVGL触摸位置指示器启用状态
     *
     * @return true 已启用
     * @return false 已关闭
     */
    bool get_lvgl_touch_indicator_enabled(void) const;

    /**
     * @brief 获取启动提示音偏好
     *
     * @return true 启动时播放提示音
     * @return false 启动时不播放提示音
     */
    bool get_startup_prompt_audio_enabled(void) const;

    /**
     * @brief 设置并保存启动提示音偏好
     *
     * @param enabled true=启动时播放提示音，false=启动时不播放提示音
     * @return esp_err_t ESP_OK 成功，其他值表示NVS写入失败
     */
    esp_err_t set_startup_prompt_audio_enabled(bool enabled);

    /**
     * @brief Get startup prompt sound delay in milliseconds.
     *
     * @return uint32_t Delay before playing the startup prompt sound.
     */
    uint32_t get_startup_prompt_audio_delay_ms(void) const;

    /**
     * @brief Set and save startup prompt sound delay.
     *
     * @param delay_ms Delay before playing the startup prompt sound, in milliseconds.
     * @return esp_err_t ESP_OK on success.
     */
    esp_err_t set_startup_prompt_audio_delay_ms(uint32_t delay_ms);

    /**
     * @brief 创建或重新创建LVGL触摸位置指示器（蓝色圆圈）
     * 
     * 创建一个蓝色的圆形指示器，用于显示触摸位置
     * 
     * @return esp_err_t ESP_OK 创建成功，ESP_FAIL 创建失败
     */
    esp_err_t create_lvgl_touch_indicator(void);

    /**
     * @brief 更新触摸位置指示器的位置
     * 
     * 根据触摸坐标更新蓝色触摸指示器的位置，并显示或隐藏指示器
     * 
     * @param x 触摸点X坐标
     * @param y 触摸点Y坐标
     * @param pressed 是否有触摸按下（true=显示指示器，false=隐藏指示器）
     * @return esp_err_t ESP_OK 更新成功，ESP_FAIL 更新失败
     */
    esp_err_t update_touch_indicator(int16_t x, int16_t y, bool pressed);

    /**
     * @brief 获取触摸屏I/O句柄
     * 
     * @return esp_lcd_panel_io_handle_t 触摸屏I/O句柄，如果未初始化则返回nullptr
     */
    esp_lcd_panel_io_handle_t get_touch_io_handle(void);

    /**
     * @brief 获取触摸屏设备句柄
     * 
     * @return esp_lcd_touch_handle_t 触摸屏设备句柄，如果未初始化则返回nullptr
     */
    esp_lcd_touch_handle_t get_touch_handle(void);

    /**
     * @brief 初始化 触摸屏
     * 
     */
    esp_err_t touch_init(void);

    /**
     * @brief 触摸测试演示 - 支持多点触摸轨迹显示
     * 
     * 此函数会在LCD屏幕上显示触摸轨迹，支持最多5个触摸点同时显示
     * 每个触摸点使用不同颜色，触摸时会绘制轨迹圆点
     * 
     * @return esp_err_t ESP_OK 成功，ESP_FAIL 失败
     */
    esp_err_t touch_test_demo(void);

    /**
     * @brief 初始化 LVGL
     * 
     */
    esp_err_t lvgl_init(void);

    /**
     * @brief 控制LCD电源开关
     * 
     * 通过PY32 IO扩展器控制LCD电源使能引脚(Pin_PYB_LCD_PWR_EN)
     * 
     * @param on true=开启LCD电源, false=关闭LCD电源
     * @return esp_err_t ESP_OK 成功
     */
    esp_err_t set_lcd_power(bool on);

    /**
     * @brief 执行LCD复位序列
     * 
     * 通过PY32 IO扩展器控制LCD复位引脚(Pin_PYB_LCD_RST)
     * 执行标准复位序列：拉低100ms -> 拉高，确保LCD正确复位
     * 
     * @return esp_err_t ESP_OK 成功
     */
    esp_err_t reset_lcd();

    /**
     * @brief 执行触摸屏复位序列
     * 
     * 通过PY32 IO扩展器控制触摸复位引脚(Pin_PYB_TP_RST)
     * 执行标准复位序列：拉低100ms -> 拉高，确保触摸IC正确复位
     * 
     * @return esp_err_t ESP_OK 成功
     */
    esp_err_t reset_touch();

    /**
     * @brief 触摸屏进入深度休眠模式
     * 
     * 向 CST3XX 触摸IC发送深度休眠命令 (0xD105)
     * 进入休眠后需要通过硬件复位或调用 touch_resume() 来唤醒
     * 
     * @return esp_err_t ESP_OK 成功, ESP_ERR_INVALID_STATE 触摸句柄为空
     */
    esp_err_t touch_enter_sleep(void);

    /**
     * @brief 触摸屏从休眠中恢复
     * 
     * 执行硬件复位并设置为正常工作模式
     * 
     * @return esp_err_t ESP_OK 成功, ESP_ERR_INVALID_STATE 触摸句柄为空
     */
    esp_err_t touch_resume(void);

    /**
     * @brief 控制音频功放电源开关
     * 
     * 通过PY32 IO扩展器控制音频功放电源使能引脚(Pin_PYB_AU_PWR_EN)
     * 
     * @param on true=开启音频功放电源, false=关闭音频功放电源
     * @return esp_err_t ESP_OK 成功
     */
    esp_err_t set_audio_chip_power(bool on);

    /**
     * @brief 控制音频功率放大器电源开关
     * 
     * 通过PY32 IO扩展器控制音频功率放大器电源使能引脚(Pin_PYB_SPK_PWR_EN)
     * 
     * @param on true=开启音频功率放大器电源, false=关闭音频功率放大器电源
     * @return esp_err_t ESP_OK 成功
     */
    esp_err_t set_audio_pa_power(bool on);

    /**
     * @brief 控制PYB L3B电源开关
     * 
     * 通过PY32 IO扩展器控制PYB L3B电源使能引脚(Pin_PYB_L3B_EN)
     * 
     * @param on true=开启PYB L3B电源, false=关闭PYB L3B电源
     * @return esp_err_t ESP_OK 成功
     */
    esp_err_t set_pyb_l3b_power(bool on);

    /**
     * @brief 控制PYB USBA电源开关
     * 
     * 通过PY32 IO扩展器控制PYB USBA电源使能引脚(Pin_PYB_USBA_OUT_EN)
     * 
     * @param on true=开启PYB USBA电源, false=关闭PYB USBA电源
     * @return esp_err_t ESP_OK 成功
     */
    esp_err_t set_pyb_usba_power(bool on);

    /**
     * @brief 控制PYB Grove电源开关
     * 
     * 通过PY32 IO扩展器控制PYB Grove电源使能引脚(Pin_PYB_GROVE_PWR_EN)
     * 
     * @param on true=开启PYB Grove电源, false=关闭PYB Grove电源
     * @return esp_err_t ESP_OK 成功
     */
    esp_err_t set_grove_power(bool on);

    /**
     * @brief 设置充电电流
     * 
     * 通过PM1 GPIO1 (PYG1_CHG_PROG) 控制充电电流
     * 
    * @param current 充电电流选择:
    *                CHARGE_CURRENT_180MA - 180mA充电电流
    *                CHARGE_CURRENT_650MA - 650mA充电电流
     *                注意: 实际充电电流会受电压影响
     * @return esp_err_t ESP_OK 成功, 其他值表示失败
     */
    esp_err_t set_charge_current(charge_current_t current);

    /**
     * @brief 控制PYB P4USBC电源开关
     * 
     * 通过PY32 IO扩展器控制PYB P4USBC电源使能引脚(Pin_PYB_P4USBC_OUT_EN)
     * 
     * @param on true=开启PYB P4USBC电源, false=关闭PYB P4USBC电源
     * @return esp_err_t ESP_OK 成功
     */
    esp_err_t set_p4usbc_power(bool on);

    /**
     * @brief 控制M5BUS电源开关
     *
     * 通过PM1的BOOST_EN控制M5BUS 5V输出
     *
     * @param on true=开启M5BUS电源, false=关闭M5BUS电源
     * @return esp_err_t ESP_OK 成功
     */
    esp_err_t set_m5bus_power(bool on);

    /**
     * @brief 设置屏幕亮度 (0-100%)
     * 
     * 通过 PY32 的 PWM1 通道 (IO9) 调节背光占空比。
     * 0 = 关闭背光 (占空比0%)，100 = 最亮 (占空比100%)。
     * 若当前未设置 PWM 频率，会自动使用默认 50kHz。
     * 
     * @param percent 亮度百分比 0-100
     * @return esp_err_t ESP_OK 成功, ESP_FAIL 失败
     */
    esp_err_t set_lcd_brightness(uint8_t percent);

    /**
     * @brief 设置LVGL显示屏旋转角度
     * 
     * @param rotation 旋转角度 (LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_90, LV_DISPLAY_ROTATION_180, LV_DISPLAY_ROTATION_270)
     * @return esp_err_t ESP_OK 成功, ESP_FAIL 失败
     */
    esp_err_t set_lvgl_rotation(lv_disp_rotation_t rotation);

    /**
     * @brief 启动LVGL基准测试演示
     * 
     * 启动LVGL的性能基准测试演示程序，用于测试显示性能
     * 需要在menuconfig中启用LV_USE_DEMO_BENCHMARK或LV_DEMO_BENCHMARK
     * 
     * @return esp_err_t ESP_OK 成功启动, ESP_FAIL 启动失败或演示不可用
     */
    esp_err_t start_lvgl_benchmark_demo();

    /**
     * @brief 启动LVGL音乐播放器演示
     * 
     * 启动LVGL的音乐播放器界面演示程序，展示复杂UI界面
     * 需要在menuconfig中启用LV_USE_DEMO_MUSIC
     * 
     * @return esp_err_t ESP_OK 成功启动, ESP_FAIL 启动失败或演示不可用
     */
    esp_err_t start_lvgl_music_demo();

    /**
     * @brief 启动自定义LVGL演示
     * 
     * @return esp_err_t ESP_OK 成功启动, ESP_FAIL 启动失败
     */
    esp_err_t start_lvgl_custom_demo(void);

    /**
    * @brief 自定义LVGL演示任务
    * 
    * 这是一个FreeRTOS任务函数，用于运行自定义的LVGL界面逻辑
    * start_lvgl_custom_demo()函数会创建这个任务
    * 
    * @param pvParameters 任务参数（未使用）
    * @return esp_err_t ESP_OK 成功启动, ESP_FAIL 启动失败
    */
    esp_err_t lvgl_custom_demo_task(void *pvParameters);
    esp_err_t stop_lvgl_custom_demo_task(uint32_t timeout_ms);

    /**
     * @brief 网卡初始化和反初始化合一的函数
     * 
     * 这个函数可以根据enable参数来初始化或反初始化以太网
     * 当enable为true时，初始化以太网驱动程序
     * 当enable为false时，反初始化以太网驱动程序
     * 
     * @param enable true=初始化网卡, false=反初始化网卡
     * @return esp_err_t ESP_OK 成功, ESP_FAIL 失败
     */
    esp_err_t ethernet_init(bool enable);

    /**
     * @brief UVC HTTPS Web服务器初始化
     * 
     * 初始化UVC相关的HTTPS Web服务器，包括SPIFFS文件系统和Web服务
     * 用于通过Web界面查看和控制UVC摄像头
     * 
     * @return esp_err_t ESP_OK 成功, ESP_FAIL 失败
     */
    esp_err_t uvc_https_init();

    /**
     * @brief UVC USB主机初始化
     * 
     * 初始化UVC USB主机功能，启动USB主机任务和UVC驱动
     * 用于连接和管理USB摄像头设备
     * 
     * @return esp_err_t ESP_OK 成功, ESP_FAIL 失败
     */
    esp_err_t uvc_usb_init();

    /**
     * @brief 初始化PM1中断系统
     * 
     * 初始化PM1电源管理芯片的中断系统，包括：
     * - ESP32 GPIO中断配置
     * - PM1芯片中断配置
     * - 启动后台中断处理任务
     * 
     * @return esp_err_t ESP_OK 成功, ESP_FAIL 失败
     */
    esp_err_t p4_pm1_irq_init();

    /**
     * @brief 将重启原因枚举转换为可读字符串
     * 
     * @param reason 重启原因枚举值
     * @return const char* 重启原因的字符串描述
     */
    static const char* reset_reason_to_string(esp_reset_reason_t reason);

    /**
     * @brief 处理 Wi‑Fi/IP 系统事件的回调。
     *
     * @param arg        用户上下文指针
     * @param event_base 事件基类（WIFI_EVENT 或 IP_EVENT）
     * @param event_id   事件 ID
     * @param event_data 事件数据指针
     */
    void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

    /**
     * @brief 处理以太网/IP 系统事件的回调。
     *
     * @param arg        用户上下文指针
     * @param event_base 事件基类（ETH_EVENT 或 IP_EVENT）
     * @param event_id   事件 ID
     * @param event_data 事件数据指针
     */
    void ethernet_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

    /**
     * @brief WiFi 基础初始化
     * 
     * 初始化 WiFi 基础架构，包括：
     * - 创建 Wi‑Fi 事件组
     * - 创建默认 WiFi STA 网络接口
     * - 初始化 WiFi 驱动程序
     * - 注册 WiFi 和 IP 事件处理程序
     * 此函数只进行基础初始化，不连接到具体的网络。
     * 
     * @return esp_err_t ESP_OK 成功，否则返回相应错误
     */
    esp_err_t wifi_base_init();

    /**
     * @brief Initialize the ESP32-C6 WiFi module and validate basic RPC access.
     */
    esp_err_t wifi_module_init(bool reset_before = false);

    /**
     * @brief Start a one-shot background WiFi initialization attempt at boot.
     */
    esp_err_t wifi_start_auto_init_once(void);

    bool is_wifi_initialized(void) const;
    bool is_wifi_init_running(void) const;
    esp_err_t get_wifi_last_init_result(void) const;
    esp_err_t wifi_refresh_status(void);

    esp_err_t wifi_scan_aps(wifi_ap_record_t* records,
                            uint16_t max_records,
                            uint16_t* out_total_count,
                            uint16_t* out_record_count,
                            bool show_hidden = false,
                            uint32_t timeout_ms = 10000);

    /**
     * @brief 后期配置并启动本地 Wi‑Fi（STA）
     * @param ssid     Wi‑Fi SSID（以 null 结尾的字符串）
     * @param password Wi‑Fi 密码（以 null 结尾的字符串）
     * @return esp_err_t ESP_OK 成功，否则返回相应错误
     */
    esp_err_t esp32c6_wifi_connect(const char* ssid, const char* password);

    /**
     * @brief 扫描可用的WiFi网络
     * 
     * 此函数会自动处理WiFi初始化（如果需要），然后扫描附近的WiFi网络。
     * 扫描结果会通过日志输出显示，包括SSID、信号强度、安全类型等信息。
     * 
     * @param max_aps 最大扫描结果数量（0-255，默认20）
     * @param timeout_ms 扫描超时时间（毫秒，默认10秒）
     * @param show_hidden 是否显示隐藏网络（默认false）
     * @return esp_err_t ESP_OK 扫描成功，否则返回相应错误代码
     */
    esp_err_t esp32c6_wifi_scan(uint8_t max_aps = 20, uint32_t timeout_ms = 10000, bool show_hidden = false);

    /**
     * @brief 等待以太网连接并获取IP地址
     * 
     * 此函数会阻塞直到以太网连接成功并获取到IP地址，或者超时
     * 
     * @param timeout_ms 超时时间（毫秒），0表示永远等待
     * @return esp_err_t ESP_OK 连接成功，ESP_ERR_TIMEOUT 超时，ESP_FAIL 连接失败
     */
    esp_err_t wait_for_ethernet_connection(uint32_t timeout_ms = 30000);

    /**
     * @brief 获取以太网连接状态
     * 
     * @return true 如果以太网已连接并获取到IP地址
     * @return false 如果以太网未连接或未获取到IP地址
     */
    bool is_ethernet_connected();

    /**
     * @brief 获取以太网IP信息
     * 
     * @param ip_info 输出的IP信息结构体指针
     * @return esp_err_t ESP_OK 成功获取，ESP_FAIL 获取失败
     */
    esp_err_t get_ethernet_ip_info(esp_netif_ip_info_t* ip_info);

    /**
     * @brief 初始化USB HID鼠标主机功能
     * 
     * 初始化TinyUSB Host模式和HID类，启用USB鼠标检测和数据读取功能
     * 
     * @return esp_err_t ESP_OK 成功，ESP_FAIL 失败
     */
    esp_err_t usb_hid_mouse_init();

    /**
     * @brief 获取USB鼠标当前数据
     * 
     * @param mouse_data 输出的鼠标数据结构体指针
     * @return esp_err_t ESP_OK 成功获取，ESP_FAIL 获取失败或鼠标未连接
     */
    esp_err_t get_usb_mouse_data(usb_mouse_data_t* mouse_data);

    /**
     * @brief 获取USB键盘当前缓存数据
     *
     * @param keyboard_data 输出的键盘数据结构体指针
     * @return esp_err_t ESP_OK 成功获取，ESP_ERR_NOT_FOUND 键盘未连接
     */
    esp_err_t get_usb_keyboard_data(usb_keyboard_data_t* keyboard_data);

    /**
     * @brief 获取USB鼠标连接状态
     * 
     * @return true 鼠标已连接
     * @return false 鼠标未连接
     */
    bool is_usb_mouse_connected();

    /**
     * @brief 获取USB键盘连接状态
     *
     * @return true 键盘已连接
     * @return false 键盘未连接
     */
    bool is_usb_keyboard_connected();

    esp_err_t init_usb_device_monitor();
    esp_err_t clear_usb_device_monitor_cache();
    int get_usb_device_count();
    bool get_usb_device_info(char* name,
                             size_t name_len,
                             uint16_t* vid,
                             uint16_t* pid,
                             uint8_t* class_flags,
                             int* count);
    int get_usb_other_device_count();
    bool get_usb_other_device_info(char* name,
                                   size_t name_len,
                                   uint16_t* vid,
                                   uint16_t* pid,
                                   int* count);

    /**
     * @brief 初始化USB HID鼠标支持（包含任务创建）
     * 
     * 初始化USB HID鼠标功能并创建鼠标检查任务，提供完整的鼠标支持
     * 
     * @return esp_err_t ESP_OK 成功，ESP_FAIL 失败
     */
    esp_err_t init_usb_mouse_support();

    /**
     * @brief 销毁LVGL鼠标支持
     * 
     * 删除LVGL鼠标光标和输入设备，用于鼠标断开连接时清理资源
     * 
     * @return esp_err_t ESP_OK 成功，ESP_FAIL 失败
     */
    esp_err_t destroy_lvgl_mouse_support();

    /**
     * @brief 发送鼠标连接事件
     */
    void notify_mouse_connected();

    /**
     * @brief 发送鼠标断开事件
     */
    void notify_mouse_disconnected();

    /**
     * @brief 手动复位 ESP-Hosted 传输层设备（如 ESP32-C6 WiFi 模块）
     * 
     * 注意：假设低电平有效的复位信号（低电平复位，高电平正常运行）
     * 
     * @return esp_err_t ESP_OK 复位成功，ESP_FAIL 复位失败或引脚配置无效
     */
    esp_err_t esp_hosted_manual_reset();

private:
    esp_eth_handle_t *eth_handles;  ///< 以太网驱动句柄数组
    uint8_t eth_cnt;                ///< 以太网接口数量
    bool ethernet_initialized;      ///< 以太网初始化状态标志
    esp_netif_t *eth_netif;         ///< 以太网网络接口
    bool netif_initialized;         ///< 网络基础设施初始化状态标志
    bool wifi_initialized;          ///< WiFi初始化状态标志
    bool wifi_hosted_initialized;
    bool wifi_event_handlers_registered;
    bool wifi_auto_init_attempted;
    volatile bool wifi_init_running;
    esp_err_t wifi_last_init_result;
    esp_netif_t *wifi_sta_netif;
    TaskHandle_t wifi_auto_init_task_handle;
    SemaphoreHandle_t wifi_init_mutex;
    EventGroupHandle_t s_eth_event_group; ///< 以太网事件组
    TaskHandle_t p4_pm1_irq_task_handle; ///< PM1按钮中断处理任务句柄
    esp_reset_reason_t reset_reason; ///< 系统重启原因
    int wifi_con_retry_num;          ///< Wi‑Fi 连接重试计数（对象内部参数）
    EventGroupHandle_t s_wifi_event_group; ///< Wi‑Fi 事件组（对象内部参数）
    char local_wifi_ssid[33] = {0};     ///< 本地 Wi‑Fi SSID（最长 32 字符 + 终止符）
    char local_wifi_password[65] = {0}; ///< 本地 Wi‑Fi 密码（最长 64 字符 + 终止符）
    
    // LCD相关句柄
    esp_lcd_panel_handle_t lcd_panel_handle;    ///< LCD面板句柄
    esp_lcd_panel_io_handle_t mipi_dbi_io;      ///< MIPI DBI I/O句柄
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;      ///< MIPI DSI bus句柄
    esp_ldo_channel_handle_t ldo_mipi_phy;      ///< MIPI DSI PHY LDO句柄
    
    // LVGL相关句柄
    lv_display_t *lvgl_disp;                    ///< LVGL显示对象
    lv_indev_t *lvgl_touch_indev;               ///< LVGL触摸输入设备
    lv_indev_t *lvgl_mouse_indev;               ///< LVGL鼠标输入设备
    lv_obj_t *lvgl_mouse_cursor;                ///< LVGL鼠标光标对象（红色）
    lv_obj_t *lvgl_touch_indicator;             ///< LVGL触摸位置指示器（蓝色）
    bool lvgl_touch_indicator_enabled;          ///< 是否启用触摸位置指示器
    bool startup_prompt_audio_enabled;          ///< 是否在启动时播放提示音
    uint32_t startup_prompt_audio_delay_ms;     ///< Delay before startup prompt sound playback
    volatile bool lvgl_port_ready;               ///< LVGL port is initialized and can be locked safely
    TaskHandle_t lvgl_custom_demo_task_handle;   ///< Custom LVGL demo update task
    SemaphoreHandle_t lvgl_custom_demo_done_sem; ///< Signaled when the custom demo task exits
    volatile bool lvgl_custom_demo_stop_requested; ///< Requests the demo task to stop before LVGL teardown
    
    // 触摸屏相关句柄
    esp_lcd_panel_io_handle_t tp_io_handle;     ///< 触摸屏I/O句柄
    esp_lcd_touch_handle_t tp_handle;           ///< 触摸屏设备句柄
    
    // LCD同步信号量
    SemaphoreHandle_t lcd_refresh_finish;       ///< LCD刷新完成信号量

    // USB鼠标相关成员
    usb_mouse_data_t current_mouse_data;        ///< 当前鼠标数据
    bool usb_host_initialized;                  ///< USB主机初始化状态
    EventGroupHandle_t mouse_event_group;       ///< 鼠标事件组，用于线程安全的鼠标状态管理

    // 鼠标事件标志位
    static const int MOUSE_EVENT_CONNECTED = BIT0;     ///< 鼠标连接事件
    static const int MOUSE_EVENT_DISCONNECTED = BIT1;  ///< 鼠标断开事件

    // ==================== 设备实时状态成员 ====================
    device_realtime_status_t device_status;  ///< 设备实时状态结构体
    uint64_t boot_timestamp_ms;              ///< 启动时间戳(毫秒)，用于计算运行时间
    std::vector<StatusUISyncCallback> ui_sync_callbacks;   ///< UI同步回调函数列表

    SemaphoreHandle_t imu_data_mutex;        ///< Protects BMI270 access and latest IMU cache
    TaskHandle_t imu_service_task_handle;    ///< Shared IMU sampling task
    volatile bool imu_service_stop_requested; ///< Requests the IMU sampling task to exit
    bmi270_tools::sensor_data_t latest_imu_data; ///< Latest shared IMU sample
    bool imu_data_valid;                     ///< Whether latest_imu_data contains a valid sample
    uint32_t imu_service_period_ms;          ///< IMU sampling period

    /**
     * @brief PM1按钮中断处理任务
     * 
     * 这是一个静态任务函数，用于处理PM1按钮中断事件
     * 
     * @param pvParameters 任务参数(指向m5_corep4_tool实例的指针)
     */
    static void p4_pm1_irq_task(void *pvParameters);

    static void imu_service_task(void *pvParameters);
    bool lvgl_ui_ready() const;
    bool try_lock_lvgl_ui(uint32_t timeout_ms) const;
    void imu_update_status_from_data(const bmi270_tools::sensor_data_t& data,
                                     StatusConfigFlag flags = StatusConfigFlag::UPDATE_STATUS);
    esp_err_t load_system_preferences_from_nvs();
    esp_err_t save_system_preference_bool(const char* key, bool enabled);
    esp_err_t save_system_preference_u32(const char* key, uint32_t value);

    /**
     * @brief 鼠标检查任务
     * 
     * 这是一个静态任务函数，用于定期检查鼠标状态和读取鼠标数据
     * 
     * @param pvParameters 任务参数(指向m5_corep4_tool实例的指针)
     */
    static void mouse_check_task(void *pvParameters);

    /**
     * @brief SD卡检测引脚中断回调函数
     * 
        * 回调触发边沿会动态切换：
        * - 当前已插入时，配置上升沿以检测拔出
        * - 当前已拔出时，配置下降沿以检测插入
     */
    static void sd_detect_isr_callback(void);

    /**
     * @brief 动态初始化SD卡检测中断
     *
     * 根据当前SD卡状态动态选择中断边沿：
     * - 若当前已插入（检测脚为LOW），则配置上升沿中断（用于检测拔出）
     * - 若当前已拔出（检测脚为HIGH），则配置下降沿中断（用于检测插入）
     *
     * 并在每次触发后根据实际电平切换下一次的中断模式。
     */
    esp_err_t sd_detect_irq_init_dynamic(void);

    /**
     * @brief 根据检测引脚电平更新下一次中断边沿
     *
     * @param pin_level Pin_PYB_SD_DECT 当前电平（HIGH=拔出, LOW=插入）
     */
    void sd_detect_irq_update_edge_from_level(int pin_level);

    /**
     * @brief SD卡热插拔处理任务句柄
     */
    TaskHandle_t sd_hotplug_task_handle = nullptr;

    /**
     * @brief SD卡热插拔处理任务
     * 
     * 接收热插拔通知后执行SD卡的初始化或反初始化，
     * 并通过 set_status() 更新状态和同步UI显示
     * 
     * @param pvParameters 任务参数(指向m5_corep4_tool实例的指针)
     */
    static void sd_hotplug_task(void *pvParameters);

};

/**
 * @brief Start the USB MSC capacity demo
 * 
 * This function initializes the USB host, registers the MSC driver,
 * and starts a task to handle device connection/disconnection and
 * print capacity information.
 */
void start_usb_msc_capacity_demo();

#endif // __M5_COREP4_COMPONENT_H__
