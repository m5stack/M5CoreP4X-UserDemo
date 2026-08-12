#include "m5_corep4_component.h"
#include "m5_corep4_ui_sync.h"
#include "corep4_lvgl_ui_out/ui.h"
#include "freertos/idf_additions.h"
#include "esp_intr_alloc.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_idf_version.h"
#include "esp_hosted.h"
#include "nvs.h"
#include "soc/soc_caps.h"
#include "usb/usb_helpers.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "hal/pmu_ll.h"
#include "driver/rtc_io.h"
#endif
#include <cstdio>
#include <cstring>
#include <new>
#include <stdint.h>

static const char *TAG = "m5_corep4_tool";
// USB HID鼠标/键盘全局变量（需要在LVGL回调和HID回调中使用）
static usb_mouse_data_t g_mouse_data = {};
static bool g_mouse_connected = false;
static hid_host_device_handle_t g_mouse_device_handle = NULL;
static usb_keyboard_data_t g_keyboard_data = {};
static bool g_keyboard_connected = false;
static hid_host_device_handle_t g_keyboard_device_handle = NULL;
static lv_obj_t *g_mouse_cursor = NULL;  // 全局鼠标光标对象指针

static constexpr int USB_MONITOR_MAX_DEVICES = 8;
static constexpr uint8_t USB_MON_CLASS_VIDEO = 0x01;
static constexpr uint8_t USB_MON_CLASS_MOUSE = 0x02;
static constexpr uint8_t USB_MON_CLASS_KEYBOARD = 0x04;
static constexpr uint8_t USB_MON_CLASS_HUB = 0x08;
static constexpr uint8_t USB_MON_CLASS_HID = 0x10;
static constexpr const char* NVS_NS_SYSTEM_PREFS = "corep4_prefs";
static constexpr const char* NVS_KEY_TOUCH_INDICATOR = "touch_ind";
static constexpr const char* NVS_KEY_BOOT_SOUND = "boot_sound";
static constexpr const char* NVS_KEY_BOOT_SOUND_DELAY_MS = "boot_snd_dly";
static constexpr uint32_t STARTUP_PROMPT_AUDIO_DELAY_DEFAULT_MS = 800;
static constexpr uint32_t STARTUP_PROMPT_AUDIO_DELAY_MAX_MS = 10000;

typedef struct {
    bool active;
    uint8_t address;
    uint16_t vid;
    uint16_t pid;
    uint8_t class_flags;
    char name[48];
    usb_device_handle_t dev_hdl;
} usb_monitor_device_t;

static usb_monitor_device_t g_usb_monitor_devices[USB_MONITOR_MAX_DEVICES] = {};
static usb_host_client_handle_t g_usb_monitor_client = NULL;
static TaskHandle_t g_usb_monitor_task_handle = NULL;
static SemaphoreHandle_t g_usb_monitor_mutex = NULL;

// 全局指针，指向当前的m5_corep4_tool实例，用于在静态回调函数中访问成员函数
static m5_corep4_tool* g_corep4_instance = nullptr;

// LVGL显示旋转角度（用于鼠标坐标自适应）
static lv_disp_rotation_t g_display_rotation = LV_DISPLAY_ROTATION_0;

// 前向声明
static void lvgl_mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data);
static void lvgl_touch_indicator_timer_cb(lv_timer_t *timer);
static void usb_device_monitor_task(void *arg);

// PM1中断标志，用于在主任务中处理PM1中断
static volatile bool pm1_irq_pending = false;
static constexpr m5pm1_clean_type_t kPm1IrqClearAll = M5PM1_CLEAN_ALL;
static constexpr uint8_t kPm1GpioIrqAllMask = (1u << M5PM1_MAX_GPIO_PINS) - 1u;
static constexpr uint8_t kPm1SysIrqAllMask = 0x3F;

static void corep4_gpio_set_high_z(gpio_num_t pin)
{
    if (pin == GPIO_NUM_NC || !GPIO_IS_VALID_GPIO(pin)) {
        return;
    }

    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_pullup_dis(pin);
    gpio_pulldown_dis(pin);
}

static void corep4_gpio_set_high_z_list(const gpio_num_t *pins, size_t pin_count)
{
    for (size_t i = 0; i < pin_count; ++i) {
        corep4_gpio_set_high_z(pins[i]);
    }
}

static esp_err_t corep4_audio_pa_power_control(void *arg, bool on)
{
    auto *tool = static_cast<m5_corep4_tool *>(arg);
    if (tool == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    return tool->set_status(StatusParam::POWER_AUDIO_PA, on);
}

static void corep4_sd_gpio_set_high_z(void)
{
    static const gpio_num_t kSdSleepPins[] = {
        Pin_P4_SD_MMC_CMD,
        Pin_P4_SD_MMC_CLK,
        Pin_P4_SD_MMC_D0,
        Pin_P4_SD_SPI_CS,
        Pin_P4_SD_SPI_MOSI,
        Pin_P4_SD_SPI_MISO,
        Pin_P4_SD_SPI_CLK,
    };

    corep4_gpio_set_high_z_list(kSdSleepPins, sizeof(kSdSleepPins) / sizeof(kSdSleepPins[0]));
    ESP_LOGI(TAG, "SD GPIOs set to high-Z");
}

extern "C" bool m5_corep4_sd_card_is_mounted_for_hosted(void)
{
    if (g_corep4_instance == nullptr) {
        return false;
    }

    auto& sd_card = g_corep4_instance->otool_tools.sd_card;
    sd_test_mode_t mode = sd_card.get_mode();
    bool uses_sdmmc_host = (mode == SD_MODE_MMC_1BIT || mode == SD_MODE_MMC_4BIT);
    return uses_sdmmc_host && (sd_card.is_initialized() || sd_card.is_mounted());
}

static esp_err_t corep4_sd_card_deinit_for_sleep(sd_tools& sd_card)
{
    esp_err_t ret = ESP_OK;

    if (sd_card.is_initialized()) {
        ESP_LOGI(TAG, "Peripheral: SD card -> deinit");
        ret = sd_card.deinit();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD deinit before sleep failed: %s", esp_err_to_name(ret));
        }
    }

    // IDF 的 SDMMC slot remove 只关闭输出驱动，不负责清内部上拉和 GPIO matrix。
    // 这里统一 reset pin + input + no-pull，避免深睡时从 SD CMD/D0/CLK 反灌已断电的 SD 卡。
    corep4_sd_gpio_set_high_z();
    return ret;
}

static void corep4_sd_detect_disable_for_sleep(M5IOE1& pyb)
{
    // Pin_PYB_SD_DECT 在卡插入时为 LOW；睡前必须关闭 PYB 内部上拉和中断，
    // 否则卡检测开关会形成持续下拉电流，也可能让 M5IOE1 保持内部活动。
    pyb.detachInterrupt(Pin_PYB_SD_DECT);
    pyb.clearInterrupt();
    pyb.pinMode(Pin_PYB_SD_DECT, INPUT);
    pyb.setPullMode(Pin_PYB_SD_DECT, M5IOE1_PULL_NONE);
    ESP_LOGI(TAG, "SD detect interrupt detached, pull disabled, GPIO_IS cleared");
}

static void corep4_gpio_output_low(gpio_num_t pin)
{
    if (pin == GPIO_NUM_NC || !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        return;
    }

    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_pullup_dis(pin);
    gpio_pulldown_dis(pin);
    gpio_set_level(pin, 0);
}

static void corep4_set_deep_sleep_hp_pad_hold_all(bool enable)
{
#if CONFIG_IDF_TARGET_ESP32P4
    // 只配置 HP_SLEEP 模式下的 pad hold，不会在当前 active 阶段冻结 Flash/UART。
    pmu_ll_hp_set_hold_all_hp_pad(&PMU, PMU_MODE_HP_SLEEP, enable);
#else
    (void)enable;
#endif
}

static void corep4_hold_sleep_reset_pins(void)
{
    static const gpio_num_t kSleepResetPins[] = {
        Pin_P4_RMII_RST,
        Pin_P4_WIFI_RESET,
    };

    for (gpio_num_t pin : kSleepResetPins) {
        corep4_gpio_output_low(pin);
        esp_err_t ret = gpio_hold_en(pin);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to hold GPIO%d before sleep: %s", static_cast<int>(pin), esp_err_to_name(ret));
        }
    }

#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_en();
#endif

    // ESP32-P4 rev < 3.0 单 pin hold 在深睡低功耗路径上不能可靠保持 HP GPIO。
    // 通过 PMU 的 HP_SLEEP hold_all 在真正进入睡眠态时统一保持 HP pad 当前状态。
    corep4_set_deep_sleep_hp_pad_hold_all(true);
}

static void corep4_unhold_sleep_reset_pins(void)
{
    static const gpio_num_t kSleepResetPins[] = {
        Pin_P4_RMII_RST,
        Pin_P4_WIFI_RESET,
    };

#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif
    corep4_set_deep_sleep_hp_pad_hold_all(false);

    for (gpio_num_t pin : kSleepResetPins) {
        // 先写入确定的 LOW 配置，再解除 hold，避免复位脚瞬间被外部上拉释放。
        corep4_gpio_output_low(pin);
        esp_err_t ret = gpio_hold_dis(pin);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to unhold GPIO%d after wake: %s", static_cast<int>(pin), esp_err_to_name(ret));
        }
    }
}

esp_err_t m5_corep4_tool::set_m5bus_gpio_output_level(bool level) {
    static const gpio_num_t kM5BusGpioPins[] = {
        COREP4_M5BUS_P2,
        COREP4_M5BUS_P4,
        COREP4_M5BUS_P8,
        COREP4_M5BUS_P10,
        COREP4_M5BUS_P14,
        COREP4_M5BUS_P16,
        COREP4_M5BUS_P18,
        COREP4_M5BUS_P20,
        COREP4_M5BUS_P22,
        COREP4_M5BUS_P24,
        COREP4_M5BUS_P26,
        COREP4_M5BUS_P7,
        COREP4_M5BUS_P9,
        COREP4_M5BUS_P11,
        COREP4_M5BUS_P13,
        COREP4_M5BUS_P15,
        COREP4_M5BUS_P17,
        COREP4_M5BUS_P19,
        COREP4_M5BUS_P21,
        COREP4_M5BUS_P23,
    };

    uint64_t pin_mask = 0;
    for (gpio_num_t pin : kM5BusGpioPins) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
            ESP_LOGW(TAG, "M5BUS pin %d is not valid output, skipped", static_cast<int>(pin));
            continue;
        }
        pin_mask |= (1ULL << pin);
    }

    if (pin_mask == 0) {
        ESP_LOGW(TAG, "No valid M5BUS output GPIO pins found");
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = pin_mask;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure M5BUS GPIOs: %s", esp_err_to_name(ret));
        return ret;
    }

    int level_value = level ? 1 : 0;
    for (gpio_num_t pin : kM5BusGpioPins) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
            continue;
        }
        ret = gpio_set_level(pin, level_value);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set M5BUS pin %d level: %s", static_cast<int>(pin), esp_err_to_name(ret));
            return ret;
        }
    }

    return ESP_OK;
}

// SD卡检测中断回调函数实现
void m5_corep4_tool::sd_detect_isr_callback(void)
{
    // 注意：当前pyb(M5IOE1)以轮询方式触发回调（M5IOE1_INT_MODE_POLLING），
    // 回调不在硬件ISR上下文中，可安全执行I2C读写与重配中断边沿。
    if (g_corep4_instance == nullptr) {
        return;
    }

    int pin_state = g_corep4_instance->pyb.digitalRead(Pin_PYB_SD_DECT);
    if (pin_state < 0) {
        ESP_LOGW(TAG, "Failed to read SD detect pin state");
        return;
    }

    if (pin_state == HIGH) {
        ESP_LOGI(TAG, "SD card removed (Pin_PYB_SD_DECT HIGH)");
    } else {
        ESP_LOGI(TAG, "SD card inserted (Pin_PYB_SD_DECT LOW)");
    }

    // 通知SD热插拔处理任务
    if (g_corep4_instance->sd_hotplug_task_handle != nullptr) {
        xTaskNotify(g_corep4_instance->sd_hotplug_task_handle,
                    (uint32_t)pin_state, eSetValueWithOverwrite);
    }

    // 每次触发后根据实际电平切换下一次中断边沿
    g_corep4_instance->sd_detect_irq_update_edge_from_level(pin_state);
}

void m5_corep4_tool::sd_detect_irq_update_edge_from_level(int pin_level)
{
    // 约定：输入上拉，LOW=已插入，HIGH=已拔出
    device_status.sdcard.sd_detected = (pin_level == LOW);

    // 若当前已插入，则下一次关注拔出 -> 上升沿
    // 若当前已拔出，则下一次关注插入 -> 下降沿
    uint8_t next_mode = (pin_level == LOW) ? RISING : FALLING;
    pyb.attachInterrupt(Pin_PYB_SD_DECT, sd_detect_isr_callback, next_mode);

    ESP_LOGI(TAG, "SD detect interrupt switched to %s edge",
             next_mode == RISING ? "rising" : "falling");
}

esp_err_t m5_corep4_tool::sd_detect_irq_init_dynamic(void)
{
    // 配置 Pin_PYB_SD_DECT 引脚为输入上拉
    pyb.pinMode(Pin_PYB_SD_DECT, INPUT_PULLUP);

    int pin_state = pyb.digitalRead(Pin_PYB_SD_DECT);
    if (pin_state < 0) {
        ESP_LOGE(TAG, "Failed to read SD detect pin state during init");
        return ESP_FAIL;
    }

    // 创建SD热插拔处理任务（只创建一次）
    // 注意：不在此处发送初始通知，由 main.cpp 完成启动初始化后手动更新状态
    if (sd_hotplug_task_handle == nullptr) {
        BaseType_t task_ret = xTaskCreate(
            sd_hotplug_task,
            "sd_hotplug",
            4096,
            this,
            5,
            &sd_hotplug_task_handle
        );
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create SD hotplug task");
            sd_hotplug_task_handle = nullptr;
        } else {
            ESP_LOGI(TAG, "SD hotplug task created successfully");
        }
    }

    // 根据当前状态动态选择中断边沿
    sd_detect_irq_update_edge_from_level(pin_state);
    ESP_LOGI(TAG, "SD detect interrupt initialized dynamically (level=%s)",
             pin_state == LOW ? "LOW(inserted)" : "HIGH(removed)");
    return ESP_OK;
}

// SD卡热插拔处理任务
void m5_corep4_tool::sd_hotplug_task(void *pvParameters)
{
    m5_corep4_tool *self = static_cast<m5_corep4_tool*>(pvParameters);
    const char *tag = "SD_HOTPLUG";
    ESP_LOGI(tag, "SD hotplug task started");

    while (true) {
        uint32_t pin_state_val = 0;
        // 阻塞等待热插拔通知
        if (xTaskNotifyWait(0, 0xFFFFFFFFUL, &pin_state_val, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // 去抖延迟 300ms
        vTaskDelay(pdMS_TO_TICKS(300));

        // 重新读取实际引脚电平以确认状态
        int actual_pin_state = self->pyb.digitalRead(Pin_PYB_SD_DECT);
        if (actual_pin_state < 0) {
            actual_pin_state = (int)pin_state_val; // fallback
        }

        if (actual_pin_state == HIGH) {
            // ---- SD卡已拔出 ----
            ESP_LOGI(tag, "Processing SD card removal...");
            if (self->otool_tools.sd_card.is_initialized()) {
                esp_err_t ret = self->otool_tools.sd_card.deinit();
                if (ret != ESP_OK) {
                    ESP_LOGE(tag, "SD card deinit failed: %s", esp_err_to_name(ret));
                } else {
                    ESP_LOGI(tag, "SD card deinitialized successfully");
                }
            }
            // 清空存储状态并通知UI
            self->set_status(StatusParam::SD_TOTAL_BYTES, (uint64_t)0);
            self->set_status(StatusParam::SD_FREE_BYTES,  (uint64_t)0);
            self->set_status(StatusParam::SD_INITIALIZED, false);
            self->set_status(StatusParam::SD_DETECTED,    false);

        } else {
            // ---- SD卡已插入 ----
            ESP_LOGI(tag, "Processing SD card insertion...");

            // 如果尚未初始化则执行初始化（如已初始化则直接查询状态）
            esp_err_t sd_ret = ESP_OK;
            if (!self->otool_tools.sd_card.is_initialized()) {
                // 热插拔重插时也使用SPI模式，避免与ESP-Hosted WiFi共享SDMMC host。
                self->otool_tools.sd_card.set_mode(
                    SD_MODE_SPI,
                    Pin_P4_SD_SPI_CS, Pin_P4_SD_SPI_MOSI, Pin_P4_SD_SPI_MISO, Pin_P4_SD_SPI_CLK);
                self->otool_tools.sd_card.set_mount_point("/sdcard");
                sd_ret = self->otool_tools.sd_card.init();
                if (sd_ret != ESP_OK) {
                    ESP_LOGE(tag, "SD card init failed: %s", esp_err_to_name(sd_ret));
                } else {
                    ESP_LOGI(tag, "SD card initialized successfully");
                }
            } else {
                ESP_LOGI(tag, "SD card already initialized, just syncing status");
            }

            // 查询容量信息并更新状态
            uint64_t total_bytes = 0, free_bytes = 0;
            if (self->otool_tools.sd_card.is_initialized()) {
                esp_err_t cap_ret = self->otool_tools.sd_card.get_capacity_info(
                    &total_bytes, &free_bytes);
                if (cap_ret != ESP_OK) {
                    ESP_LOGW(tag, "Failed to get capacity info: %s",
                             esp_err_to_name(cap_ret));
                }
            }
            self->set_status(StatusParam::SD_DETECTED,    true);
            self->set_status(StatusParam::SD_INITIALIZED, self->otool_tools.sd_card.is_initialized());
            self->set_status(StatusParam::SD_TOTAL_BYTES, total_bytes);
            self->set_status(StatusParam::SD_FREE_BYTES,  free_bytes);
        }
    }
}

// m5_corep4_tool 构造函数
m5_corep4_tool::m5_corep4_tool()
{
    // 设置全局实例指针，用于静态回调函数中访问成员函数
    g_corep4_instance = this;

    // 获取启动时间戳
    boot_timestamp_ms = esp_timer_get_time() / 1000;

    // 初始化设备状态结构体为零
    memset(&device_status, 0, sizeof(device_status));

    // 初始化以太网相关变量
    eth_handles = nullptr;
    eth_cnt = 0;
    ethernet_initialized = false;
    eth_netif = nullptr;
    netif_initialized = false;
    wifi_initialized = false;
    wifi_hosted_initialized = false;
    wifi_event_handlers_registered = false;
    wifi_auto_init_attempted = false;
    wifi_init_running = false;
    wifi_last_init_result = ESP_ERR_INVALID_STATE;
    wifi_sta_netif = nullptr;
    wifi_auto_init_task_handle = nullptr;
    wifi_init_mutex = nullptr;

    // 初始化PM1任务句柄
    p4_pm1_irq_task_handle = nullptr;

    // 获取系统重启原因
    reset_reason = esp_reset_reason();

    // 初始化 Wi‑Fi 重试计数
    wifi_con_retry_num = 0;

    // Wi‑Fi 事件组置空，init() 时创建
    s_wifi_event_group = NULL;

    // 以太网事件组置空，ethernet_init() 时创建
    s_eth_event_group = NULL;

    // 初始化LCD相关句柄
    lcd_panel_handle = nullptr;
    mipi_dbi_io = nullptr;
    mipi_dsi_bus = nullptr;
    ldo_mipi_phy = nullptr;

    // 初始化LVGL相关句柄
    lvgl_disp = nullptr;
    lvgl_touch_indev = nullptr;
    lvgl_mouse_indev = nullptr;
    lvgl_mouse_cursor = nullptr;
    lvgl_touch_indicator = nullptr;
    lvgl_touch_indicator_enabled = false;
    lvgl_port_ready = false;
    startup_prompt_audio_enabled = true;
    startup_prompt_audio_delay_ms = STARTUP_PROMPT_AUDIO_DELAY_DEFAULT_MS;
    lvgl_custom_demo_task_handle = nullptr;
    lvgl_custom_demo_done_sem = nullptr;
    lvgl_custom_demo_stop_requested = false;

    // 初始化触摸屏相关句柄
    tp_io_handle = nullptr;
    tp_handle = nullptr;

    // 初始化LCD同步信号量
    lcd_refresh_finish = nullptr;

    // 初始化USB鼠标相关成员
    usb_host_initialized = false;
    memset(&current_mouse_data, 0, sizeof(current_mouse_data));
    mouse_event_group = nullptr;

    imu_data_mutex = nullptr;
    imu_service_task_handle = nullptr;
    imu_service_stop_requested = false;
    memset(&latest_imu_data, 0, sizeof(latest_imu_data));
    imu_data_valid = false;
    imu_service_period_ms = 50;
}

// m5_corep4_tool 析构函数
m5_corep4_tool::~m5_corep4_tool()
{
    // 确保在析构时反初始化以太网
    if (ethernet_initialized) {
        ethernet_init(false);
    }
    
    // 清理WiFi资源
    if (wifi_initialized) {
        ESP_LOGI(TAG, "Cleaning up WiFi resources...");
        esp_wifi_stop();
        esp_wifi_deinit();
        wifi_initialized = false;
    }
    if (wifi_hosted_initialized) {
        ESP_LOGI(TAG, "Cleaning up ESP-Hosted resources...");
        esp_hosted_deinit();
        wifi_hosted_initialized = false;
    }
    if (wifi_init_mutex) {
        vSemaphoreDelete(wifi_init_mutex);
        wifi_init_mutex = nullptr;
    }

    stop_lvgl_custom_demo_task(1000);
    
    // 清理PM1任务
    if (p4_pm1_irq_task_handle != nullptr) {
        vTaskDelete(p4_pm1_irq_task_handle);
        p4_pm1_irq_task_handle = nullptr;
    }

    imu_stop_service();
    if (imu_data_mutex != nullptr) {
        vSemaphoreDelete(imu_data_mutex);
        imu_data_mutex = nullptr;
    }
    
    // 清理LCD资源
    if (lcd_panel_handle) {
        ESP_LOGI(TAG, "Cleaning up LCD panel...");
        esp_lcd_panel_del(lcd_panel_handle);
        lcd_panel_handle = nullptr;
    }
    
    if (mipi_dbi_io) {
        ESP_LOGI(TAG, "Cleaning up MIPI DBI I/O...");
        esp_lcd_panel_io_del(mipi_dbi_io);
        mipi_dbi_io = nullptr;
    }

    if (mipi_dsi_bus) {
        ESP_LOGI(TAG, "Cleaning up MIPI DSI bus...");
        esp_err_t dsi_ret = esp_lcd_del_dsi_bus(mipi_dsi_bus);
        if (dsi_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete MIPI DSI bus: %s", esp_err_to_name(dsi_ret));
        }
        mipi_dsi_bus = nullptr;
    }


    if (ldo_mipi_phy) {
        ESP_LOGI(TAG, "Releasing MIPI DSI PHY LDO...");
        esp_err_t ldo_ret = esp_ldo_release_channel(ldo_mipi_phy);
        if (ldo_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to release MIPI DSI PHY LDO: %s", esp_err_to_name(ldo_ret));
        }
        ldo_mipi_phy = nullptr;
    }
    
    // 清理LVGL资源
    lvgl_port_ready = false;

    if (lvgl_touch_indev) {
        ESP_LOGI(TAG, "Cleaning up LVGL touch input device...");
        lv_indev_delete(lvgl_touch_indev);
        lvgl_touch_indev = nullptr;
    }
    
    if (lvgl_disp) {
        ESP_LOGI(TAG, "Cleaning up LVGL display...");
        lv_display_delete(lvgl_disp);
        lvgl_disp = nullptr;
    }

    if (lvgl_custom_demo_done_sem != nullptr && lvgl_custom_demo_task_handle == nullptr) {
        vSemaphoreDelete(lvgl_custom_demo_done_sem);
        lvgl_custom_demo_done_sem = nullptr;
    }
    
    // 清理触摸屏资源
    if (tp_handle) {
        ESP_LOGI(TAG, "Cleaning up touch handle...");
        esp_lcd_touch_del(tp_handle);
        tp_handle = nullptr;
    }
    
    if (tp_io_handle) {
        ESP_LOGI(TAG, "Cleaning up touch I/O handle...");
        esp_lcd_panel_io_del(tp_io_handle);
        tp_io_handle = nullptr;
    }
    
    // 清理LCD同步信号量
    if (lcd_refresh_finish) {
        ESP_LOGI(TAG, "Cleaning up LCD refresh semaphore...");
        vSemaphoreDelete(lcd_refresh_finish);
        lcd_refresh_finish = nullptr;
    }
    
    // Cleanup the toolbox

    // 删除 Wi‑Fi 事件组
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    
    // 删除以太网事件组
    if (s_eth_event_group) {
        vEventGroupDelete(s_eth_event_group);
        s_eth_event_group = NULL;
    }
    
    // 清理USB Host资源
    if (usb_host_initialized) {
        ESP_LOGI(TAG, "Cleaning up USB Host resources...");
        // 注意：TinyUSB的清理需要根据具体的实现来决定
        // 通常情况下不需要手动清理，系统重启时会自动清理
        usb_host_initialized = false;
    }
}

esp_err_t m5_corep4_tool::init()
{
    // 初始化设备实时状态结构体 (在所有初始化之前)
    esp_err_t status_ret = init_device_status();
    if (status_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize device status: %s", esp_err_to_name(status_ret));
    }

    // 初始化 GPIO引脚
    gpio_init();


    // i2c init
    otool_tools.i2c_tools.set_i2c_config(Pin_COREP4_INTERNAL_I2C_SDA, Pin_COREP4_INTERNAL_I2C_SCL, COREP4_INTERNAL_I2C_FREQ_HZ, COREP4_INTERNAL_I2C_PORT);
    // otool_tools.i2c_tools.set_i2c_config(GPIO_NUM_53, GPIO_NUM_54, 100000, (i2c_port_t)I2C_NUM_SW_0, I2C_PULLUP_DISABLE);
    otool_tools.i2c_tools.init();
    otool_tools.i2c_tools.scan_and_add_i2c_bus_devices();

    // 初始化 PYB (M5IOE1)
    pyb.begin(otool_tools.i2c_tools.get_i2c_bus_handle(),
              I2C_ADDR_PYB,
              M5IOE1_I2C_FREQ_400K,
              M5IOE1_INT_MODE_POLLING);
    set_status(StatusParam::POWER_PYB_INITIALIZED, true, StatusConfigFlag::UPDATE_STATUS);
    otool_tools.audio.set_pa_power_callback(corep4_audio_pa_power_control, this);

    // 复位线使用开漏输出。这里先释放，实际休眠前必须重新拉低，避免外设断电后被复位线反灌。
    pyb.pinMode(Pin_PYB_TP_RST, OUTPUT);
    pyb.setDriveMode(Pin_PYB_TP_RST, M5IOE1_DRIVE_OPENDRAIN);       // open-drain
    pyb.digitalWrite(Pin_PYB_TP_RST, HIGH);

    // 复位线使用开漏输出。这里先释放，实际休眠前必须重新拉低，避免外设断电后被复位线反灌。
    pyb.pinMode(Pin_PYB_LCD_RST, OUTPUT);
    pyb.setDriveMode(Pin_PYB_LCD_RST, M5IOE1_DRIVE_OPENDRAIN);      // open-drain
    pyb.digitalWrite(Pin_PYB_LCD_RST, HIGH);

    // Bring up the shared peripheral rail used by the board devices initialized below.
    set_status(StatusParam::POWER_PYB_L3B, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 动态初始化SD卡检测中断：按当前插入/拔出状态选择边沿，且每次触发后自动切换
    esp_err_t sd_irq_ret = sd_detect_irq_init_dynamic();
    if (sd_irq_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD detect interrupt dynamic init failed: %s", esp_err_to_name(sd_irq_ret));
    }

    // 初始化 PM1
    pm1.begin(otool_tools.i2c_tools.get_i2c_bus_handle(), I2C_ADDR_PM1, 100000);
    set_status(StatusParam::POWER_PM1_INITIALIZED, true, StatusConfigFlag::UPDATE_STATUS);
    // 关闭 PM1 看门狗
    pm1.wdtSet(0);
    // 使PMIC的灯保持常闭
    pm1.setPowerConfig(M5PM1_PWR_CFG_LED_CTRL, 0x00);
    
    // 再次扫描并添加新增的I2C设备
    otool_tools.i2c_tools.scan_and_add_i2c_bus_devices();

    // 初始化PM1中断系统
    esp_err_t ret_pm1_irq = p4_pm1_irq_init();
    if (ret_pm1_irq != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize PM1 IRQ system: %s", esp_err_to_name(ret_pm1_irq));
        return ret_pm1_irq;
    }

    // IMU sampling is lazy-started by the IMU function page. Sleep wake paths
    // still initialize IMU explicitly through imu_prepare_for_sleep() when needed.

    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    esp_err_t prefs_ret = load_system_preferences_from_nvs();
    if (prefs_ret != ESP_OK) {
        ESP_LOGW(TAG, "System preferences load failed: %s", esp_err_to_name(prefs_ret));
    }

    // 创建默认事件循环
    esp_err_t event_loop_ret = esp_event_loop_create_default();
    
    // 初始化 TCP/IP 网络接口
    esp_err_t netif_ret = esp_netif_init();
    
    // 如果两个函数都成功执行，设置网络接口已初始化标志
    if (event_loop_ret == ESP_OK && netif_ret == ESP_OK) {
        netif_initialized = true;
        ESP_LOGI(TAG, "Network infrastructure initialized successfully");
    } else {
        ESP_LOGW(TAG, "Network infrastructure initialization incomplete - event_loop: %s, netif: %s", 
                 esp_err_to_name(event_loop_ret), esp_err_to_name(netif_ret));
    }



    return ESP_OK;
}

esp_err_t m5_corep4_tool::load_system_preferences_from_nvs()
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NS_SYSTEM_PREFS, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t value = 0;
    ret = nvs_get_u8(nvs, NVS_KEY_TOUCH_INDICATOR, &value);
    if (ret == ESP_OK) {
        set_lvgl_touch_indicator_enabled(value != 0);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read touch indicator preference: %s", esp_err_to_name(ret));
    }

    value = 0;
    ret = nvs_get_u8(nvs, NVS_KEY_BOOT_SOUND, &value);
    if (ret == ESP_OK) {
        startup_prompt_audio_enabled = (value != 0);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read boot sound preference: %s", esp_err_to_name(ret));
    }

    uint32_t delay_ms = STARTUP_PROMPT_AUDIO_DELAY_DEFAULT_MS;
    ret = nvs_get_u32(nvs, NVS_KEY_BOOT_SOUND_DELAY_MS, &delay_ms);
    if (ret == ESP_OK) {
        if (delay_ms > STARTUP_PROMPT_AUDIO_DELAY_MAX_MS) {
            ESP_LOGW(TAG,
                     "Boot sound delay %lu ms exceeds max, clamped to %lu ms",
                     static_cast<unsigned long>(delay_ms),
                     static_cast<unsigned long>(STARTUP_PROMPT_AUDIO_DELAY_MAX_MS));
            delay_ms = STARTUP_PROMPT_AUDIO_DELAY_MAX_MS;
        }
        startup_prompt_audio_delay_ms = delay_ms;
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read boot sound delay preference: %s", esp_err_to_name(ret));
    }

    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t m5_corep4_tool::save_system_preference_bool(const char* key, bool enabled)
{
    if (!key || !key[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NS_SYSTEM_PREFS, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, key, enabled ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

esp_err_t m5_corep4_tool::save_system_preference_u32(const char* key, uint32_t value)
{
    if (!key || !key[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NS_SYSTEM_PREFS, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u32(nvs, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

// ------------------ 新增控制函数实现 ------------------
esp_err_t m5_corep4_tool::set_lcd_power(bool on)
{
    pyb.pinMode(Pin_PYB_LCD_PWR_EN, OUTPUT);
    pyb.digitalWrite(Pin_PYB_LCD_PWR_EN, on ? HIGH : LOW);

    ESP_LOGI(TAG, "LCD Power %s", on?"ON":"OFF");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::reset_lcd()
{
    pyb.pinMode(Pin_PYB_LCD_RST, OUTPUT);
    pyb.setDriveMode(Pin_PYB_LCD_RST, M5IOE1_DRIVE_OPENDRAIN);
    pyb.digitalWrite(Pin_PYB_LCD_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(100));
    pyb.digitalWrite(Pin_PYB_LCD_RST, HIGH);
    // RST 拉高后须等待 >= 120ms，LCD 内部电路（电荷泵、振荡器）才能完成初始化
    // 冷启动时内部电容未充电，此延迟不可省略
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "LCD Reset pulse done (100ms low, 120ms settle)");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::reset_touch()
{
    pyb.pinMode(Pin_PYB_TP_RST, OUTPUT);
    pyb.setDriveMode(Pin_PYB_TP_RST, M5IOE1_DRIVE_OPENDRAIN);
    pyb.digitalWrite(Pin_PYB_TP_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(100));
    pyb.digitalWrite(Pin_PYB_TP_RST, HIGH);
    ESP_LOGI(TAG, "Touch Reset pulse done (100ms low)");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::touch_enter_sleep(void)
{
    if (tp_handle != nullptr) {
        // 使用 CST3XX 驱动提供的深度休眠函数
        esp_err_t ret = esp_lcd_touch_cst3xx_enter_sleep(tp_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Touch entered deep sleep mode");
        } else {
            ESP_LOGE(TAG, "Failed to enter touch sleep mode");
        }
        return ret;
    }
    ESP_LOGW(TAG, "Touch handle is NULL, cannot enter sleep");
    return ESP_ERR_INVALID_STATE;
}

esp_err_t m5_corep4_tool::touch_resume(void)
{
    if (tp_handle != nullptr) {
        // 使用 CST3XX 驱动提供的恢复函数
        esp_err_t ret = esp_lcd_touch_cst3xx_resume(tp_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Touch resumed from sleep");
        } else {
            ESP_LOGE(TAG, "Failed to resume touch");
        }
        return ret;
    }
    ESP_LOGW(TAG, "Touch handle is NULL, cannot resume");
    return ESP_ERR_INVALID_STATE;
}

esp_err_t m5_corep4_tool::set_audio_chip_power(bool on)
{
    pyb.pinMode(Pin_PYB_AU_PWR_EN, OUTPUT);
    pyb.digitalWrite(Pin_PYB_AU_PWR_EN, on ? HIGH : LOW);
    ESP_LOGI(TAG, "Audio Power %s", on?"ON":"OFF");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::set_audio_pa_power(bool on)
{
    pyb.pinMode(Pin_PYB_SPK_PWR_EN, OUTPUT);
    pyb.digitalWrite(Pin_PYB_SPK_PWR_EN, on ? HIGH : LOW);
    ESP_LOGI(TAG, "Audio PA Power %s", on?"ON":"OFF");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::set_lcd_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100; // clamp

    if (pyb.setPwmFrequency(50000) != M5IOE1_OK) {
        ESP_LOGE(TAG, "Failed to (re)set PWM frequency for brightness");
        return ESP_FAIL;
    }

    // 使用 PWM1 (channel 0) 设置占空比，对应背光 IO9
    m5ioe1_err_t pwm_ret = pyb.setPwmDuty(0, percent, false, percent > 0); // 0% 时关闭使能
    if (pwm_ret != M5IOE1_OK) {
        ESP_LOGE(TAG, "Failed to set brightness %u%%", percent);
        return ESP_FAIL;
    }
    // 确保为推挽输出
    pyb.setDriveMode(Pin_PYB_LCD_BL_PWM, 0);
    ESP_LOGI(TAG, "LCD Brightness: %u%%", percent);
    return ESP_OK;
}

esp_err_t m5_corep4_tool::set_pyb_l3b_power(bool on)
{
    // L3B 是 PYB 侧外设主供电轨；不要把它当成仅影响单个外设的细粒度开关。
    pyb.pinMode(Pin_PYB_L3B_EN, OUTPUT);
    pyb.digitalWrite(Pin_PYB_L3B_EN, on ? HIGH : LOW);
    ESP_LOGI(TAG, "PYB L3B Power %s", on?"ON":"OFF");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::set_pyb_usba_power(bool on)
{
    pyb.pinMode(Pin_PYB_USBA_OUT_EN, OUTPUT);
    pyb.digitalWrite(Pin_PYB_USBA_OUT_EN, on ? HIGH : LOW);
    ESP_LOGI(TAG, "PYB USBA Power %s", on?"ON":"OFF");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::set_grove_power(bool on)
{
    pyb.pinMode(Pin_PYB_GROVE_PWR_EN, OUTPUT);
    pyb.digitalWrite(Pin_PYB_GROVE_PWR_EN, on ? HIGH : LOW);
    ESP_LOGI(TAG, "PYB Grove Power %s", on?"ON":"OFF");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::set_charge_current(charge_current_t current)
{
    m5pm1_err_t ret = M5PM1_OK;

    if (current == CHARGE_CURRENT_OFF) {
        ret = pm1.setChargeEnable(false);
        if (ret != M5PM1_OK) {
            ESP_LOGE(TAG, "Failed to disable charge: %d", ret);
            return ESP_FAIL;
        }

        ret = pm1.gpioSetMode(PYG1_CHG_PROG, M5PM1_GPIO_MODE_INPUT);
        if (ret != M5PM1_OK) {
            ESP_LOGE(TAG, "Failed to set PM1 GPIO1 mode (OFF): %d", ret);
            return ESP_FAIL;
        }

        ret = pm1.gpioSetDrive(PYG1_CHG_PROG, M5PM1_GPIO_DRIVE_OPENDRAIN);
        if (ret != M5PM1_OK) {
            ESP_LOGE(TAG, "Failed to set PM1 GPIO1 drive (OFF): %d", ret);
            return ESP_FAIL;
        }
    } else {
        ret = pm1.setChargeEnable(true);
        if (ret != M5PM1_OK) {
            ESP_LOGE(TAG, "Failed to enable charge: %d", ret);
            return ESP_FAIL;
        }

        if (current == CHARGE_CURRENT_650MA) {
            // 650mA: GPIO is push-pull output and pulled low.
            ret = pm1.gpioSetMode(PYG1_CHG_PROG, M5PM1_GPIO_MODE_OUTPUT);
            if (ret != M5PM1_OK) {
                ESP_LOGE(TAG, "Failed to set PM1 GPIO1 mode (650mA): %d", ret);
                return ESP_FAIL;
            }

            ret = pm1.gpioSetDrive(PYG1_CHG_PROG, M5PM1_GPIO_DRIVE_PUSHPULL);
            if (ret != M5PM1_OK) {
                ESP_LOGE(TAG, "Failed to set PM1 GPIO1 drive (650mA): %d", ret);
                return ESP_FAIL;
            }

            pm1.digitalWriteWithRes(PYG1_CHG_PROG, LOW, &ret);
            if (ret != M5PM1_OK) {
                ESP_LOGE(TAG, "Failed to pull PM1 GPIO1 low (650mA): %d", ret);
                return ESP_FAIL;
            }
        } else {
            // 180mA: GPIO is input and open-drain hi-Z.
            ret = pm1.gpioSetMode(PYG1_CHG_PROG, M5PM1_GPIO_MODE_INPUT);
            if (ret != M5PM1_OK) {
                ESP_LOGE(TAG, "Failed to set PM1 GPIO1 mode (180mA): %d", ret);
                return ESP_FAIL;
            }

            ret = pm1.gpioSetDrive(PYG1_CHG_PROG, M5PM1_GPIO_DRIVE_OPENDRAIN);
            if (ret != M5PM1_OK) {
                ESP_LOGE(TAG, "Failed to set PM1 GPIO1 drive (180mA): %d", ret);
                return ESP_FAIL;
            }
        }
    }

    ESP_LOGI(TAG, "Charge current set to %s (650mA=GPIO low, 180mA=GPIO hi-Z, OFF=charge disabled)",
             (current == CHARGE_CURRENT_650MA) ? "650mA" : ((current == CHARGE_CURRENT_OFF) ? "OFF" : "180mA"));
    return ESP_OK;
}

esp_err_t m5_corep4_tool::set_p4usbc_power(bool on)
{
    pyb.pinMode(Pin_PYB_P4USBC_OUT_EN, OUTPUT);
    pyb.digitalWrite(Pin_PYB_P4USBC_OUT_EN, on ? HIGH : LOW);
    ESP_LOGI(TAG, "PYB P4USBC Power %s", on?"ON":"OFF");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::set_m5bus_power(bool on)
{
    m5pm1_err_t ret = pm1.setBoostEnable(on);
    if (ret != M5PM1_OK) {
        ESP_LOGE(TAG, "M5BUS Power control failed: %d", ret);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "M5BUS Power %s", on ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::gpio_init()
{
    // 解除上一次深睡前对 PHY/C6 复位脚的保持；先保持 LOW，避免解除瞬间外设退出复位。
    corep4_unhold_sleep_reset_pins();
    return ESP_OK;
}

// 注: 设备状态相关函数已移动到 m5_corep4_status.cpp

esp_err_t m5_corep4_tool::enter_deep_sleep()
{
    // 进入深度休眠模式
    ESP_LOGI(TAG, "Entering deep sleep mode...");

    // 获取唤醒配置
    bool wake_rtc = device_status.sleep.wake_rtc_enable;
    bool wake_touch = device_status.sleep.wake_touch_enable;
    bool wake_bmi = device_status.sleep.wake_bmi_enable;
    uint32_t wake_rtc_sec = device_status.sleep.wake_rtc_seconds;

    ESP_LOGI(TAG, "Wake config: RTC=%s(%us), Touch=%s, BMI=%s",
             wake_rtc ? "ON" : "OFF", static_cast<unsigned>(wake_rtc_sec),
             wake_touch ? "ON" : "OFF",
             wake_bmi ? "ON" : "OFF");

    // 配置ESP32-P4唤醒源
    uint64_t wakeup_gpio_mask = 0;
    
    // PM1 IRQ引脚 (GPIO0) - 按钮唤醒、RTC唤醒(通过PM1定时器)、BMI唤醒都通过此引脚
    // 按钮唤醒总是启用（由PM1硬件处理）
    wakeup_gpio_mask |= (1ULL << Pin_P4_PMIC_IRQ);
    ESP_LOGI(TAG, "Enabled PM1_IRQ (GPIO%d) as wakeup source (BTN/RTC/BMI)", Pin_P4_PMIC_IRQ);
    
    // 触摸唤醒 - 直接使用ESP32-P4 GPIO1
    // CST3XX INT → GPIO1，EXT1 ANY_LOW 触发。
    // CST3XX 始终有电（功耗分析仪验证），深度睡眠期间照常工作。
    if (wake_touch) {
        wakeup_gpio_mask |= (1ULL << Pin_P4_TP_INT);
        ESP_LOGI(TAG, "Touch wake (GPIO%d) added to EXT1 mask. Effectiveness depends on CST3XX power state in L1.", Pin_P4_TP_INT);

    }

    // 配置EXT1唤醒
    esp_err_t ret = esp_sleep_enable_ext1_wakeup(wakeup_gpio_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable EXT1 wakeup: %s", esp_err_to_name(ret));
        return ret;
    }

    // 配置PM1唤醒源
    // BMI唤醒 - 启用PM1 GPIO0唤醒
    if (wake_bmi) {
        pm1.gpioSetWakeEnable(PYG0_IMU_INT, true);
        pm1.gpioSetWakeEdge(PYG0_IMU_INT, M5PM1_GPIO_WAKE_FALLING);
        ESP_LOGI(TAG, "Enabled PM1 GPIO0 (BMI INT) wake");
    } else {
        pm1.gpioSetWakeEnable(PYG0_IMU_INT, false);
    }
    
    // RTC唤醒 - 走RX8130定时器中断，通过PM1 GPIO4(PYG4_RTC_INT)触发PM1 IRQ唤醒
    // PM1定时器不参与该路径，避免残留配置导致误唤醒
    pm1.timerClear();
    if (wake_rtc) {
        pm1.gpioSetMode(PYG4_RTC_INT, M5PM1_GPIO_MODE_INPUT);
        pm1.gpioSetPull(PYG4_RTC_INT, M5PM1_GPIO_PULL_UP);
        pm1.gpioSetWakeEnable(PYG4_RTC_INT, true);
        pm1.gpioSetWakeEdge(PYG4_RTC_INT, M5PM1_GPIO_WAKE_FALLING);
        ESP_LOGI(TAG, "Enabled RX8130 INT via PM1 GPIO4 (falling edge) wake");
    } else {
        pm1.gpioSetMode(PYG4_RTC_INT, M5PM1_GPIO_MODE_INPUT);
        pm1.gpioSetPull(PYG4_RTC_INT, M5PM1_GPIO_PULL_UP);
        pm1.gpioSetWakeEnable(PYG4_RTC_INT, false);
    }

    auto check_pm1_irq_op = [](m5pm1_err_t err, const char *op) -> esp_err_t {
        if (err != M5PM1_OK) {
            ESP_LOGE(TAG, "%s failed: %d", op, err);
            return ESP_FAIL;
        }
        return ESP_OK;
    };

    // 睡前重新收敛 PM1 IRQ mask，避免运行期 SYS 事件把 PM1_IRQ 持续拉低。
    if (check_pm1_irq_op(pm1.irqSetGpioMaskAll(M5PM1_IRQ_MASK_ENABLE), "Mask all PM1 GPIO IRQ") != ESP_OK) {
        return ESP_FAIL;
    }
    if (wake_bmi &&
        check_pm1_irq_op(pm1.irqSetGpioMask(M5PM1_IRQ_GPIO0, M5PM1_IRQ_MASK_DISABLE),
                         "Unmask PM1 GPIO0 IRQ for BMI wake") != ESP_OK) {
        return ESP_FAIL;
    }
    if (wake_rtc &&
        check_pm1_irq_op(pm1.irqSetGpioMask(M5PM1_IRQ_GPIO4, M5PM1_IRQ_MASK_DISABLE),
                         "Unmask PM1 GPIO4 IRQ for RTC wake") != ESP_OK) {
        return ESP_FAIL;
    }
    if (check_pm1_irq_op(pm1.irqSetSysMaskAll(M5PM1_IRQ_MASK_ENABLE), "Mask all PM1 SYS IRQ") != ESP_OK) {
        return ESP_FAIL;
    }
    if (check_pm1_irq_op(pm1.irqSetBtnMaskAll(M5PM1_IRQ_MASK_DISABLE), "Unmask PM1 button IRQ") != ESP_OK) {
        return ESP_FAIL;
    }

    // 清除所有 PM1 挂起的 IRQ 标志
    pm1.irqClearGpioAll();
    pm1.irqClearSysAll();
    pm1.irqClearBtnAll();
    // 等待 PM1 处理完成并将 IRQ 线拉高（OD 输出，依赖上拉）
    vTaskDelay(pdMS_TO_TICKS(20));

    int pm1_irq_level = gpio_get_level(Pin_P4_PMIC_IRQ);
    ESP_LOGI(TAG, "PM1_IRQ level before deep sleep: %d", pm1_irq_level);
    ESP_LOGI(TAG, "Starting deep sleep...");

    // PM1 配置完成后不再需要 I2C，释放 I2C GPIO，避免 L3B 断电外设被 SDA/SCL 反灌。
    corep4_gpio_set_high_z(Pin_COREP4_INTERNAL_I2C_SCL);
    corep4_gpio_set_high_z(Pin_COREP4_INTERNAL_I2C_SDA);
    if (!wake_touch) {
        corep4_gpio_set_high_z(Pin_P4_TP_INT);
    } else {
        // CST3XX INT 为 active-low 开漏输出，需要上拉使引脚在非触摸时维持高电平。
        // ext1_wakeup_prepare 内部会调用 rtcio_hal_hold_enable，将此上拉配置冻结进深度睡眠态。
        rtc_gpio_pulldown_dis((gpio_num_t)Pin_P4_TP_INT);
        rtc_gpio_pullup_en((gpio_num_t)Pin_P4_TP_INT);
        ESP_LOGI(TAG, "LP GPIO%d pull-up enabled for touch EXT1 wakeup", Pin_P4_TP_INT);
    }

    // 只保持 PHY/C6 复位脚为 LOW。不要使用 gpio_force_hold_all()，否则会冻住 Flash/UART 并导致 WDT。
    corep4_hold_sleep_reset_pins();
    esp_deep_sleep_start();

    // 注意：这里的代码不会执行，因为设备会重启
    return ESP_OK;
}

esp_err_t m5_corep4_tool::sleep_before_work()
{
    ESP_LOGI(TAG, "=== Sleep Before Work - Preparing for Deep Sleep ===");

    // 获取唤醒配置
    bool wake_rtc = device_status.sleep.wake_rtc_enable;
    bool wake_touch = device_status.sleep.wake_touch_enable;
    bool wake_bmi = device_status.sleep.wake_bmi_enable;
    uint32_t wake_rtc_sec = device_status.sleep.wake_rtc_seconds;
    bool touch_sleep_handled = false;

    esp_err_t demo_stop_ret = stop_lvgl_custom_demo_task(1500);
    if (demo_stop_ret != ESP_OK) {
        ESP_LOGW(TAG, "LVGL custom demo task did not stop cleanly before sleep: %s", esp_err_to_name(demo_stop_ret));
    }

    esp_err_t imu_sleep_ret = imu_prepare_for_sleep(wake_bmi);
    if (imu_sleep_ret != ESP_OK) {
        ESP_LOGW(TAG, "IMU prepare for sleep reported: %s", esp_err_to_name(imu_sleep_ret));
    }

    // 1. 先停止主动外设，再关闭供电轨，避免 GPIO 反向给已断电外设供电。
    if (ethernet_initialized) {
        ESP_LOGI(TAG, "Peripheral: Ethernet -> deinit");
        esp_err_t eth_ret = ethernet_init(false);
        if (eth_ret != ESP_OK) {
            ESP_LOGW(TAG, "Ethernet deinit before sleep failed: %s", esp_err_to_name(eth_ret));
        }
    }

    corep4_gpio_output_low(Pin_P4_RMII_RST);  // GPIO52 = LOW，IP101G 进入硬件复位
    // 注意：ESP32-P4 rev < 3.0 单 pin hold 对 HP GPIO 不可靠。
    // enter_deep_sleep() 结尾会通过 PMU 的 HP_SLEEP hp_pad_hold_all 保持睡眠态 HP pad。
    static const gpio_num_t kEthernetSleepPins[] = {
        Pin_P4_RMII_CLK,
        Pin_P4_RMII_MDC,
        Pin_P4_RMII_MDIO,
        Pin_P4_RMII_TXEN,
        Pin_P4_RMII_TXD0,
        Pin_P4_RMII_TXD1,
        Pin_P4_RMII_RXDV,
        Pin_P4_RMII_RXD0,
        Pin_P4_RMII_RXD1,
    };
    corep4_gpio_set_high_z_list(kEthernetSleepPins, sizeof(kEthernetSleepPins) / sizeof(kEthernetSleepPins[0]));
    ESP_LOGI(TAG, "Ethernet PHY reset asserted (held LOW), RMII GPIOs set to high-Z");

    // ESP32-C6 WiFi模块复位处理
    // C6的SDA(GPIO11)/SCL(GPIO9)与内部I2C总线共用：若C6处于运行状态会持续驱动这两根线，
    // 使M5IOE1的I2C睡眠计时器不断复位，M5IOE1永远无法进入低功耗状态。
    // 需要在释放I2C GPIO之前拉低RESET，确保C6不在深度睡眠期间驱动I2C总线。
    corep4_gpio_output_low(Pin_P4_WIFI_RESET);   // GPIO42 = LOW，C6 进入复位
    // hold 由 enter_deep_sleep() 结尾的 PMU HP_SLEEP hold_all 统一完成，不在此处单独调用。
    corep4_gpio_set_high_z(Pin_P4_WIFI_WAKE);    // WAKE引脚高阻
    static const gpio_num_t kWifiSleepPins[] = {
        Pin_P4_WIFI_SD2_CLK,
        Pin_P4_WIFI_SD2_CMD,
        Pin_P4_WIFI_SD2_D0,
        Pin_P4_WIFI_SD2_D1,
        Pin_P4_WIFI_SD2_D2,
        Pin_P4_WIFI_SD2_D3,
    };
    corep4_gpio_set_high_z_list(kWifiSleepPins, sizeof(kWifiSleepPins) / sizeof(kWifiSleepPins[0]));
    ESP_LOGI(TAG, "WiFi C6 reset asserted (GPIO42=LOW), SDIO GPIOs set to high-Z");

    ESP_LOGI(TAG, "Peripheral: Audio system -> deinit");
    esp_err_t audio_ret = otool_tools.audio.audio_system_deinit(/*for_deep_sleep=*/true);
    if (audio_ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio deinit before sleep failed: %s", esp_err_to_name(audio_ret));
    }
    device_status.audio.es8311_initialized = false;
    device_status.audio.es7210_initialized = false;
    device_status.audio.audio_power_on = false;
    device_status.audio.is_playing = false;

    static const gpio_num_t kAudioSleepPins[] = {
        Pin_P4_I2S_BCLK_PIN,
        Pin_P4_I2S_MCLK_PIN,
        Pin_P4_I2S_DATA_IN_PIN,
        Pin_P4_I2S_DATA_OUT_PIN,
        Pin_P4_I2S_DATA_WS_PIN,
    };
    corep4_gpio_set_high_z_list(kAudioSleepPins, sizeof(kAudioSleepPins) / sizeof(kAudioSleepPins[0]));
    ESP_LOGI(TAG, "I2S GPIOs set to high-Z");

    esp_err_t sd_sleep_ret = corep4_sd_card_deinit_for_sleep(otool_tools.sd_card);
    if (sd_sleep_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD prepare for sleep reported: %s", esp_err_to_name(sd_sleep_ret));
    }
    device_status.sdcard.sd_initialized = false;
    device_status.sdcard.sd_total_bytes = 0;
    device_status.sdcard.sd_free_bytes = 0;
    corep4_sd_detect_disable_for_sleep(pyb);

    // 2. 供电输出管理（统一关闭，避免重复操作）
    ESP_LOGI(TAG, "Power: LCD brightness -> 0");
    set_status(StatusParam::POWER_LCD_BRIGHTNESS, static_cast<uint8_t>(0));

    lvgl_port_ready = false;

    if (lvgl_touch_indev != nullptr) {
        ESP_LOGI(TAG, "LVGL: remove touch input");
        esp_err_t lv_touch_ret = lvgl_port_remove_touch(lvgl_touch_indev);
        if (lv_touch_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to remove LVGL touch input: %s", esp_err_to_name(lv_touch_ret));
        }
        lvgl_touch_indev = nullptr;
    }

    if (lvgl_disp != nullptr) {
        ESP_LOGI(TAG, "LVGL: remove display and stop port");
        esp_err_t lv_disp_ret = lvgl_port_remove_disp(lvgl_disp);
        if (lv_disp_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to remove LVGL display: %s", esp_err_to_name(lv_disp_ret));
        }
        lvgl_disp = nullptr;
        lvgl_touch_indicator = nullptr;
        esp_err_t lv_deinit_ret = lvgl_port_deinit();
        if (lv_deinit_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to deinit LVGL port: %s", esp_err_to_name(lv_deinit_ret));
        }
    }

    if (!wake_touch) {
        if (tp_handle != nullptr) {
            ESP_LOGI(TAG, "Touch: sleep and delete handle");
            touch_enter_sleep();
            esp_lcd_touch_del(tp_handle);
            tp_handle = nullptr;
            touch_sleep_handled = true;
        }
        if (tp_io_handle != nullptr) {
            esp_err_t tp_io_ret = esp_lcd_panel_io_del(tp_io_handle);
            if (tp_io_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to delete touch IO: %s", esp_err_to_name(tp_io_ret));
            }
            tp_io_handle = nullptr;
        }
        pyb.pinMode(Pin_PYB_TP_RST, OUTPUT);
        pyb.setDriveMode(Pin_PYB_TP_RST, M5IOE1_DRIVE_OPENDRAIN);
        pyb.digitalWrite(Pin_PYB_TP_RST, LOW);
        corep4_gpio_set_high_z(Pin_P4_TP_INT);
    }

    if (lcd_panel_handle != nullptr) {
        ESP_LOGI(TAG, "LCD: display off and delete panel");
        esp_err_t disp_ret = esp_lcd_panel_disp_on_off(lcd_panel_handle, false);
        if (disp_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to turn LCD display off: %s", esp_err_to_name(disp_ret));
        }
        esp_err_t panel_ret = esp_lcd_panel_del(lcd_panel_handle);
        if (panel_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete LCD panel: %s", esp_err_to_name(panel_ret));
        }
        lcd_panel_handle = nullptr;
        device_status.screen.lcd_initialized = false;
        device_status.screen.lcd_power_on = false;
    }

    if (mipi_dbi_io != nullptr) {
        ESP_LOGI(TAG, "LCD: delete MIPI DBI IO");
        esp_err_t dbi_ret = esp_lcd_panel_io_del(mipi_dbi_io);
        if (dbi_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete MIPI DBI IO: %s", esp_err_to_name(dbi_ret));
        }
        mipi_dbi_io = nullptr;
    }

    if (mipi_dsi_bus != nullptr) {
        ESP_LOGI(TAG, "LCD: delete MIPI DSI bus");
        esp_err_t dsi_ret = esp_lcd_del_dsi_bus(mipi_dsi_bus);
        if (dsi_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete MIPI DSI bus: %s", esp_err_to_name(dsi_ret));
        }
        mipi_dsi_bus = nullptr;
    }

    if (lcd_refresh_finish != nullptr) {
        vSemaphoreDelete(lcd_refresh_finish);
        lcd_refresh_finish = nullptr;
    }

    // corep4.init()/reset_lcd() 会释放 LCD_RST。LCD 断电或休眠前必须重新拉低，
    // 否则未供电的 LCD 可能通过复位脚被反灌，造成 mA级漏电。

    ESP_LOGI(TAG, "Power: LCD -> OFF");
    set_status(StatusParam::POWER_LCD, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::APPLY_HARDWARE);
    // LVGL 已 deinit，跳过 SYNC_UI 避免 lvgl_port_lock assert

    if (ldo_mipi_phy) {
        ESP_LOGI(TAG, "Power: MIPI DSI PHY LDO -> release");
        esp_err_t ldo_ret = esp_ldo_release_channel(ldo_mipi_phy);
        if (ldo_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to release MIPI DSI PHY LDO before sleep: %s", esp_err_to_name(ldo_ret));
        } else {
            ldo_mipi_phy = nullptr;
        }
    }

    ESP_LOGI(TAG, "Power: Audio PA -> OFF");
    set_status(StatusParam::POWER_AUDIO_PA, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::APPLY_HARDWARE);

    ESP_LOGI(TAG, "Power: Audio Chip -> OFF");
    set_status(StatusParam::POWER_AUDIO_CHIP, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::APPLY_HARDWARE);

    ESP_LOGI(TAG, "Power: Grove -> OFF");
    set_status(StatusParam::POWER_GROVE, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::APPLY_HARDWARE);

    ESP_LOGI(TAG, "Power: P4USBC -> OFF");
    set_status(StatusParam::POWER_P4USBC, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::APPLY_HARDWARE);

    ESP_LOGI(TAG, "Power: PYB USBA -> OFF");
    set_status(StatusParam::POWER_PYB_USBA, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::APPLY_HARDWARE);

    // 3. BMI270/BMM150已在sleep_before_work开头完成任务停止、中断输出、AUX上拉和PWR_CTRL收敛。
    ESP_LOGI(TAG, "IMU sleep state prepared (motion_wake=%s)", wake_bmi ? "on" : "off");

    // 4. 配置/关闭RX8130定时器中断（RTC唤醒走RX8130，不走PM1定时器）
    {
        auto rtc_device_handle = otool_tools.i2c_tools.get_i2c_device_handle(I2C_ADDR_RTC);
        bool ret = rtc.init(otool_tools.i2c_tools.get_i2c_bus_handle(), &rtc_device_handle, GPIO_NUM_NC);
        if (ret) {
            if (wake_rtc) {
                uint32_t seconds_32 = (wake_rtc_sec == 0) ? 10 : wake_rtc_sec;
                uint16_t seconds = (seconds_32 > 0xFFFF) ? 0xFFFF : static_cast<uint16_t>(seconds_32);
                rtc.clearIrqFlags();
                rtc.setTimerIrq(seconds);
                ESP_LOGI(TAG, "Enabled RX8130 timer IRQ after %u seconds", seconds);
            } else {
                rtc.disableIrq();
                rtc.clearIrqFlags();
                ESP_LOGI(TAG, "RX8130 IRQ disabled and flags cleared");
            }
        } else {
            ESP_LOGW(TAG, "Failed to initialize RTC, skipping RX8130 IRQ config");
        }
    }

    // 5. 先禁用LVGL触摸输入设备，防止LVGL任务在触摸屏休眠后尝试读取
    if (lvgl_touch_indev != nullptr) {
        ESP_LOGI(TAG, "Disabling LVGL touch input device...");
        if (lvgl_port_lock(1000)) {
            lv_indev_enable(lvgl_touch_indev, false);
            lvgl_port_unlock();
            ESP_LOGI(TAG, "LVGL touch input device disabled");
        } else {
            ESP_LOGW(TAG, "Failed to acquire LVGL lock, forcing touch disable");
            lv_indev_enable(lvgl_touch_indev, false);
        }
        // 等待确保LVGL任务完成当前的触摸读取操作
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 6. 根据触摸唤醒配置决定触摸屏处理
    if (!wake_touch) {
        // 不需要触摸唤醒，触摸屏进入休眠模式
        ESP_LOGI(TAG, "Putting touch screen to sleep...");
        if (!touch_sleep_handled) {
            touch_enter_sleep();
        }
    } else {
        ESP_LOGI(TAG, "Touch screen kept active for touch wake");
    }

    // 7. 最后关闭依赖 I2C 收尾的电源轨，避免配置 RTC/BMI 时给已断电外设反灌。
    // 注意：CST3XX 始终有电（功耗分析仪验证），深度睡眠期间照常工作。
    // GPIO1 LP 上拉和 EXT1 唤醒配置见 enter_deep_sleep()。
    // L3B 是独立扩展外设轨，与 TP 供电无关。
    ESP_LOGI(TAG, "Power: PYB L3B -> OFF");
    set_status(StatusParam::POWER_PYB_L3B, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::APPLY_HARDWARE);

    ESP_LOGI(TAG, "Power: M5BUS -> OFF");
    set_status(StatusParam::POWER_M5BUS, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::APPLY_HARDWARE);

    // 8. 等待一段时间确保所有设备进入休眠状态
    vTaskDelay(pdMS_TO_TICKS(100));

    // 9. 配置PM1和PYB I2C休眠参数。SD detect 已在关闭 L3B 前收敛，避免插卡时上拉漏电。
    pm1.setI2cSleepTime(3);           // 设置PM1 I2C休眠时间为3s，降低功耗
    pyb.setI2cSleepTime(3);           // 设置PYB I2C休眠时间为3s，降低功耗
    // 注意：pyb.setI2cSleepTime(3)内部会调用_updatePollingForI2cSleep(3)
    // 自动停止m5ioe1_poll任务（_cleanupPolling），无需手动停止轮询任务
    ESP_LOGI(TAG, "PM1 and PYB I2C sleep parameters configured");

    ESP_LOGI(TAG, "=== Sleep Before Work - Completed ===");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::test_pyb_io_diagnostics()
{
    ESP_LOGI("PYB_DIAG", "=== PYB IO Diagnostics ===");
    ESP_LOGI("PYB_DIAG", "I2C Address: 0x%02X", M5IOE1_DEFAULT_ADDR);
    // ESP_LOGI("PYB_DIAG", "Register Map:");
    // ESP_LOGI("PYB_DIAG", "  0x00-0x01: UID, 0x02: VERSION");
    // ESP_LOGI("PYB_DIAG", "  0x03-0x04: GPIO_MODE, 0x05-0x06: GPIO_OUT, 0x07-0x08: GPIO_IN");
    // ESP_LOGI("PYB_DIAG", "  0x09-0x0A: GPIO_PU, 0x0B-0x0C: GPIO_PD");
    // ESP_LOGI("PYB_DIAG", "  0x0D-0x0E: GPIO_IE, 0x0F-0x10: GPIO_IT, 0x11-0x12: GPIO_IS");
    // ESP_LOGI("PYB_DIAG", "  0x13-0x14: GPIO_DRV, 0x1B-0x22: PWM1-4, 0x25-0x26: PWM_FREQ");
    
    // 读取所有关键寄存器并显示寄存器地址
    uint16_t mode_reg = 0, output_reg = 0, input_reg = 0;
    uint16_t pu_reg = 0, pd_reg = 0, drv_reg = 0;
    
    if (pyb.getModeReg(&mode_reg) == M5IOE1_OK) {
        ESP_LOGI("PYB_DIAG", "REG[0x03-0x04] GPIO_MODE: %d%d%d%d %d%d%d%d %d%d%d%d %d%dXX (IO1-14)", 
                 (mode_reg>>0)&1, (mode_reg>>1)&1, (mode_reg>>2)&1, (mode_reg>>3)&1,
                 (mode_reg>>4)&1, (mode_reg>>5)&1, (mode_reg>>6)&1, (mode_reg>>7)&1,
                 (mode_reg>>8)&1, (mode_reg>>9)&1, (mode_reg>>10)&1, (mode_reg>>11)&1,
                 (mode_reg>>12)&1, (mode_reg>>13)&1);
    }
    if (pyb.getOutputReg(&output_reg) == M5IOE1_OK) {
        ESP_LOGI("PYB_DIAG", "REG[0x05-0x06] GPIO_OUT:  %d%d%d%d %d%d%d%d %d%d%d%d %d%dXX (IO1-14)", 
                 (output_reg>>0)&1, (output_reg>>1)&1, (output_reg>>2)&1, (output_reg>>3)&1,
                 (output_reg>>4)&1, (output_reg>>5)&1, (output_reg>>6)&1, (output_reg>>7)&1,
                 (output_reg>>8)&1, (output_reg>>9)&1, (output_reg>>10)&1, (output_reg>>11)&1,
                 (output_reg>>12)&1, (output_reg>>13)&1);
    }
    if (pyb.getInputReg(&input_reg) == M5IOE1_OK) {
        ESP_LOGI("PYB_DIAG", "REG[0x07-0x08] GPIO_IN:   %d%d%d%d %d%d%d%d %d%d%d%d %d%dXX (IO1-14)", 
                 (input_reg>>0)&1, (input_reg>>1)&1, (input_reg>>2)&1, (input_reg>>3)&1,
                 (input_reg>>4)&1, (input_reg>>5)&1, (input_reg>>6)&1, (input_reg>>7)&1,
                 (input_reg>>8)&1, (input_reg>>9)&1, (input_reg>>10)&1, (input_reg>>11)&1,
                 (input_reg>>12)&1, (input_reg>>13)&1);
    }
    if (pyb.getPullUpReg(&pu_reg) == M5IOE1_OK) {
        ESP_LOGI("PYB_DIAG", "REG[0x09-0x0A] GPIO_PU:   %d%d%d%d %d%d%d%d %d%d%d%d %d%dXX (IO1-14)", 
                 (pu_reg>>0)&1, (pu_reg>>1)&1, (pu_reg>>2)&1, (pu_reg>>3)&1,
                 (pu_reg>>4)&1, (pu_reg>>5)&1, (pu_reg>>6)&1, (pu_reg>>7)&1,
                 (pu_reg>>8)&1, (pu_reg>>9)&1, (pu_reg>>10)&1, (pu_reg>>11)&1,
                 (pu_reg>>12)&1, (pu_reg>>13)&1);
    }
    if (pyb.getPullDownReg(&pd_reg) == M5IOE1_OK) {
        ESP_LOGI("PYB_DIAG", "REG[0x0B-0x0C] GPIO_PD:   %d%d%d%d %d%d%d%d %d%d%d%d %d%dXX (IO1-14)", 
                 (pd_reg>>0)&1, (pd_reg>>1)&1, (pd_reg>>2)&1, (pd_reg>>3)&1,
                 (pd_reg>>4)&1, (pd_reg>>5)&1, (pd_reg>>6)&1, (pd_reg>>7)&1,
                 (pd_reg>>8)&1, (pd_reg>>9)&1, (pd_reg>>10)&1, (pd_reg>>11)&1,
                 (pd_reg>>12)&1, (pd_reg>>13)&1);
    }
    if (pyb.getDriveReg(&drv_reg) == M5IOE1_OK) {
        ESP_LOGI("PYB_DIAG", "REG[0x13-0x14] GPIO_DRV:  %d%d%d%d %d%d%d%d %d%d%d%d %d%dXX (IO1-14, 0=push-pull, 1=open-drain)", 
                 (drv_reg>>0)&1, (drv_reg>>1)&1, (drv_reg>>2)&1, (drv_reg>>3)&1,
                 (drv_reg>>4)&1, (drv_reg>>5)&1, (drv_reg>>6)&1, (drv_reg>>7)&1,
                 (drv_reg>>8)&1, (drv_reg>>9)&1, (drv_reg>>10)&1, (drv_reg>>11)&1,
                 (drv_reg>>12)&1, (drv_reg>>13)&1);
    }

    // 打印 PWM 频率
    uint16_t pwm_freq = 0;
    if (pyb.getPwmFrequency(&pwm_freq) == M5IOE1_OK) {
        ESP_LOGI("PYB_DIAG", "REG[0x25-0x26] PWM_FREQ: %u Hz", pwm_freq);
    } else {
        ESP_LOGW("PYB_DIAG", "Read PWM frequency failed");
    }
    
    // 打印 PWM1-4 状态: 通道->绑定IO, 使能, 极性, 占空比(百分比与原始12bit)
    for (int ch = 0; ch < 4; ++ch) {
        uint8_t duty_pct = 0; bool pol = false; bool en = false; uint16_t duty12 = 0;
        if (pyb.getPwmDuty12bit(ch, &duty12, &pol, &en) == M5IOE1_OK) {
            duty_pct = (uint8_t)((duty12 * 100) / 0x0FFF);
            // 逆查通道对应 IO (见手册: PWM1->IO9, PWM2->IO8, PWM3->IO11, PWM4->IO10)
            int io = -1;
            switch(ch){
                case 0: io = 9; break;  // PWM1 IO9
                case 1: io = 8; break;  // PWM2 IO8
                case 2: io = 11; break; // PWM3 IO11
                case 3: io = 10; break; // PWM4 IO10
            }
            ESP_LOGI("PYB_DIAG", "PWM%d (IO%d) EN=%d POL=%d DUTY=%3u%% (0x%03X/0xFFF)",
                     ch+1, io, en?1:0, pol?1:0, duty_pct, duty12);
        } else {
            ESP_LOGW("PYB_DIAG", "PWM%d read failed", ch+1);
        }
    }
    
    // ESP_LOGW("PYB_DIAG", "*** IMPORTANT: If you see 'Brownout detector was triggered', check power supply stability! ***");
    
    return ESP_OK;
}


// 初始化 显示屏
esp_err_t m5_corep4_tool::lcd_init()
{
    ESP_LOGI(TAG, "Initializing LCD START");

    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
    if (ldo_mipi_phy == nullptr) {
        esp_ldo_channel_config_t ldo_mipi_phy_config;
        memset(&ldo_mipi_phy_config, 0, sizeof(ldo_mipi_phy_config));
        ldo_mipi_phy_config.chan_id = LCD_MIPI_DSI_PHY_PWR_LDO_CHAN;
        ldo_mipi_phy_config.voltage_mv = LCD_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV;
        ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    } else {
        ESP_LOGW(TAG, "MIPI DSI PHY LDO already acquired");
    }

    ESP_LOGI(TAG, "Initialize MIPI DSI bus");
    esp_lcd_dsi_bus_config_t bus_config = {};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = LCD_MIPI_DSI_LANE_NUM;
    bus_config.lane_bit_rate_mbps = LCD_MIPI_DSI_LANE_BITRATE_MBPS;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    bus_config.flags.clock_lane_force_hs = 0;
#endif
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

        ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &mipi_dbi_io));

    ESP_LOGI(TAG, "Install LCD driver");
    esp_lcd_dpi_panel_config_t dpi_config;
    memset(&dpi_config, 0, sizeof(dpi_config));
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = LCD_MIPI_DSI_DPI_CLK_MHZ;
    dpi_config.in_color_format = LCD_MIPI_DSI_COLOR_DEPTH;
    dpi_config.out_color_format = LCD_MIPI_DSI_COLOR_DEPTH;
    dpi_config.num_fbs = 2;
    dpi_config.video_timing.h_size = LCD_MIPI_DSI_LCD_H_RES;
    dpi_config.video_timing.v_size = LCD_MIPI_DSI_LCD_V_RES;
    dpi_config.video_timing.hsync_pulse_width = LCD_MIPI_DSI_LCD_HSYNC;
    dpi_config.video_timing.hsync_back_porch = LCD_MIPI_DSI_LCD_HBP;
    dpi_config.video_timing.hsync_front_porch = LCD_MIPI_DSI_LCD_HFP;
    dpi_config.video_timing.vsync_pulse_width = LCD_MIPI_DSI_LCD_VSYNC;
    dpi_config.video_timing.vsync_back_porch = LCD_MIPI_DSI_LCD_VBP;
    dpi_config.video_timing.vsync_front_porch = LCD_MIPI_DSI_LCD_VFP;

    st7102_vendor_config_t vendor_config;
    memset(&vendor_config, 0, sizeof(vendor_config));
    vendor_config.init_cmds = st7102_init_cmds;
    vendor_config.init_cmds_size = sizeof(st7102_init_cmds) / sizeof(st7102_init_cmds[0]);
    vendor_config.init_in_command_mode = false;
    vendor_config.mipi_config.dsi_bus = mipi_dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;

    esp_lcd_panel_dev_config_t lcd_dev_config;
    memset(&lcd_dev_config, 0, sizeof(lcd_dev_config));
    lcd_dev_config.reset_gpio_num = LCD_RST_PIN_NUM;
    lcd_dev_config.rgb_ele_order = LCD_COLOR_SPACE;
    lcd_dev_config.bits_per_pixel = LCD_BITS_PER_PIXEL;
    lcd_dev_config.data_endian = static_cast<decltype(lcd_dev_config.data_endian)>(0);
    lcd_dev_config.vendor_config = &vendor_config;

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7102(mipi_dbi_io, &lcd_dev_config, &lcd_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel_handle, true));

    // 创建LCD刷新完成信号量
    lcd_refresh_finish = xSemaphoreCreateBinary();
    if (lcd_refresh_finish == NULL) {
        ESP_LOGE(TAG, "Failed to create LCD refresh semaphore");
        return ESP_ERR_NO_MEM;
    }
    
    // 注册LCD事件回调
    esp_lcd_dpi_panel_event_callbacks_t lcd_cbs = {
        .on_color_trans_done = [](esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx) -> bool {
            SemaphoreHandle_t refresh_finish = (SemaphoreHandle_t)user_ctx;
            BaseType_t high_task_wakeup = pdFALSE;
            xSemaphoreGiveFromISR(refresh_finish, &high_task_wakeup);
            return high_task_wakeup == pdTRUE;
        },
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
        .on_frame_buf_complete = nullptr,
        .on_vsync = nullptr,
#else
        .on_refresh_done = nullptr,
#endif
    };

    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(lcd_panel_handle, &lcd_cbs, lcd_refresh_finish));

    ESP_LOGI(TAG, "LCD Initialized");
    return ESP_OK;
}

// LCD清屏函数
void m5_corep4_tool::lcd_clear(esp_lcd_panel_handle_t panel_handle,
                               uint16_t h_res, uint16_t v_res,
                               bool is_white)
{
    // 根据LCD_MIPI_DSI_COLOR_DEPTH确定byte_per_pixel
    uint8_t byte_per_pixel;

    if(LCD_MIPI_DSI_COLOR_DEPTH == LCD_COLOR_PIXEL_FORMAT_RGB565)
    {
        byte_per_pixel = 2;  // RGB565: 2字节
    }
    else if(LCD_MIPI_DSI_COLOR_DEPTH == LCD_COLOR_PIXEL_FORMAT_RGB888)
    {
        byte_per_pixel = 3;  // RGB888: 3字节
    }
    else
    {
        ESP_LOGE(TAG, "Unsupported LCD_MIPI_DSI_COLOR_DEPTH");
        return;
    }


    // 每行的字节数
    size_t line_bytes = h_res * byte_per_pixel;
    
    // 分配行缓存 - 使用PSRAM而不是DMA内存，避免耗尽内部RAM
    // DMA2D可以直接访问PSRAM，不需要使用MALLOC_CAP_DMA
    uint8_t *line_buf = (uint8_t *)heap_caps_malloc(line_bytes, MALLOC_CAP_SPIRAM);
    if (!line_buf) {
        ESP_LOGE(TAG, "OOM allocating clear line buffer (%zu bytes)", line_bytes);
        return;
    }
    
    // 根据颜色深度填充缓冲区
    if (byte_per_pixel == 2) {
        // RGB565格式
        uint16_t *buf16 = (uint16_t *)line_buf;
        uint16_t color = is_white ? 0xFFFF : 0x0000;  // 白色或黑色
        for (int x = 0; x < h_res; x++) {
            buf16[x] = color;
        }
    } else {
        // RGB888格式  
        uint8_t color = is_white ? 0xFF : 0x00;  // 白色或黑色
        for (int x = 0; x < h_res; x++) {
            line_buf[x * 3 + 0] = color;  // R
            line_buf[x * 3 + 1] = color;  // G
            line_buf[x * 3 + 2] = color;  // B
        }
    }
    
    // ESP_LOGI(TAG, "Clearing screen: %ux%u, %s, bpp=%d", 
    //          h_res, v_res, is_white ? "WHITE" : "BLACK", byte_per_pixel);
    
    // 逐行清屏
    for (int y = 0; y < v_res; y++) {
        esp_lcd_panel_draw_bitmap(panel_handle,
                                  0, y,            // x0, y0
                                  h_res, y + 1,    // x1, y1 （绘制单行）
                                  line_buf);
        // 等待本行刷新完毕
        if (lcd_refresh_finish) {
            xSemaphoreTake(lcd_refresh_finish, portMAX_DELAY);
        }
    }
    
    free(line_buf);
    ESP_LOGI(TAG, "Screen cleared successfully");
}

// LCD清屏函数 - 使用内部句柄的简化版本
esp_err_t m5_corep4_tool::lcd_clear(bool is_white)
{
    if (lcd_panel_handle == nullptr) {
        ESP_LOGE(TAG, "LCD panel not initialized, call lcd_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 自动获取屏幕分辨率（假设480x480，可以根据实际情况调整）
    lcd_clear(lcd_panel_handle, 480, 480, is_white);
    return ESP_OK;
}

// LCD绘制彩虹条纹函数 - 13色彩虹铺满全屏
void m5_corep4_tool::lcd_draw_rainbow_stripes(esp_lcd_panel_handle_t panel_handle,
                                              uint16_t h_res, uint16_t v_res,
                                              bool show_fullscreen_colors)
{
    ESP_LOGI(TAG, "Rainbow function called: handle=%p, resolution=%ux%u, show_fullscreen=%d", 
             panel_handle, h_res, v_res, show_fullscreen_colors);
    
    // 打印内存状态
    ESP_LOGI(TAG, "Memory status - Internal: %zu bytes free, SPIRAM: %zu bytes free", 
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    if (panel_handle == nullptr) {
        ESP_LOGE(TAG, "Rainbow function: panel_handle is null!");
        return;
    }
    
    // 根据LCD_MIPI_DSI_COLOR_DEPTH确定byte_per_pixel
    uint8_t byte_per_pixel;

    if(LCD_MIPI_DSI_COLOR_DEPTH == LCD_COLOR_PIXEL_FORMAT_RGB565)
    {
        byte_per_pixel = 2;  // RGB565: 2字节
    }
    else if(LCD_MIPI_DSI_COLOR_DEPTH == LCD_COLOR_PIXEL_FORMAT_RGB888)
    {
        byte_per_pixel = 3;  // RGB888: 3字节
    }
    else
    {
        ESP_LOGE(TAG, "Unsupported LCD_MIPI_DSI_COLOR_DEPTH");
        return;
    }

    // 定义14色渐变颜色（RGB888格式 - 从黑到白）
    // 格式: {R, G, B} 每个分量0-255
    // 渐变规律：黑色 -> 红色系 -> 黄色系 -> 绿色系 -> 青色系 -> 蓝色系 -> 紫色系 -> 白色
    struct RGB888Color {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        const char* name;
    };
    
    const RGB888Color rainbow_colors[] = {
        {0, 0, 0, "Black"},           // 1. 黑色 - 起点
        {128, 0, 0, "Dark Red"},      // 2. 暗红
        {255, 0, 0, "Red"},           // 3. 纯红
        {255, 128, 0, "Orange"},      // 4. 橙色
        {255, 255, 0, "Yellow"},      // 5. 纯黄
        {128, 255, 0, "Lime"},        // 6. 黄绿
        {0, 255, 0, "Green"},         // 7. 纯绿
        {0, 255, 128, "Spring Green"},// 8. 青绿
        {0, 255, 255, "Cyan"},        // 9. 纯青
        {0, 128, 255, "Sky Blue"},    // 10. 天蓝
        {0, 0, 255, "Blue"},          // 11. 纯蓝
        {128, 0, 255, "Purple"},      // 12. 紫色
        {255, 0, 255, "Magenta"},     // 13. 品红
        {255, 255, 255, "White"}      // 14. 白色 - 终点
    };
    const int num_colors = sizeof(rainbow_colors) / sizeof(rainbow_colors[0]);

    if (num_colors <= 0) {
        ESP_LOGE(TAG, "Rainbow function: invalid color count %d", num_colors);
        return;
    }

    // 每个条纹的基础高度（向下取整）以及剩余高度
    const uint16_t stripe_height = v_res / num_colors;
    const uint16_t remainder_height = v_res % num_colors;
    const uint16_t max_stripe_height = stripe_height + (remainder_height ? remainder_height : 0);

    ESP_LOGI(TAG,
             "Rainbow colors: %d, base_height=%u, remainder=%u, max_height=%u, bpp=%d",
             num_colors, stripe_height, remainder_height, max_stripe_height, byte_per_pixel);

    // 每个条带缓冲区的最大字节数（确保可容纳最高的条带）
    size_t stripe_bytes = h_res * max_stripe_height * byte_per_pixel;
    if (stripe_bytes == 0) {
        ESP_LOGE(TAG,
                 "Rainbow function: stripe_bytes is zero (h_res=%u, max_height=%u, bpp=%u)",
                 h_res, max_stripe_height, byte_per_pixel);
        return;
    }

    // 分配条带缓存 - 使用PSRAM而不是DMA内存，避免耗尽内部RAM
    // DMA2D可以直接访问PSRAM，不需要使用MALLOC_CAP_DMA
    uint8_t *stripe_buf = (uint8_t *)heap_caps_malloc(stripe_bytes, MALLOC_CAP_SPIRAM);
    if (!stripe_buf) {
        ESP_LOGE(TAG, "OOM allocating rainbow stripe buffer (%zu bytes)", stripe_bytes);
        return;
    }
    
    ESP_LOGI(TAG,
             "Drawing %d-color rainbow stripes: %ux%u, base_height=%u, max_height=%u, stripe_bytes=%zu",
             num_colors, h_res, v_res, stripe_height, max_stripe_height, stripe_bytes);
    
    // 逐条带绘制
    for (int stripe_idx = 0; stripe_idx < num_colors; stripe_idx++) {
        const RGB888Color& color888 = rainbow_colors[stripe_idx];
        
        // 计算当前条带的Y坐标范围
        uint16_t y_start = stripe_idx * stripe_height;
        uint16_t y_end = y_start + stripe_height;
        
        // 处理最后一个条带，确保覆盖到屏幕底部
        if (stripe_idx == num_colors - 1) {
            y_end = v_res;
        }
        
        uint16_t actual_height = y_end - y_start;
        
        // 根据颜色深度填充条带缓存
        if (byte_per_pixel == 2) {
            // RGB565格式：从RGB888转换到RGB565（高位到低位）
            uint16_t color565 = ((color888.r & 0xF8) << 8) |   // R: 取高5位
                               ((color888.g & 0xFC) << 3) |    // G: 取高6位
                               ((color888.b & 0xF8) >> 3);     // B: 取高5位
            
            uint16_t *buf16 = (uint16_t *)stripe_buf;
            for (int i = 0; i < h_res * actual_height; i++) {
                buf16[i] = color565;
            }
            
            ESP_LOGI(TAG, "Drawing stripe %d: %s RGB888(0x%02X%02X%02X) -> RGB565(0x%04X), y=%u-%u, height=%u", 
                     stripe_idx, color888.name, color888.r, color888.g, color888.b, color565, y_start, y_end, actual_height);
        } else {
            // RGB888格式：直接使用原始RGB888值
            for (int i = 0; i < h_res * actual_height; i++) {
                stripe_buf[i * 3 + 0] = color888.r;  // R
                stripe_buf[i * 3 + 1] = color888.g;  // G
                stripe_buf[i * 3 + 2] = color888.b;  // B
            }
            
            ESP_LOGI(TAG, "Drawing stripe %d: %s RGB888(0x%02X%02X%02X), y=%u-%u, height=%u", 
                     stripe_idx, color888.name, color888.r, color888.g, color888.b, y_start, y_end, actual_height);
        }
        
        // 绘制整个条带
        esp_lcd_panel_draw_bitmap(panel_handle,
                                  0, y_start,        // x0, y0
                                  h_res, y_end,      // x1, y1
                                  stripe_buf);
        
        // 等待条带刷新完毕
        if (lcd_refresh_finish) {
            xSemaphoreTake(lcd_refresh_finish, portMAX_DELAY);
        }
        
        ESP_LOGD(TAG, "Stripe %d completed", stripe_idx);
    }
    
    
    vTaskDelay(pdMS_TO_TICKS(1000)); // 等待1秒
    ESP_LOGI(TAG, "Rainbow stripes drawn successfully");
    
    // 先释放条带缓冲区，为全屏缓冲区腾出空间
    if (stripe_buf) {
        free(stripe_buf);
        stripe_buf = nullptr;
        ESP_LOGI(TAG, "Stripe buffer freed");
    }
    
    // 如果需要全屏显示每种颜色
    if (show_fullscreen_colors) {
        vTaskDelay(pdMS_TO_TICKS(2000)); // 等待2秒
        ESP_LOGI(TAG, "Starting fullscreen color display mode...");
        
        // 分配全屏缓冲区 - 480x480x3 = 691,200 字节
        size_t fullscreen_bytes = h_res * v_res * byte_per_pixel;
        ESP_LOGI(TAG, "Attempting to allocate fullscreen buffer: %zu bytes (%.2f KB)", 
                 fullscreen_bytes, fullscreen_bytes / 1024.0);
        
        uint8_t *fullscreen_buf = (uint8_t *)heap_caps_malloc(fullscreen_bytes, MALLOC_CAP_SPIRAM);
        if (!fullscreen_buf) {
            ESP_LOGE(TAG, "OOM allocating fullscreen buffer (%zu bytes)", fullscreen_bytes);
            ESP_LOGE(TAG, "Free SPIRAM: %zu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            return;
        }
        ESP_LOGI(TAG, "Fullscreen buffer allocated successfully at %p", fullscreen_buf);
        
        // 为文字分配缓冲区（用于绘制RGB编号）
        const int text_height = 40;
        const int text_width = 300;
        size_t text_bytes = text_width * text_height * byte_per_pixel;
        uint8_t *text_buf = (uint8_t *)heap_caps_malloc(text_bytes, MALLOC_CAP_SPIRAM);
        if (!text_buf) {
            ESP_LOGW(TAG, "Failed to allocate text buffer (%zu bytes), will display without RGB bars", text_bytes);
        } else {
            ESP_LOGI(TAG, "Text buffer allocated successfully at %p", text_buf);
        }
        
        // 逐个全屏显示每种颜色
        for (int color_idx = 0; color_idx < num_colors; color_idx++) {
            const RGB888Color& color = rainbow_colors[color_idx];
            
            ESP_LOGI(TAG, "Displaying fullscreen color %d/%d: %s RGB888(0x%02X%02X%02X)", 
                     color_idx + 1, num_colors, color.name, color.r, color.g, color.b);
            
            // 填充全屏缓冲区
            if (byte_per_pixel == 2) {
                // RGB565格式
                uint16_t color565 = ((color.r & 0xF8) << 8) |
                                   ((color.g & 0xFC) << 3) |
                                   ((color.b & 0xF8) >> 3);
                uint16_t *buf16 = (uint16_t *)fullscreen_buf;
                for (int i = 0; i < h_res * v_res; i++) {
                    buf16[i] = color565;
                }
            } else {
                // RGB888格式
                for (int i = 0; i < h_res * v_res; i++) {
                    fullscreen_buf[i * 3 + 0] = color.r;
                    fullscreen_buf[i * 3 + 1] = color.g;
                    fullscreen_buf[i * 3 + 2] = color.b;
                }
            }
            
            // 绘制全屏颜色
            esp_lcd_panel_draw_bitmap(panel_handle,
                                      0, 0,
                                      h_res, v_res,
                                      fullscreen_buf);
            
            if (lcd_refresh_finish) {
                xSemaphoreTake(lcd_refresh_finish, portMAX_DELAY);
            }
            
            // 如果有文字缓冲区，在屏幕顶部中间绘制RGB值
            if (text_buf) {
                ESP_LOGI(TAG, "Drawing RGB bar indicator: R=%d, G=%d, B=%d", color.r, color.g, color.b);
                
                // 填充白色背景
                if (byte_per_pixel == 2) {
                    uint16_t bg_color = 0xFFFF; // 白色
                    uint16_t *text16 = (uint16_t *)text_buf;
                    for (int i = 0; i < text_width * text_height; i++) {
                        text16[i] = bg_color;
                    }
                } else {
                    for (int i = 0; i < text_width * text_height; i++) {
                        text_buf[i * 3 + 0] = 255; // R
                        text_buf[i * 3 + 1] = 255; // G
                        text_buf[i * 3 + 2] = 255; // B
                    }
                }
                
                // 绘制3条RGB横线（代表RGB颜色的长度）
                const int bar_height = 8;  // 每条横线的高度
                const int bar_spacing = 2; // 横线之间的间距
                const int bar_start_x = 10; // 横线起始X位置（相对于text_buf）
                const int bar_max_width = text_width - 20; // 横线最大宽度
                
                // 计算每条横线的长度（根据RGB值0-255映射到0-bar_max_width）
                int r_bar_width = (color.r * bar_max_width) / 255;
                int g_bar_width = (color.g * bar_max_width) / 255;
                int b_bar_width = (color.b * bar_max_width) / 255;
                
                // 绘制R横线（红色）
                int r_bar_y = 5;
                for (int y = r_bar_y; y < r_bar_y + bar_height && y < text_height; y++) {
                    for (int x = bar_start_x; x < bar_start_x + r_bar_width && x < text_width; x++) {
                        int idx = y * text_width + x;
                        if (byte_per_pixel == 2) {
                            uint16_t *text16 = (uint16_t *)text_buf;
                            text16[idx] = 0xF800; // 红色 RGB565
                        } else {
                            text_buf[idx * 3 + 0] = 255; // R
                            text_buf[idx * 3 + 1] = 0;   // G
                            text_buf[idx * 3 + 2] = 0;   // B
                        }
                    }
                }
                
                // 绘制G横线（绿色）
                int g_bar_y = r_bar_y + bar_height + bar_spacing;
                for (int y = g_bar_y; y < g_bar_y + bar_height && y < text_height; y++) {
                    for (int x = bar_start_x; x < bar_start_x + g_bar_width && x < text_width; x++) {
                        int idx = y * text_width + x;
                        if (byte_per_pixel == 2) {
                            uint16_t *text16 = (uint16_t *)text_buf;
                            text16[idx] = 0x07E0; // 绿色 RGB565
                        } else {
                            text_buf[idx * 3 + 0] = 0;   // R
                            text_buf[idx * 3 + 1] = 255; // G
                            text_buf[idx * 3 + 2] = 0;   // B
                        }
                    }
                }
                
                // 绘制B横线（蓝色）
                int b_bar_y = g_bar_y + bar_height + bar_spacing;
                for (int y = b_bar_y; y < b_bar_y + bar_height && y < text_height; y++) {
                    for (int x = bar_start_x; x < bar_start_x + b_bar_width && x < text_width; x++) {
                        int idx = y * text_width + x;
                        if (byte_per_pixel == 2) {
                            uint16_t *text16 = (uint16_t *)text_buf;
                            text16[idx] = 0x001F; // 蓝色 RGB565
                        } else {
                            text_buf[idx * 3 + 0] = 0;   // R
                            text_buf[idx * 3 + 1] = 0;   // G
                            text_buf[idx * 3 + 2] = 255; // B
                        }
                    }
                }
                
                // 计算文字区域位置（屏幕顶部中间）
                int text_x = (h_res - text_width) / 2;
                int text_y = 10;
                
                ESP_LOGI(TAG, "Drawing RGB bars at x=%d, y=%d: R_width=%d, G_width=%d, B_width=%d", 
                         text_x, text_y, r_bar_width, g_bar_width, b_bar_width);
                
                // 绘制文字区域
                esp_lcd_panel_draw_bitmap(panel_handle,
                                          text_x, text_y,
                                          text_x + text_width, text_y + text_height,
                                          text_buf);
                
                if (lcd_refresh_finish) {
                    xSemaphoreTake(lcd_refresh_finish, portMAX_DELAY);
                }
            }
            
            // 显示每种颜色2秒
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        
        // 清理缓冲区
        if (fullscreen_buf) {
            ESP_LOGI(TAG, "Freeing fullscreen buffer at %p", fullscreen_buf);
            free(fullscreen_buf);
            fullscreen_buf = nullptr;
        }
        if (text_buf) {
            ESP_LOGI(TAG, "Freeing text buffer at %p", text_buf);
            free(text_buf);
            text_buf = nullptr;
        }
        
        ESP_LOGI(TAG, "Fullscreen color display completed");
        ESP_LOGI(TAG, "Free SPIRAM after cleanup: %zu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

// 获取LCD面板句柄
esp_lcd_panel_handle_t m5_corep4_tool::get_lcd_panel_handle(void)
{
    return lcd_panel_handle;
}

// 获取MIPI DBI I/O句柄
esp_lcd_panel_io_handle_t m5_corep4_tool::get_mipi_dbi_io_handle(void)
{
    return mipi_dbi_io;
}

// 获取LVGL显示对象
lv_display_t* m5_corep4_tool::get_lvgl_display(void)
{
    return lvgl_disp;
}

// 获取LVGL触摸输入设备
lv_indev_t* m5_corep4_tool::get_lvgl_touch_indev(void)
{
    return lvgl_touch_indev;
}

// 获取LVGL鼠标输入设备
lv_indev_t* m5_corep4_tool::get_lvgl_mouse_indev(void)
{
    return lvgl_mouse_indev;
}

// 获取LVGL鼠标光标对象
lv_obj_t* m5_corep4_tool::get_lvgl_mouse_cursor(void)
{
    return lvgl_mouse_cursor;
}

// 创建或重新创建LVGL鼠标光标
esp_err_t m5_corep4_tool::create_lvgl_mouse_cursor(void)
{
    // 如果光标已存在，先删除旧的
    if (lvgl_mouse_cursor != nullptr) {
        lv_obj_delete(lvgl_mouse_cursor);  // 使用正确的LVGL9 API
        lvgl_mouse_cursor = nullptr;
        g_mouse_cursor = nullptr;
    }
    
    // 创建新的鼠标光标（直径5像素的圆圈）
    lvgl_mouse_cursor = lv_obj_create(lv_screen_active());  // 使用lv_screen_active()替代lv_scr_act()
    if (lvgl_mouse_cursor == nullptr) {
        ESP_LOGE(TAG, "Failed to create mouse cursor object");
        return ESP_FAIL;
    }
    
    lv_obj_set_size(lvgl_mouse_cursor, 8, 8);
    lv_obj_set_style_radius(lvgl_mouse_cursor, 4, LV_PART_MAIN);  // 半径2.5近似为3
    lv_obj_set_style_bg_opa(lvgl_mouse_cursor, LV_OPA_TRANSP, LV_PART_MAIN);  // 背景透明
    lv_obj_set_style_border_width(lvgl_mouse_cursor, 1, LV_PART_MAIN);  // 1像素边框
    lv_obj_set_style_border_color(lvgl_mouse_cursor, lv_color_hex(0xFF0000), LV_PART_MAIN);  // 红色边框
    lv_obj_set_style_border_opa(lvgl_mouse_cursor, LV_OPA_COVER, LV_PART_MAIN);  // 边框不透明
    lv_obj_move_to_index(lvgl_mouse_cursor, -1);  // 置于最顶层
    lv_obj_add_flag(lvgl_mouse_cursor, LV_OBJ_FLAG_IGNORE_LAYOUT);  // 忽略布局
    lv_obj_add_flag(lvgl_mouse_cursor, LV_OBJ_FLAG_FLOATING);  // 浮动对象
    lv_obj_clear_flag(lvgl_mouse_cursor, LV_OBJ_FLAG_CLICKABLE);  // 不拦截鼠标事件，让底层控件能接收点击事件
    
    // 设置初始位置为当前鼠标位置（如果可用），否则设置为屏幕中央
    int16_t cursor_x = 240 - 2;  // 默认屏幕中央
    int16_t cursor_y = 240 - 2;
    
    if (g_mouse_connected) {
        cursor_x = g_mouse_data.x - 2;
        cursor_y = g_mouse_data.y - 2;
        // 确保坐标在有效范围内
        if (cursor_x < 0) cursor_x = 0;
        if (cursor_x > 475) cursor_x = 475;  // 480 - 5 = 475
        if (cursor_y < 0) cursor_y = 0;
        if (cursor_y > 475) cursor_y = 475;
    }
    
    lv_obj_set_pos(lvgl_mouse_cursor, cursor_x, cursor_y);
    
    // 将光标指针赋值给全局变量，供回调函数使用
    g_mouse_cursor = lvgl_mouse_cursor;
    
    // 根据LVGL文档，设置输入设备的光标对象
    if (lvgl_mouse_indev != NULL) {
        lv_indev_set_cursor(lvgl_mouse_indev, lvgl_mouse_cursor);
        ESP_LOGI(TAG, "LVGL mouse cursor set to input device");
    }
    
    ESP_LOGI(TAG, "LVGL mouse cursor created at position (%d, %d)", cursor_x + 2, cursor_y + 2);
    return ESP_OK;
}

// 动态创建LVGL鼠标输入设备
esp_err_t m5_corep4_tool::create_lvgl_mouse_input_device(void)
{
    // 检查LVGL显示是否已初始化
    if (lvgl_disp == nullptr) {
        ESP_LOGE(TAG, "LVGL display not initialized, cannot create mouse input device");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 如果鼠标输入设备已存在，需要检查光标是否也存在
    if (lvgl_mouse_indev != nullptr) {
        ESP_LOGI(TAG, "LVGL mouse input device already exists");
        // 检查光标是否存在，如果不存在则创建
        if (lvgl_mouse_cursor == nullptr || g_mouse_cursor == nullptr) {
            ESP_LOGI(TAG, "Mouse cursor missing, recreating...");
            if (create_lvgl_mouse_cursor() != ESP_OK) {
                ESP_LOGE(TAG, "Failed to recreate LVGL mouse cursor");
                return ESP_FAIL;
            }
        }
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Creating LVGL USB mouse input device...");
    
    // 创建鼠标输入设备
    lvgl_mouse_indev = lv_indev_create();
    if (lvgl_mouse_indev == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL mouse input device");
        return ESP_FAIL;
    }

    // 设置鼠标输入设备的属性
    lv_indev_set_type(lvgl_mouse_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvgl_mouse_indev, lvgl_mouse_read_cb);
    lv_indev_set_display(lvgl_mouse_indev, lvgl_disp);

    // 创建鼠标光标
    if (create_lvgl_mouse_cursor() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LVGL mouse cursor");
        // 清理已创建的输入设备
        if (lvgl_mouse_indev) {
            lv_indev_delete(lvgl_mouse_indev);
            lvgl_mouse_indev = nullptr;
        }
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "LVGL USB mouse input device created successfully");
    return ESP_OK;
}

// 获取LVGL触摸位置指示器对象
lv_obj_t* m5_corep4_tool::get_lvgl_touch_indicator(void)
{
    return lvgl_touch_indicator;
}

void m5_corep4_tool::set_lvgl_touch_indicator_enabled(bool enabled)
{
    lvgl_touch_indicator_enabled = enabled;
    if (!enabled && lvgl_touch_indicator) {
        lv_obj_add_flag(lvgl_touch_indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

esp_err_t m5_corep4_tool::set_lvgl_touch_indicator_preference(bool enabled)
{
    set_lvgl_touch_indicator_enabled(enabled);
    return save_system_preference_bool(NVS_KEY_TOUCH_INDICATOR, enabled);
}

bool m5_corep4_tool::get_lvgl_touch_indicator_enabled(void) const
{
    return lvgl_touch_indicator_enabled;
}

bool m5_corep4_tool::get_startup_prompt_audio_enabled(void) const
{
    return startup_prompt_audio_enabled;
}

esp_err_t m5_corep4_tool::set_startup_prompt_audio_enabled(bool enabled)
{
    startup_prompt_audio_enabled = enabled;
    return save_system_preference_bool(NVS_KEY_BOOT_SOUND, enabled);
}

uint32_t m5_corep4_tool::get_startup_prompt_audio_delay_ms(void) const
{
    return startup_prompt_audio_delay_ms;
}

esp_err_t m5_corep4_tool::set_startup_prompt_audio_delay_ms(uint32_t delay_ms)
{
    if (delay_ms > STARTUP_PROMPT_AUDIO_DELAY_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    startup_prompt_audio_delay_ms = delay_ms;
    return save_system_preference_u32(NVS_KEY_BOOT_SOUND_DELAY_MS, delay_ms);
}

// 创建或重新创建LVGL触摸位置指示器（蓝色圆圈）
esp_err_t m5_corep4_tool::create_lvgl_touch_indicator(void)
{
    // 如果指示器已存在，先删除旧的
    if (lvgl_touch_indicator != nullptr) {
        lv_obj_delete(lvgl_touch_indicator);
        lvgl_touch_indicator = nullptr;
    }
    
    // 在 LVGL 9 中，创建触摸指示器到 top layer（顶层），这样它会在所有屏幕上都可见
    // 使用 lv_layer_top() 获取顶层对象
    lv_obj_t *top_layer = lv_layer_top();
    if (top_layer == nullptr) {
        ESP_LOGE(TAG, "Failed to get top layer");
        return ESP_FAIL;
    }
    
    // 创建新的触摸位置指示器（蓝色圆圈）到顶层
    lvgl_touch_indicator = lv_obj_create(top_layer);
    if (lvgl_touch_indicator == nullptr) {
        ESP_LOGE(TAG, "Failed to create touch indicator object");
        return ESP_FAIL;
    }
    
    // 设置指示器样式（与鼠标光标相同，但颜色为蓝色）
    lv_obj_set_size(lvgl_touch_indicator, 8, 8);
    lv_obj_set_style_radius(lvgl_touch_indicator, 4, LV_PART_MAIN);  // 圆形（半径4）
    lv_obj_set_style_bg_opa(lvgl_touch_indicator, LV_OPA_TRANSP, LV_PART_MAIN);  // 背景透明
    lv_obj_set_style_border_width(lvgl_touch_indicator, 1, LV_PART_MAIN);  // 1像素边框
    lv_obj_set_style_border_color(lvgl_touch_indicator, lv_color_hex(0x0000FF), LV_PART_MAIN);  // 蓝色边框
    lv_obj_set_style_border_opa(lvgl_touch_indicator, LV_OPA_COVER, LV_PART_MAIN);  // 边框不透明
    lv_obj_move_to_index(lvgl_touch_indicator, -1);  // 置于最顶层
    lv_obj_add_flag(lvgl_touch_indicator, LV_OBJ_FLAG_IGNORE_LAYOUT);  // 忽略布局
    lv_obj_add_flag(lvgl_touch_indicator, LV_OBJ_FLAG_FLOATING);  // 浮动对象
    lv_obj_clear_flag(lvgl_touch_indicator, LV_OBJ_FLAG_CLICKABLE);  // 不拦截触摸事件
    
    // 初始位置设置在屏幕中央，并隐藏
    lv_obj_set_pos(lvgl_touch_indicator, 240 - 4, 240 - 4);
    lv_obj_add_flag(lvgl_touch_indicator, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏
    
    ESP_LOGI(TAG, "LVGL touch indicator created on top layer (blue circle)");
    return ESP_OK;
}

// 更新触摸位置指示器的位置
esp_err_t m5_corep4_tool::update_touch_indicator(int16_t x, int16_t y, bool pressed)
{
    static lv_obj_t *last_indicator = nullptr;
    static int16_t last_x = INT16_MIN;
    static int16_t last_y = INT16_MIN;
    static bool last_pressed = false;

    if (!lvgl_touch_indicator_enabled) {
        if (lvgl_touch_indicator) {
            lv_obj_add_flag(lvgl_touch_indicator, LV_OBJ_FLAG_HIDDEN);
        }
        last_pressed = false;
        return ESP_OK;
    }

    // 检查指示器是否已创建
    if (lvgl_touch_indicator == nullptr) {
        ESP_LOGW(TAG, "Touch indicator not created, creating now...");
        if (create_lvgl_touch_indicator() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create touch indicator");
            return ESP_FAIL;
        }
    }

    if (last_indicator != lvgl_touch_indicator) {
        last_indicator = lvgl_touch_indicator;
        last_x = INT16_MIN;
        last_y = INT16_MIN;
        last_pressed = false;
    }
    
    // 根据触摸状态显示或隐藏指示器
    if (pressed) {
        // 有触摸按下，更新位置并显示
        if (!last_pressed || x != last_x || y != last_y) {
            lv_obj_set_pos(lvgl_touch_indicator, x - 4, y - 4);  // 减去半径（4），使圆心对准触摸点
        }
        lv_obj_move_foreground(lvgl_touch_indicator);
        if (!last_pressed) {
            lv_obj_clear_flag(lvgl_touch_indicator, LV_OBJ_FLAG_HIDDEN);  // 显示指示器
        }
    } else if (last_pressed) {
        // 没有触摸，隐藏指示器
        lv_obj_add_flag(lvgl_touch_indicator, LV_OBJ_FLAG_HIDDEN);  // 隐藏指示器
    }

    last_x = x;
    last_y = y;
    last_pressed = pressed;
    
    return ESP_OK;
}

// 获取触摸屏I/O句柄
esp_lcd_panel_io_handle_t m5_corep4_tool::get_touch_io_handle(void)
{
    return tp_io_handle;
}

// 获取触摸屏设备句柄
esp_lcd_touch_handle_t m5_corep4_tool::get_touch_handle(void)
{
    return tp_handle;
}

// 初始化触摸
esp_err_t m5_corep4_tool::touch_init(void)
{
    ESP_LOGI(TAG, "Initializing Touch START");
    esp_err_t ret = ESP_OK;

    // 获取 i2c_master 句柄
    i2c_master_bus_handle_t touch_master_bus_handle = otool_tools.i2c_tools.get_i2c_master_bus_handle();

    ESP_LOGI(TAG, "Initialize touch panel start");
    esp_lcd_panel_io_i2c_config_t tp_io_config;
    memset(&tp_io_config, 0, sizeof(tp_io_config));
    tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST3XX_ADDRESS;
    tp_io_config.control_phase_bytes = 1;
    tp_io_config.dc_bit_offset = 0;
    tp_io_config.lcd_cmd_bits = 8;
    tp_io_config.lcd_param_bits = 8;
    tp_io_config.flags.dc_low_on_data = 0;
    tp_io_config.flags.disable_control_phase = 1;
    tp_io_config.on_color_trans_done = nullptr;
    tp_io_config.user_ctx = nullptr;
    tp_io_config.scl_speed_hz = 400000;
    // GPIO1 is only the interrupt line here; the touch IC itself is fed from the PYB/L3B side.
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = 480,
        .y_max = 480,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = Pin_P4_TP_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1,
        },
        .process_coordinates = NULL,
        .interrupt_callback = NULL,
        .user_data = NULL,
        .driver_data = NULL,
    };
    ret = esp_lcd_new_panel_io_i2c((i2c_master_bus_handle_t)touch_master_bus_handle, &tp_io_config, &tp_io_handle);
    if( ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C IO for Touch: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_lcd_touch_new_i2c_cst3xx(tp_io_handle, &tp_cfg, &tp_handle);
    if (tp_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create touch panel handle for Touch");
        esp_lcd_panel_io_del(tp_io_handle);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Touch boot flash checksum is only read during explicit CST3530 update flow");

    ESP_LOGI(TAG, "Touch Initialized");
    return ESP_OK;
}

/**
 * @brief 触摸测试演示 - 支持多点触摸轨迹显示
 * 
 * 使用esp_lcd直接绘制触摸轨迹，支持最多5个触摸点同时显示
 * 每个触摸点使用不同颜色的圆点显示轨迹
 */
esp_err_t m5_corep4_tool::touch_test_demo(void)
{
    ESP_LOGI(TAG, "=== Touch Test Demo Start ===");
    
    // 检查LCD和触摸是否已初始化
    if (lcd_panel_handle == NULL) {
        ESP_LOGE(TAG, "LCD panel not initialized! Please call lcd_init() first.");
        return ESP_FAIL;
    }
    
    if (tp_handle == NULL) {
        ESP_LOGE(TAG, "Touch panel not initialized! Please call touch_init() first.");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Touch test ready. Touch the screen to draw trails...");
    ESP_LOGI(TAG, "Press CTRL+C to exit the test");
    ESP_LOGI(TAG, "Each touch point will be a random bright color!");

    // 绘制浅灰色网格线（4条横线 + 4条竖线，将屏幕分成 5×5 网格）
    ESP_LOGI(TAG, "Drawing grid lines for touch test...");
    const uint16_t grid_divisions = 5;  // 分成5等份（需要4条线）
    const uint16_t h_res = LCD_MIPI_DSI_LCD_H_RES;  // 480
    const uint16_t v_res = LCD_MIPI_DSI_LCD_V_RES;  // 480

    uint8_t bytes_per_pixel = 0;
    uint16_t gray565 = 0x4208; // 更暗的灰色 RGB565 约等于RGB(64,64,64)
    uint8_t gray888 = 64;      // 更暗的灰色 RGB888

    if (LCD_MIPI_DSI_COLOR_DEPTH == LCD_COLOR_PIXEL_FORMAT_RGB565) {
        bytes_per_pixel = 2;
    } else if (LCD_MIPI_DSI_COLOR_DEPTH == LCD_COLOR_PIXEL_FORMAT_RGB888) {
        bytes_per_pixel = 3;
    } else {
        ESP_LOGW(TAG, "Unsupported color depth, skip grid rendering");
    }

    if (bytes_per_pixel) {
        // 绘制横线（水平线）
        size_t horizontal_bytes = h_res * bytes_per_pixel;
        uint8_t *horizontal_buf = (uint8_t *)heap_caps_malloc(horizontal_bytes, MALLOC_CAP_SPIRAM);
        if (horizontal_buf) {
            // 填充浅灰色
            if (bytes_per_pixel == 2) {
                uint16_t *buf16 = (uint16_t *)horizontal_buf;
                for (int x = 0; x < h_res; ++x) {
                    buf16[x] = gray565;
                }
            } else {
                for (int x = 0; x < h_res; ++x) {
                    horizontal_buf[x * 3 + 0] = gray888;
                    horizontal_buf[x * 3 + 1] = gray888;
                    horizontal_buf[x * 3 + 2] = gray888;
                }
            }

            // 绘制4条横线，将屏幕垂直方向分成5等份
            // 例如 480 / 5 = 96，所以线在 y = 96, 192, 288, 384
            for (int i = 1; i < grid_divisions; ++i) {
                uint16_t y = (v_res * i) / grid_divisions;
                ESP_LOGI(TAG, "Drawing horizontal line %d at y=%u", i, y);
                esp_err_t line_ret = esp_lcd_panel_draw_bitmap(lcd_panel_handle, 0, y, h_res, y + 1, horizontal_buf);
                if (line_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to draw horizontal grid line %d: %s", i, esp_err_to_name(line_ret));
                }
            }

            free(horizontal_buf);
            ESP_LOGI(TAG, "Horizontal grid lines drawn");
        } else {
            ESP_LOGW(TAG, "Failed to allocate horizontal grid buffer (%zu bytes)", horizontal_bytes);
        }

        // 绘制竖线（垂直线）
        size_t vertical_bytes = v_res * bytes_per_pixel;
        uint8_t *vertical_buf = (uint8_t *)heap_caps_malloc(vertical_bytes, MALLOC_CAP_SPIRAM);
        if (vertical_buf) {
            // 填充浅灰色
            if (bytes_per_pixel == 2) {
                uint16_t *buf16 = (uint16_t *)vertical_buf;
                for (int y = 0; y < v_res; ++y) {
                    buf16[y] = gray565;
                }
            } else {
                for (int y = 0; y < v_res; ++y) {
                    vertical_buf[y * 3 + 0] = gray888;
                    vertical_buf[y * 3 + 1] = gray888;
                    vertical_buf[y * 3 + 2] = gray888;
                }
            }

            // 绘制4条竖线，将屏幕水平方向分成5等份
            // 例如 480 / 5 = 96，所以线在 x = 96, 192, 288, 384
            for (int i = 1; i < grid_divisions; ++i) {
                uint16_t x = (h_res * i) / grid_divisions;
                ESP_LOGI(TAG, "Drawing vertical line %d at x=%u", i, x);
                esp_err_t line_ret = esp_lcd_panel_draw_bitmap(lcd_panel_handle, x, 0, x + 1, v_res, vertical_buf);
                if (line_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to draw vertical grid line %d: %s", i, esp_err_to_name(line_ret));
                }
            }

            free(vertical_buf);
            ESP_LOGI(TAG, "Vertical grid lines drawn");
        } else {
            ESP_LOGW(TAG, "Failed to allocate vertical grid buffer (%zu bytes)", vertical_bytes);
        }
        
        ESP_LOGI(TAG, "Grid drawing complete: 4 horizontal + 4 vertical lines (5x5 grid)");
    }
    
    // 预定义一组鲜艳的颜色（RGB565格式）- 更加鲜艳明亮
    const uint16_t bright_colors[] = {
        0xF800,  // 纯红
        0x07E0,  // 纯绿  
        0x001F,  // 纯蓝
        0xFFE0,  // 黄色
        0xF81F,  // 品红
        0x07FF,  // 青色
        0xFD20,  // 橙色
        0xFFFF,  // 白色
        0xFBE0,  // 浅黄
        0xF81F,  // 紫红
        0x7FFF,  // 浅青
        0xFFDF,  // 浅橙
    };
    const int num_colors = sizeof(bright_colors) / sizeof(bright_colors[0]);
    
    // 用于存储上一次的触摸点位置，避免重复绘制
    static int16_t last_x[5] = {-1, -1, -1, -1, -1};
    static int16_t last_y[5] = {-1, -1, -1, -1, -1};
    
    // 初始化随机数生成器
    srand(esp_timer_get_time());
    
    // 用于调试输出
    uint32_t loop_count = 0;
    uint32_t last_debug_time = 0;
    
    // 主循环
    while (1) {
        loop_count++;
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        uint16_t touch_x[5] = {0};
        uint16_t touch_y[5] = {0};
        uint16_t touch_strength[5] = {0};
        uint8_t touch_count = 0;
        
        // 读取触摸数据
        esp_err_t ret = esp_lcd_touch_read_data(tp_handle);
        if (ret != ESP_OK) {
            if (current_time - last_debug_time > 5000) {
                ESP_LOGW(TAG, "Failed to read touch data: %s (loop: %lu)", 
                         esp_err_to_name(ret), loop_count);
                last_debug_time = current_time;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // 获取触摸点信息 - 注意：返回值是bool，不是esp_err_t
        bool touched = esp_lcd_touch_get_coordinates(tp_handle, touch_x, touch_y, 
                                                      touch_strength, &touch_count, 5);
        
        
        if (touched) {
            // ESP_LOGI(TAG, "Touch detected: %d points", touch_count);
            
            // 遍历所有触摸点
            for (int i = 0; i < touch_count && i < 5; i++) {
                int16_t x = touch_x[i];
                int16_t y = touch_y[i];
                
                // 检查坐标是否在有效范围内
                if (x < 0 || x >= LCD_MIPI_DSI_LCD_H_RES || 
                    y < 0 || y >= LCD_MIPI_DSI_LCD_V_RES) {
                    continue;
                }
                
                // 移除位置去重检查，让每次采样都能绘制
                // 这样即使手指停在同一位置，也会不断绘制随机颜色的点
                // 产生更丰富的视觉效果
                
                // 为每个像素生成随机的鲜艳颜色
                uint16_t color = bright_colors[rand() % num_colors];
                
                // 创建一个1x1像素的缓冲区
                uint16_t pixel_buffer[1] = {color};
                
                // 直接绘制单个像素点
                ret = esp_lcd_panel_draw_bitmap(lcd_panel_handle, 
                                                x, y,           // 起始坐标
                                                x + 1, y + 1,   // 结束坐标（绘制1x1像素）
                                                pixel_buffer);
                
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to draw pixel: %s", esp_err_to_name(ret));
                }
                
                // 减少日志输出频率以提高性能，每100个点输出一次
                // static uint32_t draw_count = 0;
                // draw_count++;
                // if (draw_count % 100 == 0) {
                //     ESP_LOGI(TAG, "Drew %lu pixels, latest at (%d, %d) color=0x%04X", 
                //              draw_count, x, y, color);
                // }
            }
        } else {
            // 没有触摸时，重置上次位置
            for (int i = 0; i < 5; i++) {
                last_x[i] = -1;
                last_y[i] = -1;
            }
        }
        
        // 最小延迟以获得最快的触摸响应速度
        // 1ms延迟 = 1000Hz采样率，非常流畅
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    ESP_LOGI(TAG, "=== Touch Test Demo End ===");
    return ESP_OK;
}

/* LVGL display and touch */
esp_err_t m5_corep4_tool::lvgl_init(void)
{    
    esp_err_t ret = ESP_OK;
    lvgl_port_ready = false;

    /* Initialize LVGL */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 6,         /* LVGL task priority */
        .task_stack = 16384,         /* LVGL task stack size */
        .task_affinity = 1,        /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500,   /* Maximum sleep in LVGL task */
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5        /* LVGL timer tick period in ms */
    };
    ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    
    // Check if required handles are valid
    if (mipi_dbi_io == NULL) {
        ESP_LOGE(TAG, "MIPI DBI IO handle is NULL");
        return ESP_FAIL;
    }
    if (lcd_panel_handle == NULL) { 
        ESP_LOGE(TAG, "LCD panel handle is NULL (did you call lcd_init() before lvgl_init()?)");
        return ESP_FAIL;
    }
    
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = NULL,
        .panel_handle = lcd_panel_handle,
        .control_handle = NULL,
        .buffer_size = LCD_MIPI_DSI_LCD_H_RES * LCD_MIPI_DSI_LCD_V_RES,
        .double_buffer = 1,
        .trans_size = 0,
        .hres = LCD_MIPI_DSI_LCD_H_RES,
        .vres = LCD_MIPI_DSI_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
        .rounder_cb = NULL,
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_UNKNOWN,
#endif
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 1,
            .sw_rotate = 1,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = 0,
#endif
            .full_refresh = 0,
            .direct_mode = 0,
        }
    };

    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {
            .avoid_tearing = 0,
        }
    };

    //配置 color-format
    if(LCD_MIPI_DSI_COLOR_DEPTH == LCD_COLOR_PIXEL_FORMAT_RGB565) {
        disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
        disp_cfg.flags.buff_dma = 1; // 使用DMA
    } else if(LCD_MIPI_DSI_COLOR_DEPTH == LCD_COLOR_PIXEL_FORMAT_RGB888) {
        disp_cfg.color_format = LV_COLOR_FORMAT_RGB888;
        disp_cfg.flags.buff_dma = 0; // RGB888不使用DMA
    } else {
        ESP_LOGW(TAG, "Unsupported LCD_MIPI_DSI_COLOR_DEPTH, using default color format");
        disp_cfg.color_format = LV_COLOR_FORMAT_RGB565; // 默认回退
    }


    lvgl_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (lvgl_disp == NULL) {
        ESP_LOGE(TAG, "Failed to add LVGL display");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LVGL display added successfully");

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = tp_handle,
        .scale = {
            .x = 1.0f,
            .y = 1.0f,
        },
    };
    lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
    if (lvgl_touch_indev == NULL) {
        ESP_LOGE(TAG, "Failed to add LVGL touch input");
        return ESP_FAIL;
    }
    lv_indev_set_type(lvgl_touch_indev, LV_INDEV_TYPE_POINTER);
    // lv_indev_set_read_cb(lvgl_touch_indev, lvgl_read_cb);
    lv_indev_set_display(lvgl_touch_indev, lvgl_disp);

    ESP_LOGI(TAG, "LVGL touch input added successfully");

    // 注意：不要在这里直接创建触摸指示器，因为 LVGL 任务可能还没完全启动
    // 创建一个延迟定时器，在 LVGL 启动后再创建触摸指示器
    lv_timer_t *init_timer = lv_timer_create([](lv_timer_t *timer) {
        // 一次性定时器，创建完触摸指示器后删除自己
        if (g_corep4_instance && g_corep4_instance->create_lvgl_touch_indicator() == ESP_OK) {
            ESP_LOGI(TAG, "Touch indicator created successfully (delayed)");
            // 创建定期更新定时器（每20ms检查一次，提高响应速度）
            lv_timer_t *update_timer = lv_timer_create(lvgl_touch_indicator_timer_cb, 20, NULL);
            if (update_timer) {
                ESP_LOGI(TAG, "Touch indicator update timer created (20ms interval)");
            }
        }
        // 删除初始化定时器（一次性）
        lv_timer_delete(timer);
    }, 500, NULL);  // 延迟500ms后执行
    
    if (init_timer) {
        lv_timer_set_repeat_count(init_timer, 1);  // 只执行一次
        ESP_LOGI(TAG, "Touch indicator delayed initialization scheduled");
    } else {
        ESP_LOGW(TAG, "Failed to create touch indicator init timer");
    }

    lvgl_port_ready = true;
    return ESP_OK;
}

esp_err_t m5_corep4_tool::set_lvgl_rotation(lv_disp_rotation_t rotation)
{
    if (!lvgl_port_lock(100)) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for rotation");
        return ESP_FAIL;
    }

    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
        lv_display_set_rotation(disp, rotation);
        // 保存旋转角度，供鼠标坐标转换使用
        g_display_rotation = rotation;
        ESP_LOGI(TAG, "LVGL display rotated to %d degrees", rotation * 90);
    } else {
        ESP_LOGW(TAG, "No default LVGL display found to rotate");
        lvgl_port_unlock();
        return ESP_FAIL;
    }
    
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t m5_corep4_tool::start_lvgl_benchmark_demo()
{
    // 等待LVGL完全初始化
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 尝试获取LVGL锁，增加超时时间到1秒，并添加重试机制
    int retry_count = 3;
    bool lock_acquired = false;
    
    for (int i = 0; i < retry_count; i++) {
        if (lvgl_port_lock(1000)) { // 增加到1秒超时
            lock_acquired = true;
            break;
        }
        ESP_LOGW(TAG, "LVGL lock acquisition attempt %d/%d failed, retrying...", i+1, retry_count);
        vTaskDelay(pdMS_TO_TICKS(50)); // 重试间隔
    }
    
    if (!lock_acquired) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for benchmark demo after %d attempts", retry_count);
        return ESP_FAIL;
    }

    #if (LV_USE_DEMO_BENCHMARK) || defined(LV_DEMO_BENCHMARK)
        lv_demo_benchmark();
        ESP_LOGI(TAG, "LVGL benchmark demo started successfully");
        lvgl_port_unlock();
        return ESP_OK;
    #else
        ESP_LOGW(TAG, "LVGL benchmark demo not available in this build");
        lvgl_port_unlock();
        return ESP_FAIL;
    #endif
}

esp_err_t m5_corep4_tool::start_lvgl_music_demo()
{
    // 等待LVGL完全初始化
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 尝试获取LVGL锁，增加超时时间到1秒，并添加重试机制
    int retry_count = 3;
    bool lock_acquired = false;
    
    for (int i = 0; i < retry_count; i++) {
        if (lvgl_port_lock(1000)) { // 增加到1秒超时
            lock_acquired = true;
            break;
        }
        ESP_LOGW(TAG, "LVGL lock acquisition attempt %d/%d failed, retrying...", i+1, retry_count);
        vTaskDelay(pdMS_TO_TICKS(50)); // 重试间隔
    }
    
    if (!lock_acquired) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for music demo after %d attempts", retry_count);
        return ESP_FAIL;
    }

    #if (LV_USE_DEMO_MUSIC)
        lv_demo_music();
        ESP_LOGI(TAG, "LVGL music demo started successfully");
        lvgl_port_unlock();
        return ESP_OK;
    #else
        ESP_LOGW(TAG, "LVGL music demo not available in this build");
        lvgl_port_unlock();
        return ESP_FAIL;
    #endif
}
esp_err_t m5_corep4_tool::stop_lvgl_custom_demo_task(uint32_t timeout_ms)
{
    TaskHandle_t task_handle = lvgl_custom_demo_task_handle;
    if (task_handle == nullptr) {
        lvgl_custom_demo_stop_requested = true;
        return ESP_OK;
    }

    lvgl_custom_demo_stop_requested = true;

    if (xTaskGetCurrentTaskHandle() == task_handle) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping LVGL custom demo task");

    if (lvgl_custom_demo_done_sem == nullptr) {
        const TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);
        vTaskDelay(wait_ticks);
        return (lvgl_custom_demo_task_handle == nullptr) ? ESP_OK : ESP_ERR_TIMEOUT;
    }

    const TickType_t wait_ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(lvgl_custom_demo_done_sem, wait_ticks) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for LVGL custom demo task to stop");
        return ESP_ERR_TIMEOUT;
    }

    lvgl_custom_demo_task_handle = nullptr;
    ESP_LOGI(TAG, "LVGL custom demo task stopped");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::start_lvgl_custom_demo(void)
{
    // USB 鼠标仅允许在 USB Mouse Function 页面内启用，避免影响外部 UI。
    constexpr bool kEnableGlobalUsbMouseSupport = false;
    if (kEnableGlobalUsbMouseSupport) {
        esp_err_t mouse_support_ret = init_usb_mouse_support();
        if (mouse_support_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize USB mouse support: %s", esp_err_to_name(mouse_support_ret));
            ESP_LOGI(TAG, "System will continue without mouse support...");
        }
    } else {
        ESP_LOGI(TAG, "Global USB mouse support disabled; use USB Mouse function page.");
    }

    if (lvgl_custom_demo_task_handle != nullptr) {
        ESP_LOGW(TAG, "LVGL custom demo task is already running");
        return ESP_OK;
    }

    // 等待LVGL完全初始化
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 尝试获取LVGL锁，增加超时时间到1秒，并添加重试机制
    int retry_count = 3;
    bool lock_acquired = false;
    
    for (int i = 0; i < retry_count; i++) {
        if (lvgl_port_lock(1000)) { // 增加到1秒超时
            lock_acquired = true;
            break;
        }
        ESP_LOGW(TAG, "LVGL lock acquisition attempt %d/%d failed, retrying...", i+1, retry_count);
        vTaskDelay(pdMS_TO_TICKS(50)); // 重试间隔
    }
    
    if (!lock_acquired) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for custom demo after %d attempts", retry_count);
        return ESP_FAIL;
    }

    ui_init();  

    // 初始化UI同步模块（注册回调、绑定事件、同步初始状态）
    esp_err_t sync_ret = ui_sync_init(*this);
    if (sync_ret != ESP_OK) {
        ESP_LOGW(TAG, "UI sync init failed: %s", esp_err_to_name(sync_ret));
    }
    sync_ret = ui_sync_bindAllEvents(*this);
    if (sync_ret != ESP_OK) {
        ESP_LOGW(TAG, "UI sync bind events failed: %s", esp_err_to_name(sync_ret));
    }
    sync_power_status_to_ui();  // 同步当前电源状态到UI

    // The old chart update task polls cached IMU data and prints sensor values.
    // Keep it disabled so IMU sampling only runs while the IMU function page is open.
    lvgl_custom_demo_stop_requested = true;
    ESP_LOGI(TAG, "Custom LVGL UI started; IMU chart task disabled");
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t m5_corep4_tool::ethernet_init(bool enable)
{
    if (enable) {
        // 初始化以太网
        if (ethernet_initialized) {
            ESP_LOGW(TAG, "Ethernet already initialized");
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Initializing Ethernet...");
        
        // 初始化网络基础设施（仅在第一次调用时）
        if (!netif_initialized) {
            ESP_LOGI(TAG, "Initializing network infrastructure...");
            esp_err_t ret = esp_netif_init();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initialize netif: %s", esp_err_to_name(ret));
                return ret;
            }
            
            ret = esp_event_loop_create_default();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(ret));
                return ret;
            }
            
            netif_initialized = true;
            ESP_LOGI(TAG, "Network infrastructure initialized successfully");
        }
        
        // 创建以太网事件组
        if (s_eth_event_group == NULL) {
            s_eth_event_group = xEventGroupCreate();
            if (s_eth_event_group == NULL) {
                ESP_LOGE(TAG, "Failed to create Ethernet event group");
                return ESP_FAIL;
            }
        }
        
        // 注册以太网事件处理程序
        esp_err_t ret = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, [](void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
            m5_corep4_tool* self = static_cast<m5_corep4_tool*>(arg);
            self->ethernet_event_handler(arg, event_base, event_id, event_data);
        }, this);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register Ethernet event handler: %s", esp_err_to_name(ret));
            return ret;
        }
        
        // 注册IP事件处理程序（用于获取以太网IP）
        ret = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, [](void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
            m5_corep4_tool* self = static_cast<m5_corep4_tool*>(arg);
            self->ethernet_event_handler(arg, event_base, event_id, event_data);
        }, this);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register IP event handler for Ethernet: %s", esp_err_to_name(ret));
            return ret;
        }
        
        // 创建以太网网络接口
        esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
        eth_netif = esp_netif_new(&netif_cfg);
        if (eth_netif == nullptr) {
            ESP_LOGE(TAG, "Failed to create Ethernet netif");
            return ESP_FAIL;
        }

        // 初始化以太网驱动
        ret = p4_eth_init(&eth_handles, &eth_cnt);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize Ethernet: %s", esp_err_to_name(ret));
            esp_netif_destroy(eth_netif);
            eth_netif = nullptr;
            return ret;
        }

        // 将网络接口与以太网驱动连接
        if (eth_cnt > 0 && eth_handles[0] != nullptr) {
            ret = esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0]));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to attach netif to Ethernet: %s", esp_err_to_name(ret));
                ethernet_init(false);
                return ret;
            }
        }

        // 标记驱动已就绪，异步启动接口（避免 PHY 自动协商最长 4s 阻塞）
        ethernet_initialized = true;

        // 后台一次性任务：调用 esp_eth_start（PHY autoneg 在此任务内等待）
        BaseType_t task_ret = xTaskCreate([](void *arg) {
            m5_corep4_tool *self = static_cast<m5_corep4_tool*>(arg);
            for (int i = 0; i < self->eth_cnt; i++) {
                esp_err_t r = esp_eth_start(self->eth_handles[i]);
                if (r != ESP_OK) {
                    ESP_LOGE("eth_start", "Failed to start Ethernet interface %d: %s",
                             i, esp_err_to_name(r));
                    self->ethernet_init(false);
                    vTaskDelete(nullptr);
                    return;
                }
            }
            vTaskDelete(nullptr);
        }, "eth_start", 3072, this, 5, nullptr);

        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create eth_start task, falling back to sync start");
            for (int i = 0; i < eth_cnt; i++) {
                ret = esp_eth_start(eth_handles[i]);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to start Ethernet interface %d: %s", i, esp_err_to_name(ret));
                    ethernet_initialized = false;
                    ethernet_init(false);
                    return ret;
                }
            }
        }

        ESP_LOGI(TAG, "Ethernet driver ready, PHY autoneg running in background");
        return ESP_OK;
    } else {
        // 反初始化以太网
        if (!ethernet_initialized) {
            ESP_LOGW(TAG, "Ethernet not initialized");
            return ESP_OK;
        }

        ESP_LOGI(TAG, "Deinitializing Ethernet...");
        
        // 注销事件处理程序
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, [](void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
            m5_corep4_tool* self = static_cast<m5_corep4_tool*>(arg);
            self->ethernet_event_handler(arg, event_base, event_id, event_data);
        });
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, [](void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
            m5_corep4_tool* self = static_cast<m5_corep4_tool*>(arg);
            self->ethernet_event_handler(arg, event_base, event_id, event_data);
        });
        
        // 停止所有以太网接口
        if (eth_handles != nullptr) {
            for (int i = 0; i < eth_cnt; i++) {
                if (eth_handles[i] != nullptr) {
                    esp_err_t ret = esp_eth_stop(eth_handles[i]);
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to stop Ethernet interface %d: %s", i, esp_err_to_name(ret));
                    }
                }
            }
        }

        // 反初始化以太网驱动
        esp_err_t ret = p4_eth_deinit(eth_handles, eth_cnt);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to deinitialize Ethernet: %s", esp_err_to_name(ret));
        }

        // 销毁网络接口
        if (eth_netif != nullptr) {
            esp_netif_destroy(eth_netif);
            eth_netif = nullptr;
        }

        // 清理事件组（不删除，因为可能会重新初始化）
        if (s_eth_event_group) {
            xEventGroupClearBits(s_eth_event_group, ETH_CONNECTED_BIT | ETH_GOT_IP_BIT);
            xEventGroupSetBits(s_eth_event_group, ETH_DISCONNECTED_BIT);
        }

        // 重置状态变量
        eth_handles = nullptr;
        eth_cnt = 0;
        ethernet_initialized = false;
        
        ESP_LOGI(TAG, "Ethernet deinitialized successfully");
        return ESP_OK;
    }
}

esp_err_t m5_corep4_tool::uvc_https_init()
{
    ESP_LOGI(TAG, "Initializing UVC HTTPS Web Server...");
    
    // 调用原始的app_https_init函数
    app_https_init();
    
    ESP_LOGI(TAG, "UVC HTTPS Web Server initialized successfully");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::uvc_usb_init()
{
    ESP_LOGI(TAG, "Initializing UVC USB Host...");
    
    // 调用原始的app_uvc_init函数
    esp_err_t ret = app_uvc_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize UVC USB Host: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "UVC USB Host initialized successfully");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::p4_pm1_irq_init()
{
    ESP_LOGI(TAG, "Initializing PM1 IRQ system...");

    // 使用存储的重启原因
    ESP_LOGI(TAG, "Last reset reason: %s (%d)", reset_reason_to_string(reset_reason), reset_reason);

    // ESP32 注册中断
    gpio_reset_pin((gpio_num_t)Pin_P4_PMIC_IRQ);
    // gpio_set_pull_mode((gpio_num_t)Pin_P4_PMIC_IRQ, GPIO_PULLUP_ONLY); // 上拉
    gpio_set_direction((gpio_num_t)Pin_P4_PMIC_IRQ, GPIO_MODE_INPUT);   // 输入模式
    gpio_set_intr_type((gpio_num_t)Pin_P4_PMIC_IRQ, GPIO_INTR_NEGEDGE); // 下降沿触发
    
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = gpio_isr_handler_add((gpio_num_t)Pin_P4_PMIC_IRQ, [](void*){
        // 中断处理函数 - 只设置标志，不进行I2C操作
        pm1_irq_pending = true;
    }, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add GPIO ISR handler: %s", esp_err_to_name(ret));
        return ret;
    }

    // 禁用单击复位
    pm1.setSingleResetDisable(true); // 禁用单击复位
    // 禁用双击关机
    // pm1.setDoubleOffDisable(true); // 禁用双击关机功能
    // 禁用无用中断
    pm1.irqSetGpioMaskAll(M5PM1_IRQ_MASK_ENABLE);     // 先屏蔽所有 GPIO 中断
    pm1.irqSetGpioMask(M5PM1_IRQ_GPIO0, M5PM1_IRQ_MASK_DISABLE); // 仅启用 GPIO0 中断
    pm1.irqSetSysMaskAll(M5PM1_IRQ_MASK_ENABLE);      // 禁用所有系统中断
    // pm1.irqSetBtnMaskAll(M5PM1_IRQ_MASK_ENABLE);   // 禁用所有按钮中断
    // 清除所有中断标志
    pm1.irqClearGpioAll();                           // 清除所有GPIO中断标志
    pm1.irqClearSysAll();                            // 清除所有系统中断标志
    // pm1.irqClearBtn(M5PM1_BTN_IRQ_ALL);             // 清除所有按钮中断标志
    // 配置 PM1 GPIO2 为中断输出
    pm1.gpioSetFunc(PYG2_IRQ, M5PM1_GPIO_FUNC_IRQ);     // PM1 GPIO2 为中断输出
    pm1.gpioSetPull(PYG2_IRQ, M5PM1_GPIO_PULL_UP);      // PM1 GPIO2 内部上拉
    pm1.gpioSetDrive(PYG2_IRQ, M5PM1_GPIO_DRIVE_PUSHPULL); // PM1 GPIO2 推挽输出
    // 配置 PM1 GPIO0 为BMI中断输入
    pm1.gpioSetFunc(PYG0_IMU_INT, M5PM1_GPIO_FUNC_GPIO);     // PM1 GPIO0 为BMI中断输入
    pm1.gpioSetMode(PYG0_IMU_INT, M5PM1_GPIO_MODE_INPUT);   // PM1 GPIO0 输入模式

    // 获取本次启动的前置状态
    uint8_t wake_src = 0;
    m5pm1_err_t wake_ret = pm1.getWakeSource(&wake_src, kPm1IrqClearAll);
    if (wake_ret == M5PM1_OK) {
        ESP_LOGI(TAG, "System boot reason detection:");
        if (wake_src == 0) {
            ESP_LOGI(TAG, "  - First power-on or unknown wake source (reg: 0x%02X)", wake_src);
        }
        else
        {
            if (wake_src & M5PM1_WAKE_SRC_TIM) {
                ESP_LOGI(TAG, "  - Timer wake up");
            }
            if (wake_src & M5PM1_WAKE_SRC_VIN) {
                ESP_LOGI(TAG, "  - 5VIN insertion wake up");
            }
            if (wake_src & M5PM1_WAKE_SRC_PWRBTN) {
                ESP_LOGI(TAG, "  - Power button wake up");
            }
            if (wake_src & M5PM1_WAKE_SRC_RSTBTN) {
                ESP_LOGI(TAG, "  - Reset button wake up");
            }
            if (wake_src & M5PM1_WAKE_SRC_CMD_RST) {
                ESP_LOGI(TAG, "  - Command reset wake up");
            }
            if (wake_src & M5PM1_WAKE_SRC_EXT_WAKE) {
                ESP_LOGI(TAG, "  - External GPIO wake up");
            }
            if (wake_src & M5PM1_WAKE_SRC_5VINOUT) {
                ESP_LOGI(TAG, "  - 5VINOUT insertion wake up");
            }
        }
    } else {
        ESP_LOGE(TAG, "Failed to read wake source: %d", wake_ret);
    }

    // 如果是DEEPSLEEP唤醒，则要忽略唤醒那一次所触发的按键中断
    if (reset_reason == ESP_RST_DEEPSLEEP) {
        ESP_LOGI(TAG, "Woke from deep sleep, ignoring any immediate button IRQ");
        pm1.irqClearBtnAll(); // 清除所有按钮中断标志
        pm1_irq_pending = false; // 清除中断标志
        vTaskDelay(pdMS_TO_TICKS(500)); // 等待500ms
        pm1.setPowerConfig(M5PM1_PWR_CFG_LED_CTRL, 0x00);
    }

    // 创建PM1按钮处理任务
    BaseType_t task_ret = xTaskCreate(
        p4_pm1_irq_task,           // 任务函数
        "p4_pm1_task",             // 任务名称
        4096,                       // 栈大小
        this,                       // 传递给任务的参数(this指针)
        5,                          // 任务优先级
        &p4_pm1_irq_task_handle    // 任务句柄
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PM1 button task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PM1 IRQ system initialized successfully");
    return ESP_OK;
}

bool m5_corep4_tool::lvgl_ui_ready() const
{
    return lvgl_port_ready && lvgl_disp != nullptr;
}

bool m5_corep4_tool::try_lock_lvgl_ui(uint32_t timeout_ms) const
{
    if (!lvgl_ui_ready()) {
        return false;
    }
    return lvgl_port_lock(timeout_ms);
}

void m5_corep4_tool::p4_pm1_irq_task(void *pvParameters)
{
    m5_corep4_tool *self = static_cast<m5_corep4_tool*>(pvParameters);
    const char *task_tag = "P4_PM1_TASK";
    
    ESP_LOGI(task_tag, "P4 PM1 IRQ task started");

    // Main loop
    while (true) {
        // 检查PM1中断标志
        if (pm1_irq_pending) {
            pm1_irq_pending = false; // 清除标志
            
            // 在主任务中处理PM1中断
            uint8_t btn_irq = 0;
            m5pm1_err_t ret_pm1 = self->pm1.irqGetBtnStatus(&btn_irq, kPm1IrqClearAll);
            if (ret_pm1 == M5PM1_OK && btn_irq != 0) {
                ESP_LOGI(task_tag, "PM1 Button IRQ: 0x%02X", btn_irq);
                if (btn_irq & M5PM1_IRQ_BTN_CLICK) {
                    ESP_LOGI(task_tag, "PM1 Button IRQ: Single Click");
                }
                if (btn_irq & M5PM1_IRQ_BTN_DOUBLE) {
                    ESP_LOGI(task_tag, "PM1 Button IRQ: Double Click");
                }
                if (btn_irq & M5PM1_IRQ_BTN_WAKE) {
                    ESP_LOGI(task_tag, "PM1 Button IRQ: Wakeup");
                }
            }

            // 如果
            if (ret_pm1 == M5PM1_OK && btn_irq != 0)
            {
                if (btn_irq & M5PM1_IRQ_BTN_WAKE) {
                    ESP_LOGI(task_tag, "PM1_BTN_IRQ_WAKE detected - no action");
                }

                if (btn_irq & M5PM1_IRQ_BTN_CLICK) {
                    ESP_LOGI(task_tag, "PM1_BTN_IRQ_CLICK detected");
                    // 检查是否允许按键休眠
                    if (self->device_status.sleep.btn_sleep_enable) {
                        ESP_LOGI(task_tag, "Button sleep enabled, entering deep sleep in 1s...");
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        self->sleep_before_work(); // 休眠前处理
                        self->enter_deep_sleep(); // 进入深度休眠
                    } else {
                        ESP_LOGI(task_tag, "Button sleep disabled, ignoring click");
                    }
                }

                // if (btn_irq & (1 << M5PM1_BTN_IRQ_DOUBLE)) {
                //     ESP_LOGI(task_tag, "Double Click detected - after 2s PM1 shutdown");
                //     vTaskDelay(pdMS_TO_TICKS(2000));
                //     self->pm1.shutdown(); // 关机
                // }
            }

            // 检查PM1 GPIO中断状态
            uint8_t gpio_irq_status = 0;
            m5pm1_err_t gpio_ret = self->pm1.irqGetGpioStatus(&gpio_irq_status, kPm1IrqClearAll);
            if (gpio_ret == M5PM1_OK && gpio_irq_status != 0) {
                ESP_LOGI(task_tag, "PM1 GPIO IRQ: 0x%02X", gpio_irq_status);
                
                // 检查GPIO0中断（BMI270传感器中断）
                if (gpio_irq_status & (1 << M5PM1_GPIO_NUM_0)) {
                    ESP_LOGI(task_tag, "BMI270 sensor interrupt detected on PM1 GPIO0");
                    // lvgl ui_irqcon 背景 配置为红色
                    // 在LVGL线程安全地设置指示器为红色并短暂高亮，随后恢复原样
                    if (self->try_lock_lvgl_ui(100)) {
                        if (ui_irqcon) {
                                    lv_obj_set_style_bg_color(ui_irqcon, lv_color_hex(0xFF0000), LV_PART_MAIN);
                                    lv_obj_set_style_bg_opa(ui_irqcon, LV_OPA_COVER, LV_PART_MAIN);
                        }
                        lvgl_port_unlock();
                    }

                    // 清除BMI270中断
                    self->imu_clear_interrupt();

                    // 创建一个一次性任务，在短延迟后恢复指示器颜色与文字
                    xTaskCreate(
                        [](void *pv) {
                            auto* tool = static_cast<m5_corep4_tool*>(pv);
                            const TickType_t delay = pdMS_TO_TICKS(3000);
                            vTaskDelay(delay);

                            if (tool && tool->try_lock_lvgl_ui(100)) {
                                if (ui_irqcon) {
                                    lv_obj_set_style_bg_color(ui_irqcon, lv_color_hex(0xd7d7d7), LV_PART_MAIN);
                                    lv_obj_set_style_bg_opa(ui_irqcon, LV_OPA_TRANSP, LV_PART_MAIN);
                                }
                                lvgl_port_unlock();
                            }

                            
                            vTaskDelete(NULL);
                        },
                        "irq_ui_clear",
                        2048,
                        self, // 传递self指针作为参数
                        5,
                        NULL
                    );
                }
                if (gpio_irq_status & ~(1 << M5PM1_GPIO_NUM_0)) {
                    ESP_LOGW(task_tag, "Unhandled GPIO IRQ: 0x%02X", gpio_irq_status);
                }
                
                // 检查其他GPIO中断
                // if (gpio_irq_status & (1 << M5PM1_GPIO_NUM_1)) {
                //     ESP_LOGI(task_tag, "PM1 GPIO1 interrupt detected");
                // }
                // if (gpio_irq_status & (1 << M5PM1_GPIO_NUM_2)) {
                //     ESP_LOGI(task_tag, "PM1 GPIO2 interrupt detected");
                // }
                // if (gpio_irq_status & (1 << M5PM1_GPIO_NUM_3)) {
                //     ESP_LOGI(task_tag, "PM1 GPIO3 interrupt detected");
                // }
                // if (gpio_irq_status & (1 << M5PM1_GPIO_NUM_4)) {
                //     ESP_LOGI(task_tag, "PM1 GPIO4 interrupt detected");
                // }
            }

            // 检查PM1系统中断状态
            // uint8_t sys_irq_status = 0;
            // m5pm1_err_t sys_ret = self->pm1.irqGetSysStatus(&sys_irq_status, kPm1IrqClearAll);
            // if (sys_ret == M5PM1_OK && sys_irq_status != 0) {
            //     ESP_LOGI(task_tag, "PM1 System IRQ: 0x%02X", sys_irq_status);
                
            //     // 处理系统中断
            //     if (sys_irq_status & (1 << 0)) {
            //         ESP_LOGI(task_tag, "5V IN插入检测");
            //     }
            //     if (sys_irq_status & (1 << 1)) {
            //         ESP_LOGI(task_tag, "5V IN移除检测");
            //     }
            //     if (sys_irq_status & (1 << 2)) {
            //         ESP_LOGI(task_tag, "5V INOUT插入检测");
            //     }
            //     if (sys_irq_status & (1 << 3)) {
            //         ESP_LOGI(task_tag, "5V INOUT移除检测");
            //     }
            //     if (sys_irq_status & (1 << 4)) {
            //         ESP_LOGI(task_tag, "电池插入检测");
            //     }
            //     if (sys_irq_status & (1 << 5)) {
            //         ESP_LOGI(task_tag, "电池移除检测");
            //     }
            // }
        }
        else
        {
            // 如果引脚被拉低，说明中断可能丢失，手动处理一次
            if (gpio_get_level((gpio_num_t)Pin_P4_PMIC_IRQ) == 0) {
                self->pm1.irqClearBtnAll(); // 清除所有按钮中断标志
                ESP_LOGW(task_tag, "PM1 IRQ pin low, manually triggering IRQ handling");
            }
        }

        //等待200ms
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

const char* m5_corep4_tool::reset_reason_to_string(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_UNKNOWN:
            return "UNKNOWN";
        case ESP_RST_POWERON:
            return "POWER_ON";
        case ESP_RST_EXT:
            return "EXTERNAL_PIN";
        case ESP_RST_SW:
            return "SOFTWARE_RESTART";
        case ESP_RST_PANIC:
            return "EXCEPTION_PANIC";
        case ESP_RST_INT_WDT:
            return "INTERRUPT_WATCHDOG";
        case ESP_RST_TASK_WDT:
            return "TASK_WATCHDOG";
        case ESP_RST_WDT:
            return "OTHER_WATCHDOG";
        case ESP_RST_DEEPSLEEP:
            return "DEEP_SLEEP";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT";
        case ESP_RST_SDIO:
            return "SDIO";
        case ESP_RST_USB:
            return "USB_PERIPHERAL";
        case ESP_RST_JTAG:
            return "JTAG";
        case ESP_RST_EFUSE:
            return "EFUSE_ERROR";
        case ESP_RST_PWR_GLITCH:
            return "POWER_GLITCH";
        case ESP_RST_CPU_LOCKUP:
            return "CPU_LOCKUP";
        default:
            return "INVALID_REASON";
    }
}

void m5_corep4_tool::wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (local_wifi_ssid[0] != '\0') {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        device_status.wifi.wifi_connected = false;
        device_status.wifi.ssid[0] = '\0';
        set_status(StatusParam::WIFI_CONNECTED, false,
                   StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
        if (wifi_con_retry_num < 10) {
            if (local_wifi_ssid[0] != '\0') {
                esp_wifi_connect();
            }
            wifi_con_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (local_wifi_ssid[0] != '\0') {
            esp_wifi_remote_connect();
        }
    } else if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        device_status.wifi.wifi_connected = false;
        device_status.wifi.ssid[0] = '\0';
        set_status(StatusParam::WIFI_CONNECTED, false,
                   StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
        if (wifi_con_retry_num < 10) {
            if (local_wifi_ssid[0] != '\0') {
                esp_wifi_remote_connect();
            }
            wifi_con_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the remote AP");
        } else {
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_REMOTE_FAIL_BIT);
            }
        }
        ESP_LOGI(TAG, "remote AP connect fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        if (event->esp_netif == esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
            ESP_LOGI(TAG, "WiFi got IP:" IPSTR, IP2STR(&event->ip_info.ip));
            wifi_con_retry_num = 0;
            wifi_refresh_status();
            set_status(StatusParam::WIFI_CONNECTED, true,
                       StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }
        }
    }
}

void m5_corep4_tool::ethernet_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    uint8_t mac_addr[6] = {0};
    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        if (s_eth_event_group) {
            xEventGroupSetBits(s_eth_event_group, ETH_CONNECTED_BIT);
        }

        // 更新状态
        device_status.ethernet.ethernet_connected = true;
        memcpy(device_status.ethernet.eth_mac_addr, mac_addr, 6);
        set_status(StatusParam::ETH_CONNECTED, true, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        if (s_eth_event_group) {
            xEventGroupSetBits(s_eth_event_group, ETH_DISCONNECTED_BIT);
            xEventGroupClearBits(s_eth_event_group, ETH_GOT_IP_BIT);
        }

        // 更新状态并清除IP信息
        device_status.ethernet.ethernet_connected = false;
        device_status.ethernet.got_ip = false;
        memset(device_status.ethernet.ip_address, 0, sizeof(device_status.ethernet.ip_address));
        memset(device_status.ethernet.gateway, 0, sizeof(device_status.ethernet.gateway));
        memset(device_status.ethernet.netmask, 0, sizeof(device_status.ethernet.netmask));
        set_status(StatusParam::ETH_CONNECTED, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
        set_status(StatusParam::ETH_GOT_IP, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
        break;

    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        if (s_eth_event_group) {
            xEventGroupClearBits(s_eth_event_group, ETH_CONNECTED_BIT | ETH_GOT_IP_BIT);
            xEventGroupSetBits(s_eth_event_group, ETH_DISCONNECTED_BIT);
        }

        // 清除所有状态
        device_status.ethernet.ethernet_connected = false;
        device_status.ethernet.got_ip = false;
        memset(device_status.ethernet.ip_address, 0, sizeof(device_status.ethernet.ip_address));
        memset(device_status.ethernet.gateway, 0, sizeof(device_status.ethernet.gateway));
        memset(device_status.ethernet.netmask, 0, sizeof(device_status.ethernet.netmask));
        set_status(StatusParam::ETH_CONNECTED, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
        set_status(StatusParam::ETH_GOT_IP, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
        break;

    default:
        break;
    }

    // 处理IP事件（以太网接口获取IP）
    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Ethernet Got IP Address");
        ESP_LOGI(TAG, "~~~~~~~~~~~");
        ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&event->ip_info.gw));
        ESP_LOGI(TAG, "~~~~~~~~~~~");
        if (s_eth_event_group) {
            xEventGroupSetBits(s_eth_event_group, ETH_GOT_IP_BIT);
        }

        // 保存IP信息到状态结构体
        device_status.ethernet.got_ip = true;
        snprintf(device_status.ethernet.ip_address, sizeof(device_status.ethernet.ip_address),
                 IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(device_status.ethernet.gateway, sizeof(device_status.ethernet.gateway),
                 IPSTR, IP2STR(&event->ip_info.gw));
        snprintf(device_status.ethernet.netmask, sizeof(device_status.ethernet.netmask),
                 IPSTR, IP2STR(&event->ip_info.netmask));

        // 触发UI更新
        set_status(StatusParam::ETH_GOT_IP, true, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
    }
}

esp_err_t m5_corep4_tool::wifi_base_init()
{
    if (wifi_initialized) {
        device_status.wifi.wifi_initialized = true;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WiFi base infrastructure...");

    if (!netif_initialized) {
        ESP_LOGI(TAG, "Initializing network infrastructure...");
        esp_err_t ret = esp_netif_init();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to initialize netif: %s", esp_err_to_name(ret));
            return ret;
        }
        
        ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(ret));
            return ret;
        }
        
        netif_initialized = true;
        ESP_LOGI(TAG, "Network infrastructure initialized successfully");
    }

    // 创建 Wi‑Fi 事件组（成员）
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create WiFi event group");
            return ESP_FAIL;
        }
    }

    if (wifi_sta_netif == nullptr) {
        wifi_sta_netif = esp_netif_create_default_wifi_sta();
        if (wifi_sta_netif == nullptr) {
            ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
            return ESP_FAIL;
        }
    }

    if (!wifi_hosted_initialized) {
        ESP_LOGI(TAG, "Initializing ESP-Hosted transport for ESP32-C6...");
        esp_err_t ret = esp_hosted_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ESP-Hosted init failed: %s", esp_err_to_name(ret));
            device_status.wifi.wifi_initialized = false;
            wifi_last_init_result = ret;
            return ret;
        }
        wifi_hosted_initialized = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.dynamic_rx_buf_num = 48;
    cfg.dynamic_tx_buf_num = 48;
    cfg.static_rx_buf_num = 16;
    
    ESP_LOGI(TAG, "Initializing WiFi with optimized configuration for ESP32-C6...");
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        device_status.wifi.wifi_initialized = false;
        wifi_last_init_result = ret;
        return ret;
    }

    if (!wifi_event_handlers_registered) {
        ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, [](void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
            m5_corep4_tool* self = static_cast<m5_corep4_tool*>(arg);
            self->wifi_event_handler(arg, event_base, event_id, event_data);
        }, this);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, [](void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
            m5_corep4_tool* self = static_cast<m5_corep4_tool*>(arg);
            self->wifi_event_handler(arg, event_base, event_id, event_data);
        }, this);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register IP_EVENT handler: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_event_handler_register(WIFI_REMOTE_EVENT, ESP_EVENT_ANY_ID, [](void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
            m5_corep4_tool* self = static_cast<m5_corep4_tool*>(arg);
            self->wifi_event_handler(arg, event_base, event_id, event_data);
        }, this);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register WIFI_REMOTE_EVENT handler: %s", esp_err_to_name(ret));
            return ret;
        }

        wifi_event_handlers_registered = true;
    }

    wifi_initialized = true;
    device_status.wifi.wifi_initialized = true;
    wifi_last_init_result = ESP_OK;
    ESP_LOGI(TAG, "WiFi base initialized successfully");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::wifi_refresh_status(void)
{
    device_status.wifi.wifi_initialized = wifi_initialized;
    if (!wifi_initialized) {
        device_status.wifi.wifi_connected = false;
        device_status.wifi.ssid[0] = '\0';
        device_status.wifi.rssi = 0;
        device_status.wifi.channel = 0;
        device_status.wifi.auth_mode = 0;
        memset(device_status.wifi.wifi_mac_addr, 0, sizeof(device_status.wifi.wifi_mac_addr));
        return ESP_ERR_WIFI_NOT_INIT;
    }

    uint8_t mac[6] = {};
    esp_err_t mac_ret = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (mac_ret == ESP_OK) {
        memcpy(device_status.wifi.wifi_mac_addr, mac, sizeof(mac));
    }

    wifi_ap_record_t ap = {};
    esp_err_t ap_ret = esp_wifi_sta_get_ap_info(&ap);
    if (ap_ret == ESP_OK) {
        device_status.wifi.wifi_connected = true;
        device_status.wifi.rssi = ap.rssi;
        device_status.wifi.channel = ap.primary;
        device_status.wifi.auth_mode = ap.authmode;
        strncpy(device_status.wifi.ssid, reinterpret_cast<const char*>(ap.ssid),
                sizeof(device_status.wifi.ssid) - 1);
        device_status.wifi.ssid[sizeof(device_status.wifi.ssid) - 1] = '\0';
    } else {
        device_status.wifi.wifi_connected = false;
        device_status.wifi.ssid[0] = '\0';
        device_status.wifi.rssi = 0;
        device_status.wifi.channel = 0;
        device_status.wifi.auth_mode = 0;
    }

    return mac_ret;
}

bool m5_corep4_tool::is_wifi_initialized(void) const
{
    return wifi_initialized;
}

bool m5_corep4_tool::is_wifi_init_running(void) const
{
    return wifi_init_running;
}

esp_err_t m5_corep4_tool::get_wifi_last_init_result(void) const
{
    return wifi_last_init_result;
}

esp_err_t m5_corep4_tool::wifi_module_init(bool reset_before)
{
    if (wifi_init_mutex == nullptr) {
        wifi_init_mutex = xSemaphoreCreateMutex();
        if (wifi_init_mutex == nullptr) {
            wifi_last_init_result = ESP_ERR_NO_MEM;
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(wifi_init_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        wifi_last_init_result = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }

    wifi_init_running = true;
    wifi_last_init_result = ESP_ERR_INVALID_STATE;

    if (reset_before) {
        if (wifi_initialized) {
            esp_wifi_stop();
            esp_wifi_deinit();
            wifi_initialized = false;
            device_status.wifi.wifi_initialized = false;
            device_status.wifi.wifi_connected = false;
            device_status.wifi.ssid[0] = '\0';
        }
        if (wifi_hosted_initialized) {
            esp_err_t deinit_ret = esp_hosted_deinit();
            if (deinit_ret != ESP_OK) {
                ESP_LOGW(TAG, "ESP-Hosted deinit before retry failed: %s", esp_err_to_name(deinit_ret));
            }
            wifi_hosted_initialized = false;
        }

        esp_err_t reset_ret = esp_hosted_manual_reset();
        if (reset_ret != ESP_OK) {
            ESP_LOGW(TAG, "ESP-Hosted reset before WiFi init failed: %s", esp_err_to_name(reset_ret));
        }
    }

    esp_err_t ret = wifi_base_init();
    if (ret == ESP_OK) {
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Set WiFi STA mode failed: %s", esp_err_to_name(ret));
        }
    }

    if (ret == ESP_OK) {
        uint8_t mac[6] = {};
        ret = esp_wifi_get_mac(WIFI_IF_STA, mac);
        if (ret == ESP_OK) {
            memcpy(device_status.wifi.wifi_mac_addr, mac, sizeof(mac));
            device_status.wifi.wifi_initialized = true;
            wifi_initialized = true;
        } else {
            ESP_LOGE(TAG, "WiFi MAC validation failed: %s", esp_err_to_name(ret));
        }
    }

    if (ret != ESP_OK) {
        if (wifi_initialized) {
            esp_wifi_stop();
            esp_wifi_deinit();
            wifi_initialized = false;
        }
        if (wifi_hosted_initialized) {
            esp_hosted_deinit();
            wifi_hosted_initialized = false;
        }
        device_status.wifi.wifi_initialized = false;
        device_status.wifi.wifi_connected = false;
        device_status.wifi.ssid[0] = '\0';
        memset(device_status.wifi.wifi_mac_addr, 0, sizeof(device_status.wifi.wifi_mac_addr));
        set_status(StatusParam::WIFI_INITIALIZED, false,
                   StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
        set_status(StatusParam::WIFI_CONNECTED, false,
                   StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
    } else {
        wifi_refresh_status();
        set_status(StatusParam::WIFI_INITIALIZED, true,
                   StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
        ESP_LOGI(TAG, "WiFi module initialized, STA MAC %02X:%02X:%02X:%02X:%02X:%02X",
                 device_status.wifi.wifi_mac_addr[0], device_status.wifi.wifi_mac_addr[1],
                 device_status.wifi.wifi_mac_addr[2], device_status.wifi.wifi_mac_addr[3],
                 device_status.wifi.wifi_mac_addr[4], device_status.wifi.wifi_mac_addr[5]);
    }

    wifi_last_init_result = ret;
    wifi_init_running = false;
    xSemaphoreGive(wifi_init_mutex);
    return ret;
}

esp_err_t m5_corep4_tool::wifi_start_auto_init_once(void)
{
    if (wifi_auto_init_attempted || wifi_initialized || wifi_init_running) {
        return ESP_OK;
    }

    wifi_auto_init_attempted = true;
    BaseType_t task_ret = xTaskCreate([](void* arg) {
        auto* self = static_cast<m5_corep4_tool*>(arg);
        if (self) {
            self->wifi_module_init(false);
            self->wifi_auto_init_task_handle = nullptr;
        }
        vTaskDelete(nullptr);
    }, "wifi_auto_init", 8192, this, 5, &wifi_auto_init_task_handle);

    if (task_ret != pdPASS) {
        wifi_auto_init_task_handle = nullptr;
        wifi_auto_init_attempted = false;
        wifi_last_init_result = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Failed to create WiFi auto init task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t m5_corep4_tool::wifi_scan_aps(wifi_ap_record_t* records,
                                        uint16_t max_records,
                                        uint16_t* out_total_count,
                                        uint16_t* out_record_count,
                                        bool show_hidden,
                                        uint32_t timeout_ms)
{
    (void)timeout_ms;

    if ((max_records > 0 && records == nullptr) || out_total_count == nullptr || out_record_count == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_total_count = 0;
    *out_record_count = 0;

    esp_err_t ret = wifi_module_init(false);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        return ret;
    }

    wifi_country_t country = {};
    memcpy(country.cc, "CN", 2);
    country.schan = 1;
    country.nchan = 13;
    country.max_tx_power = 20;
    country.policy = WIFI_COUNTRY_POLICY_AUTO;
    esp_wifi_set_country(&country);

    wifi_scan_config_t scan_config = {};
    scan_config.channel = 0;
    scan_config.show_hidden = show_hidden;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 120;
    scan_config.scan_time.active.max = 350;
    scan_config.scan_time.passive = 600;

    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t total_count = 0;
    ret = esp_wifi_scan_get_ap_num(&total_count);
    if (ret != ESP_OK) {
        return ret;
    }
    *out_total_count = total_count;

    uint16_t record_count = total_count;
    if (record_count > max_records) {
        record_count = max_records;
    }

    if (record_count > 0) {
        ret = esp_wifi_scan_get_ap_records(&record_count, records);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    *out_record_count = record_count;
    wifi_refresh_status();
    return ESP_OK;
}

esp_err_t m5_corep4_tool::esp32c6_wifi_connect(const char* ssid, const char* password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = wifi_module_init(false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi module initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 保存到对象内部缓冲（确保以 NUL 结尾）
    size_t ssid_len = strnlen(ssid, sizeof(local_wifi_ssid) - 1);
    size_t pwd_len  = strnlen(password, sizeof(local_wifi_password) - 1);
    memcpy(local_wifi_ssid, ssid, ssid_len);
    local_wifi_ssid[ssid_len] = '\0';
    memcpy(local_wifi_password, password, pwd_len);
    local_wifi_password[pwd_len] = '\0';

    // 重置重试计数器
    wifi_con_retry_num = 0;

    // 配置本地 STA 参数并启动
    wifi_config_t wifi_config = {};
    memcpy(wifi_config.sta.ssid, local_wifi_ssid, sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.password, local_wifi_password, sizeof(wifi_config.sta.password));

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_BITS);
    }

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WiFi STA starting, SSID: %s", local_wifi_ssid);

    if (err == ESP_ERR_WIFI_CONN) {
        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (!s_wifi_event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by wifi_event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_BITS, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "[OK] WiFi connected to AP SSID:%s", local_wifi_ssid);
        wifi_refresh_status();
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "[FAIL] Failed to connect to SSID:%s", local_wifi_ssid);
        set_status(StatusParam::WIFI_CONNECTED, false,
                   StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        return ESP_FAIL;
    }
}

esp_err_t m5_corep4_tool::esp32c6_wifi_scan(uint8_t max_aps, uint32_t timeout_ms, bool show_hidden)
{
    uint16_t limit = max_aps == 0 ? 20 : max_aps;
    wifi_ap_record_t* ap_records = static_cast<wifi_ap_record_t*>(calloc(limit, sizeof(wifi_ap_record_t)));
    if (!ap_records) {
        ESP_LOGE(TAG, "Failed to allocate memory for WiFi scan records");
        return ESP_ERR_NO_MEM;
    }

    uint16_t total_count = 0;
    uint16_t record_count = 0;
    esp_err_t ret = wifi_scan_aps(ap_records, limit, &total_count, &record_count, show_hidden, timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
        free(ap_records);
        return ret;
    }

    ESP_LOGI(TAG, "WiFi Scan Results (%u networks found, showing %u):",
             static_cast<unsigned>(total_count),
             static_cast<unsigned>(record_count));
    ESP_LOGI(TAG, "+==============================================================================+");
    ESP_LOGI(TAG, "| #  | SSID                             | RSSI | Ch | Security              |");
    ESP_LOGI(TAG, "+====+==================================+======+====+=======================+");

    for (uint16_t i = 0; i < record_count; i++) {
        const wifi_ap_record_t* ap = &ap_records[i];
        char ssid_str[33] = {};
        bool ssid_printable = true;
        for (int j = 0; j < 32 && ap->ssid[j] != 0; j++) {
            if (ap->ssid[j] < 32 || ap->ssid[j] > 126) {
                ssid_printable = false;
                break;
            }
        }

        if (ssid_printable && strlen(reinterpret_cast<const char*>(ap->ssid)) > 0) {
            snprintf(ssid_str, sizeof(ssid_str), "%s", reinterpret_cast<const char*>(ap->ssid));
        } else if (strlen(reinterpret_cast<const char*>(ap->ssid)) == 0) {
            snprintf(ssid_str, sizeof(ssid_str), "<Hidden Network>");
        } else {
            snprintf(ssid_str, sizeof(ssid_str), "<Non-printable SSID>");
        }

        const char* auth_mode_str = "Unknown";
        switch (ap->authmode) {
            case WIFI_AUTH_OPEN:             auth_mode_str = "Open"; break;
            case WIFI_AUTH_WEP:              auth_mode_str = "WEP"; break;
            case WIFI_AUTH_WPA_PSK:          auth_mode_str = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK:         auth_mode_str = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK:     auth_mode_str = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA2_ENTERPRISE:  auth_mode_str = "WPA2-ENT"; break;
            case WIFI_AUTH_WPA3_PSK:         auth_mode_str = "WPA3"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK:    auth_mode_str = "WPA2/WPA3"; break;
            case WIFI_AUTH_WAPI_PSK:         auth_mode_str = "WAPI"; break;
            default:                         break;
        }

        const char* signal_icon = ap->rssi >= -50 ? "****" :
                                  ap->rssi >= -60 ? "*** " :
                                  ap->rssi >= -70 ? "**  " : "*   ";
        ESP_LOGI(TAG, "|%2u  | %-32s |%s %3d |%2u | %-21s |",
                 static_cast<unsigned>(i + 1), ssid_str, signal_icon,
                 static_cast<int>(ap->rssi), static_cast<unsigned>(ap->primary), auth_mode_str);
    }

    ESP_LOGI(TAG, "+====+==================================+======+====+=======================+");
    free(ap_records);
    return ESP_OK;
}
esp_err_t m5_corep4_tool::wait_for_ethernet_connection(uint32_t timeout_ms)
{
    if (!ethernet_initialized) {
        ESP_LOGE(TAG, "Ethernet not initialized");
        return ESP_FAIL;
    }
    
    if (s_eth_event_group == NULL) {
        ESP_LOGE(TAG, "Ethernet event group not created");
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Waiting for Ethernet connection...");
    
    TickType_t timeout_ticks = timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    
    /* Waiting until either the connection is established (ETH_GOT_IP_BIT) or connection failed for the maximum
     * amount of retries (ETH_FAIL_BIT). The bits are set by ethernet_event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_eth_event_group,
            ETH_GOT_IP_BIT | ETH_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            timeout_ticks);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & ETH_GOT_IP_BIT) {
        ESP_LOGI(TAG, "Ethernet connected and got IP");
        return ESP_OK;
    } else if (bits & ETH_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect Ethernet");
        return ESP_FAIL;
    } else {
        ESP_LOGD(TAG, "Ethernet connection timeout");
        return ESP_ERR_TIMEOUT;
    }
}

bool m5_corep4_tool::is_ethernet_connected()
{
    if (!ethernet_initialized || s_eth_event_group == NULL) {
        return false;
    }
    
    EventBits_t bits = xEventGroupGetBits(s_eth_event_group);
    return (bits & ETH_GOT_IP_BIT) != 0;
}

esp_err_t m5_corep4_tool::get_ethernet_ip_info(esp_netif_ip_info_t* ip_info)
{
    if (ip_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!ethernet_initialized || eth_netif == NULL) {
        ESP_LOGE(TAG, "Ethernet not initialized");
        return ESP_FAIL;
    }
    
    return esp_netif_get_ip_info(eth_netif, ip_info);
}

// ==================== IMU shared service ====================

esp_err_t m5_corep4_tool::imu_init(bool enable_magnetometer, bmi270_tools::bmi270_mode_t mode,
                                   bool configure_motion_interrupt)
{
    const char *task_tag = "IMU_SERVICE";

    if (imu_data_mutex == nullptr) {
        imu_data_mutex = xSemaphoreCreateMutex();
        if (imu_data_mutex == nullptr) {
            ESP_LOGE(task_tag, "Failed to create IMU mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    const bool bmi270_was_initialized = bmi270.is_initialized();
    if (enable_magnetometer && bmi270_was_initialized) {
        if (imu_service_task_handle != nullptr) {
            ESP_LOGI(task_tag, "Stopping IMU service before BMM150 reconfiguration");
            imu_stop_service();
        }
        esp_err_t power_ret = set_status(StatusParam::POWER_PYB_L3B, true);
        if (power_ret != ESP_OK) {
            ESP_LOGW(task_tag, "Failed to enable PYB L3B before BMM150 reconfiguration: %s",
                     esp_err_to_name(power_ret));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!bmi270.is_initialized()) {
        ESP_LOGI(task_tag, "Initializing BMI270/BMM150 shared service...");
        esp_err_t power_ret = set_status(StatusParam::POWER_PYB_L3B, true);
        if (power_ret != ESP_OK) {
            ESP_LOGW(task_tag, "Failed to enable PYB L3B before BMI270 init: %s", esp_err_to_name(power_ret));
        }
        vTaskDelay(pdMS_TO_TICKS(50));

        auto bmi270_device_handle = otool_tools.i2c_tools.get_i2c_device_handle(I2C_BMI270_ADDR);
        esp_err_t init_ret = bmi270.init(otool_tools.i2c_tools.get_i2c_bus_handle(),
                                         &bmi270_device_handle,
                                         enable_magnetometer,
                                         mode);
        if (init_ret != ESP_OK) {
            set_status(StatusParam::IMU_BMI270_INITIALIZED, false,
                       StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
            ESP_LOGE(task_tag, "BMI270 initialization failed: %s", esp_err_to_name(init_ret));
            return init_ret;
        }
    } else {
        ESP_LOGI(task_tag, "BMI270 already initialized, reusing shared service");
    }

    set_status(StatusParam::IMU_BMI270_INITIALIZED, true,
               StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);

    bmi270_tools::sensor_type_t base_sensors[] = {
        bmi270_tools::SENSOR_ACCEL,
        bmi270_tools::SENSOR_GYRO,
    };
    esp_err_t enable_ret = bmi270.enable_sensors(base_sensors, 2);
    if (enable_ret == ESP_OK) {
        set_status(StatusParam::IMU_ACC_ENABLED, true,
                   StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
        set_status(StatusParam::IMU_GYR_ENABLED, true,
                   StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
    } else {
        ESP_LOGW(task_tag, "Failed to enable ACC/GYR sensors: %s", esp_err_to_name(enable_ret));
    }
    if (enable_magnetometer && bmi270_was_initialized) {
        ESP_LOGI(task_tag, "Reconfiguring BMM150 magnetometer for reused BMI270");
        int mag_ret = bmi270.configure_magnetometer();
        if (mag_ret == 0) {
            ESP_LOGI(task_tag, "BMM150 magnetometer reconfigured successfully");
        } else {
            ESP_LOGW(task_tag, "BMM150 magnetometer reconfiguration failed: %d", mag_ret);
            set_status(StatusParam::IMU_MAG_ENABLED, false,
                       StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
        }
    } else if (!enable_magnetometer) {
        set_status(StatusParam::IMU_MAG_ENABLED, false,
                   StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::SYNC_UI);
    }

    // any-motion 中断仅睡眠唤醒场景需要，并附带 500ms 稳定延迟。
    // 常规界面（IMU 指南针等）传 configure_motion_interrupt=false，跳过以加快进入、降低开销。
    if (configure_motion_interrupt) {
        esp_err_t motion_ret = imu_configure_motion_interrupt();
        if (motion_ret != ESP_OK) {
            ESP_LOGW(task_tag, "BMI270 motion interrupt setup failed: %s", esp_err_to_name(motion_ret));
        }
    }

    bmi270_tools::sensor_data_t first_sample{};
    esp_err_t read_ret = imu_read_sensor_data(first_sample, true);
    if (read_ret != ESP_OK) {
        ESP_LOGW(task_tag, "Initial IMU sample failed: %s", esp_err_to_name(read_ret));
    }

    return imu_start_service(50);
}

esp_err_t m5_corep4_tool::imu_configure_motion_interrupt(void)
{
    const char *task_tag = "IMU_SERVICE";

    if (!bmi270.is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(task_tag, "Enabling BMI2_ACCEL and BMI2_ANY_MOTION features...");
    bmi270_tools::sensor_type_t motion_sensors[] = {
        bmi270_tools::SENSOR_ACCEL,
        static_cast<bmi270_tools::sensor_type_t>(BMI2_ANY_MOTION)
    };
    esp_err_t enable_ret = bmi270.enable_sensors(motion_sensors, 2);
    if (enable_ret != ESP_OK) {
        ESP_LOGE(task_tag, "Failed to enable BMI2_ANY_MOTION feature: %s", esp_err_to_name(enable_ret));
        return enable_ret;
    }

    bmi270_tools::motion_config_t motion_config;
    motion_config.duration = 0x0A;
    motion_config.threshold = 0x60;
    ESP_LOGI(task_tag, "Configuring any-motion with duration=%u (%.1fms), threshold=%u (%.1fmg)",
             motion_config.duration, motion_config.duration * 20.0f,
             motion_config.threshold, motion_config.threshold * 0.48f);
    esp_err_t motion_ret = bmi270.configure_any_motion(motion_config);
    if (motion_ret != ESP_OK) {
        ESP_LOGW(task_tag, "Failed to configure any-motion detection: %s", esp_err_to_name(motion_ret));
        return motion_ret;
    }

    ESP_LOGI(task_tag, "Configuring BMI270 interrupt...");
    esp_err_t int_enable_ret = bmi270.enable_interrupt(bmi270_tools::INT_PIN_1, false, true, false);
    if (int_enable_ret != ESP_OK) {
        ESP_LOGE(task_tag, "Failed to enable interrupt: %s", esp_err_to_name(int_enable_ret));
        return int_enable_ret;
    }

    esp_err_t int_map_ret = bmi270.map_interrupt_to_pin(BMI2_ANY_MOTION, bmi270_tools::INT_PIN_1);
    if (int_map_ret != ESP_OK) {
        ESP_LOGE(task_tag, "Failed to map interrupt: %s", esp_err_to_name(int_map_ret));
        return int_map_ret;
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(task_tag, "BMI270 motion interrupt configured");
    return ESP_OK;
}

esp_err_t m5_corep4_tool::imu_start_service(uint32_t period_ms)
{
    if (!bmi270.is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t new_period_ms = (period_ms == 0) ? 50 : period_ms;
    if (imu_service_task_handle != nullptr) {
        imu_service_period_ms = new_period_ms;
        imu_service_stop_requested = false;
        return ESP_OK;
    }

    imu_service_period_ms = new_period_ms;
    imu_service_stop_requested = false;
    BaseType_t ret = xTaskCreate(
        imu_service_task,
        "imu_service",
        4096,
        this,
        4,
        &imu_service_task_handle
    );

    if (ret != pdPASS) {
        imu_service_task_handle = nullptr;
        ESP_LOGE(TAG, "Failed to create IMU service task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "IMU service task started (%lu ms)", (unsigned long)imu_service_period_ms);
    return ESP_OK;
}

esp_err_t m5_corep4_tool::imu_stop_service(void)
{
    TaskHandle_t task_handle = imu_service_task_handle;
    if (task_handle != nullptr) {
        imu_service_stop_requested = true;
        xTaskNotifyGive(task_handle);

        if (xTaskGetCurrentTaskHandle() != task_handle) {
            const TickType_t timeout_ticks = pdMS_TO_TICKS(250);
            const TickType_t start_ticks = xTaskGetTickCount();
            while (imu_service_task_handle != nullptr &&
                   (xTaskGetTickCount() - start_ticks) < timeout_ticks) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            if (imu_service_task_handle != nullptr) {
                ESP_LOGW(TAG, "IMU service task did not exit in time, deleting it");
                vTaskDelete(task_handle);
                imu_service_task_handle = nullptr;
            }
        } else {
            imu_service_task_handle = nullptr;
        }

        ESP_LOGI(TAG, "IMU service task stopped");
    }

    if (imu_data_mutex != nullptr && xSemaphoreTake(imu_data_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        latest_imu_data = {};
        imu_data_valid = false;
        xSemaphoreGive(imu_data_mutex);
    }
    return ESP_OK;
}

esp_err_t m5_corep4_tool::imu_prepare_for_sleep(bool keep_motion_wake)
{
    const char *task_tag = "IMU_SERVICE";
    ESP_LOGI(task_tag, "Preparing IMU for sleep (motion_wake=%s)", keep_motion_wake ? "on" : "off");

    esp_err_t first_err = ESP_OK;
    auto keep_first_error = [&first_err](esp_err_t err) {
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
    };
    auto check_pm1 = [&first_err, task_tag](m5pm1_err_t err, const char *op) {
        if (err != M5PM1_OK) {
            ESP_LOGW(task_tag, "%s failed: %d", op, err);
            if (first_err == ESP_OK) {
                first_err = ESP_FAIL;
            }
        }
    };

    if (keep_motion_wake && !bmi270.is_initialized()) {
        ESP_LOGI(task_tag, "BMI wake requested, initializing minimal IMU path");
        set_status(StatusParam::POWER_PYB_L3B, true);
        vTaskDelay(pdMS_TO_TICKS(20));
        // 睡眠唤醒需要 any-motion 中断，懒初始化路径必须显式配置中断。
        keep_first_error(imu_init(false, bmi270_tools::MODE_BASE, true));
    } else if (keep_motion_wake && bmi270.is_initialized()) {
        // IMU 已被界面初始化（不带中断，imu_init configure_motion_interrupt=false）。
        // 此时 bmi270.prepare_for_sleep(true) 只会 enable/map INT1，并不会使能 ANY_MOTION
        // 特征或写阈值，需在此显式补配，否则从 IMU 界面直接睡眠时运动唤醒会失效。
        ESP_LOGI(task_tag, "BMI wake requested, (re)configuring any-motion interrupt on initialized IMU");
        keep_first_error(imu_configure_motion_interrupt());
    }

    keep_first_error(imu_stop_service());

    if (imu_data_mutex != nullptr) {
        xSemaphoreTake(imu_data_mutex, portMAX_DELAY);
    }
    if (bmi270.is_initialized()) {
        keep_first_error(bmi270.prepare_for_sleep(keep_motion_wake));
    } else {
        ESP_LOGI(task_tag, "BMI270 is not initialized, skip BMI sleep prep");
    }
    latest_imu_data = {};
    imu_data_valid = false;
    if (imu_data_mutex != nullptr) {
        xSemaphoreGive(imu_data_mutex);
    }

    if (keep_motion_wake) {
        check_pm1(pm1.gpioSetFunc(PYG0_IMU_INT, M5PM1_GPIO_FUNC_GPIO), "PM1 GPIO0 func");
        check_pm1(pm1.gpioSetMode(PYG0_IMU_INT, M5PM1_GPIO_MODE_INPUT), "PM1 GPIO0 input");
        check_pm1(pm1.gpioSetPull(PYG0_IMU_INT, M5PM1_GPIO_PULL_UP), "PM1 GPIO0 pull-up");
        check_pm1(pm1.gpioSetWakeEnable(PYG0_IMU_INT, true), "PM1 GPIO0 wake enable");
        check_pm1(pm1.gpioSetWakeEdge(PYG0_IMU_INT, M5PM1_GPIO_WAKE_FALLING), "PM1 GPIO0 wake edge");
        check_pm1(pm1.irqSetGpioMask(M5PM1_IRQ_GPIO0, M5PM1_IRQ_MASK_DISABLE), "PM1 GPIO0 IRQ unmask");
        ESP_LOGI(task_tag, "PM1 GPIO0 kept for BMI wake");
    } else {
        check_pm1(pm1.gpioSetWakeEnable(PYG0_IMU_INT, false), "PM1 GPIO0 wake disable");
        check_pm1(pm1.irqSetGpioMask(M5PM1_IRQ_GPIO0, M5PM1_IRQ_MASK_ENABLE), "PM1 GPIO0 IRQ mask");
        check_pm1(pm1.irqClearGpioAll(), "PM1 GPIO IRQ clear");
        check_pm1(pm1.gpioSetFunc(PYG0_IMU_INT, M5PM1_GPIO_FUNC_GPIO), "PM1 GPIO0 func");
        check_pm1(pm1.gpioSetMode(PYG0_IMU_INT, M5PM1_GPIO_MODE_INPUT), "PM1 GPIO0 input");
        check_pm1(pm1.gpioSetPull(PYG0_IMU_INT, M5PM1_GPIO_PULL_NONE), "PM1 GPIO0 no-pull");

        set_status(StatusParam::IMU_ACC_ENABLED, false, StatusConfigFlag::UPDATE_STATUS);
        set_status(StatusParam::IMU_GYR_ENABLED, false, StatusConfigFlag::UPDATE_STATUS);
        set_status(StatusParam::IMU_MAG_ENABLED, false, StatusConfigFlag::UPDATE_STATUS);
        ESP_LOGI(task_tag, "PM1 GPIO0 and BMI interrupt disabled for sleep");
    }

    return first_err;
}

esp_err_t m5_corep4_tool::imu_read_sensor_data(bmi270_tools::sensor_data_t& data, bool update_status)
{
    if (!bmi270.is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (imu_data_mutex == nullptr) {
        imu_data_mutex = xSemaphoreCreateMutex();
        if (imu_data_mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    data = {};
    xSemaphoreTake(imu_data_mutex, portMAX_DELAY);
    esp_err_t ret = bmi270.get_sensor_data(data);
    if (ret == ESP_OK && (data.acc_valid || data.gyr_valid || data.mag_valid)) {
        latest_imu_data = data;
        imu_data_valid = true;
    }
    xSemaphoreGive(imu_data_mutex);

    if (ret == ESP_OK && update_status) {
        imu_update_status_from_data(data);
    }

    return ret;
}

esp_err_t m5_corep4_tool::imu_get_latest_sensor_data(bmi270_tools::sensor_data_t& data)
{
    if (imu_data_mutex == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(imu_data_mutex, portMAX_DELAY);
    if (!imu_data_valid) {
        xSemaphoreGive(imu_data_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    data = latest_imu_data;
    xSemaphoreGive(imu_data_mutex);
    return ESP_OK;
}

esp_err_t m5_corep4_tool::imu_clear_interrupt(void)
{
    if (!bmi270.is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (imu_data_mutex != nullptr) {
        xSemaphoreTake(imu_data_mutex, portMAX_DELAY);
    }
    esp_err_t ret = bmi270.clear_interrupt();
    if (imu_data_mutex != nullptr) {
        xSemaphoreGive(imu_data_mutex);
    }
    return ret;
}

bool m5_corep4_tool::imu_is_initialized(void) const
{
    return bmi270.is_initialized();
}

void m5_corep4_tool::imu_update_status_from_data(const bmi270_tools::sensor_data_t& data,
                                                 StatusConfigFlag flags)
{
    if (data.acc_valid) {
        set_status(StatusParam::IMU_ACC_ENABLED, true, flags);
        set_status(StatusParam::IMU_ACC_X, data.acc_x, flags);
        set_status(StatusParam::IMU_ACC_Y, data.acc_y, flags);
        set_status(StatusParam::IMU_ACC_Z, data.acc_z, flags);
    }

    if (data.gyr_valid) {
        set_status(StatusParam::IMU_GYR_ENABLED, true, flags);
        set_status(StatusParam::IMU_GYR_X, data.gyr_x, flags);
        set_status(StatusParam::IMU_GYR_Y, data.gyr_y, flags);
        set_status(StatusParam::IMU_GYR_Z, data.gyr_z, flags);
    }

    if (data.mag_valid) {
        set_status(StatusParam::IMU_MAG_ENABLED, true, flags);
        set_status(StatusParam::IMU_MAG_X, data.mag_x, flags);
        set_status(StatusParam::IMU_MAG_Y, data.mag_y, flags);
        set_status(StatusParam::IMU_MAG_Z, data.mag_z, flags);
    }
}

void m5_corep4_tool::imu_service_task(void *pvParameters)
{
    m5_corep4_tool *self = static_cast<m5_corep4_tool*>(pvParameters);
    const char *task_tag = "IMU_SERVICE";
    uint32_t error_count = 0;

    while (!self->imu_service_stop_requested) {
        bmi270_tools::sensor_data_t data{};
        esp_err_t ret = self->imu_read_sensor_data(data, true);
        if (ret != ESP_OK && (++error_count % 40) == 1) {
            ESP_LOGW(task_tag, "IMU sample failed: %s", esp_err_to_name(ret));
        } else if (ret == ESP_OK) {
            error_count = 0;
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(self->imu_service_period_ms));
    }

    self->imu_service_task_handle = nullptr;
    self->imu_service_stop_requested = false;
    vTaskDelete(nullptr);
}

esp_err_t m5_corep4_tool::lvgl_custom_demo_task(void *pvParameters)
{
    (void)pvParameters; // 避免未使用参数警告
    const char *task_tag = "LVGL_DEMO_TASK";

    ESP_LOGI(task_tag, "LVGL Custom Demo task started");

    if (!imu_is_initialized()) {
        ESP_LOGE(task_tag, "Shared IMU service is not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(task_tag, "Using shared IMU service data");

    if (lvgl_custom_demo_stop_requested || lvgl_disp == nullptr) {
        ESP_LOGI(task_tag, "LVGL Custom Demo task stopped before UI update loop");
        return ESP_OK;
    }

    // 初始化时创建图表系列
    recreate_chart_series();
    
    // 初始化传感器显示参数
    switch_sensor_display(SENSOR_TYPE_ACC);

    // 主循环 - 根据当前选择的传感器类型读取数据并更新波形
    bmi270_tools::sensor_data_t sens_data{};
    
    while (!lvgl_custom_demo_stop_requested) {
        if (lvgl_disp == nullptr || ui_bmichart == nullptr) {
            ESP_LOGI(task_tag, "LVGL display/chart unavailable, stopping demo task");
            break;
        }

        // 检查是否需要切换传感器类型
        if (g_sensor_manager.sensor_changed) {
            recreate_chart_series();
        }
        
        // 读取共享IMU缓存，避免界面任务直接访问BMI270/BMM150总线
        if (this->imu_get_latest_sensor_data(sens_data) == ESP_OK) {
            float sensor_values[3] = {0.0f, 0.0f, 0.0f};
            bool data_valid = false;
            const char *sensor_name = "";
            
            // 根据当前传感器类型获取相应数据
            switch (g_sensor_manager.current_sensor) {
                case SENSOR_TYPE_ACC:
                    if (sens_data.acc_valid) {
                        sensor_values[0] = sens_data.acc_x;
                        sensor_values[1] = sens_data.acc_y;
                        sensor_values[2] = sens_data.acc_z;
                        data_valid = true;
                        sensor_name = "ACC";
                    }
                    break;
                case SENSOR_TYPE_GRY:
                    if (sens_data.gyr_valid) {
                        sensor_values[0] = sens_data.gyr_x;
                        sensor_values[1] = sens_data.gyr_y;
                        sensor_values[2] = sens_data.gyr_z;
                        data_valid = true;
                        sensor_name = "GRY";
                    }
                    break;
                case SENSOR_TYPE_MAG:
                    if (sens_data.mag_valid) {
                        sensor_values[0] = (float)sens_data.mag_x;
                        sensor_values[1] = (float)sens_data.mag_y;
                        sensor_values[2] = (float)sens_data.mag_z;
                        data_valid = true;
                        sensor_name = "MAG";
                    }
                    break;
                default:
                    break;
            }
            
            // 如果数据有效，更新波形显示
            if (data_valid) {
                // 数据标签对象数组，便于循环处理
                lv_obj_t* data_labels[] = {ui_bmilabxdata, ui_bmilabydata, ui_bmilabzdata};
                
                for (int i = 0; i < 3 && !lvgl_custom_demo_stop_requested; i++) {
                    if (g_sensor_manager.lines[i].series != NULL) {
                        // 将传感器值缩放到合适的显示范围 (0-200)
                        int display_value = (int)(sensor_values[i] * g_sensor_manager.lines[i].scale_factor + 
                                                  g_sensor_manager.lines[i].center_offset);
                        
                        // 限制显示值范围
                        if (display_value < 0) display_value = 0;
                        if (display_value > 200) display_value = 200;
                        
                        // 更新图表数据和数字显示标签时需要加锁
                        if (!lvgl_port_lock(100)) {
                            continue;
                        }

                        if (lvgl_custom_demo_stop_requested || lvgl_disp == nullptr ||
                            ui_bmichart == nullptr || g_sensor_manager.lines[i].series == NULL) {
                            lvgl_port_unlock();
                            break;
                        }
                        
                        // 更新图表数据
                        lv_chart_set_next_value(ui_bmichart, g_sensor_manager.lines[i].series, display_value);
                        
                        // 更新数字显示标签 - 智能格式化显示
                        if (data_labels[i] != NULL) {
                            char value_str[16];
                            float value = sensor_values[i];
                            
                            // 检查数值范围并格式化显示
                            if (value > 99.9f) {
                                strcpy(value_str, "MAX");
                            } else if (value < -99.9f) {
                                strcpy(value_str, "MIN");
                            } else if (value >= -9.99f && value <= 9.99f) {
                                // -9.99 到 +9.99 范围：显示2位小数
                                snprintf(value_str, sizeof(value_str), "%.2f", value);
                            } else {
                                // -99.9 到 -9.99 或 +9.99 到 +99.9 范围：显示1位小数
                                snprintf(value_str, sizeof(value_str), "%.1f", value);
                            }
                            
                            lv_label_set_text(data_labels[i], value_str);
                        }
                        
                        lvgl_port_unlock();
                    }
                }
                
            } else {
                ESP_LOGW(task_tag, "No valid %s data available", sensor_name);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50)); // 每50ms更新一次
    }

    ESP_LOGI(task_tag, "LVGL Custom Demo task exiting");
    return ESP_OK;
}

// ==================== LVGL鼠标输入支持 ====================

// LVGL鼠标读取回调函数
static void lvgl_mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    // 安全检查：确保data指针有效
    if (!data) {
        ESP_LOGE(TAG, "Invalid mouse data pointer");
        return;
    }
    
    // 默认设置为未按下状态
    data->state = LV_INDEV_STATE_RELEASED;
    data->point.x = 240;  // 默认屏幕中央
    data->point.y = 240;
    
    // 检查鼠标是否连接 - 如果没有连接，使用默认值
    if (!g_mouse_connected) {
        return; // 早期返回，使用默认值
    }
    
    // 读取当前鼠标数据并映射到屏幕坐标
    // 由于鼠标坐标是累计值，需要限制在屏幕范围内
    int16_t raw_x = g_mouse_data.x;
    int16_t raw_y = g_mouse_data.y;
    
    // 限制原始坐标范围在屏幕尺寸内 (0-480, 0-480)
    if (raw_x < 0) raw_x = 0;
    if (raw_x > 480) raw_x = 480;
    if (raw_y < 0) raw_y = 0;
    if (raw_y > 480) raw_y = 480;
    
    // 根据当前显示旋转角度自适应坐标转换
    int16_t screen_x, screen_y;
    switch (g_display_rotation) {
        case LV_DISPLAY_ROTATION_0:   // 0度 - 无变化
            screen_x = raw_x;
            screen_y = raw_y;
            break;
        case LV_DISPLAY_ROTATION_90:  // 90度 - X和Y交换，Y反向
            screen_x = raw_y;
            screen_y = 480 - raw_x;
            break;
        case LV_DISPLAY_ROTATION_180: // 180度 - X和Y都反向
            screen_x = 480 - raw_x;
            screen_y = 480 - raw_y;
            break;
        case LV_DISPLAY_ROTATION_270: // 270度 - X和Y交换，X反向
            screen_x = 480 - raw_y;
            screen_y = raw_x;
            break;
        default:
            screen_x = raw_x;
            screen_y = raw_y;
            break;
    }
    
    data->point.x = screen_x;
    data->point.y = screen_y;
    
    // 设置按钮状态（主要检查左键）
    if (g_mouse_data.left_button) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    
    // 处理滚轮（如果LVGL版本支持）
    #if LV_VERSION_CHECK(8, 0, 0)
    data->enc_diff = g_mouse_data.wheel;
    #endif
}

// ==================== LVGL触摸指示器定时器回调 ====================

// LVGL触摸指示器定时器回调函数 - 定期检查触摸状态并更新蓝色指示器和坐标显示
static void lvgl_touch_indicator_timer_cb(lv_timer_t *timer)
{
    static bool last_label_valid = false;
    static lv_indev_state_t last_label_state = LV_INDEV_STATE_RELEASED;
    static int32_t last_label_x = INT32_MIN;
    static int32_t last_label_y = INT32_MIN;

    // 安全检查
    if (!g_corep4_instance) {
        return;
    }
    
    // 获取触摸输入设备
    lv_indev_t *touch_indev = g_corep4_instance->get_lvgl_touch_indev();
    if (!touch_indev) {
        return;
    }
    
    // 获取触摸状态
    lv_indev_state_t touch_state = lv_indev_get_state(touch_indev);
    
    // 策略：使用 LVGL 的坐标（已经过转换）
    lv_point_t touch_point;
    lv_indev_get_point(touch_indev, &touch_point);
    
    // 实时更新触摸坐标显示（安全的UI更新）
    if (ui_touchlabxdata && ui_touchlabydata) {
        bool label_changed = !last_label_valid || touch_state != last_label_state;
        if (touch_state == LV_INDEV_STATE_PRESSED) {
            label_changed = label_changed || touch_point.x != last_label_x || touch_point.y != last_label_y;
        }

        if (label_changed) {
            char x_str[16], y_str[16];

            if (touch_state == LV_INDEV_STATE_PRESSED) {
                // 有触摸时显示当前坐标
                snprintf(x_str, sizeof(x_str), "%ld", (long)touch_point.x);
                snprintf(y_str, sizeof(y_str), "%ld", (long)touch_point.y);
            } else {
                // 无触摸时显示 "--"
                strcpy(x_str, "--");
                strcpy(y_str, "--");
            }

            // 安全地更新标签文本
            lv_label_set_text(ui_touchlabxdata, x_str);
            lv_label_set_text(ui_touchlabydata, y_str);

            last_label_valid = true;
            last_label_state = touch_state;
            last_label_x = touch_point.x;
            last_label_y = touch_point.y;
        }
    } else {
        last_label_valid = false;
    }
    
    // 如果没有触摸，直接隐藏指示器
    if (touch_state != LV_INDEV_STATE_PRESSED) {
        g_corep4_instance->update_touch_indicator(0, 0, false);
        return;
    }
    
    // LVGL input driver already owns hardware polling. Reuse its cached point only.
    g_corep4_instance->update_touch_indicator(touch_point.x, touch_point.y, true);
}

// ==================== USB HID鼠标功能实现 ====================

// 前向声明
static void usb_hid_interface_event_callback(hid_host_device_handle_t hid_device_handle,
                                            const hid_host_interface_event_t event,
                                            void *arg);

static uint8_t usb_monitor_class_flags_from_desc(usb_device_handle_t dev_hdl,
                                                 const usb_device_desc_t *dev_desc)
{
    uint8_t flags = 0;
    if (!dev_desc) {
        return flags;
    }

    if (dev_desc->bDeviceClass == USB_CLASS_VIDEO) {
        flags |= USB_MON_CLASS_VIDEO;
    } else if (dev_desc->bDeviceClass == USB_CLASS_HID) {
        flags |= USB_MON_CLASS_HID;
    } else if (dev_desc->bDeviceClass == USB_CLASS_HUB) {
        flags |= USB_MON_CLASS_HUB;
    }

    const usb_config_desc_t *config_desc = NULL;
    if (usb_host_get_active_config_descriptor(dev_hdl, &config_desc) != ESP_OK || !config_desc) {
        return flags;
    }

    int offset = 0;
    const usb_intf_desc_t *intf_desc = (const usb_intf_desc_t *)usb_parse_next_descriptor_of_type(
        (const usb_standard_desc_t *)config_desc,
        config_desc->wTotalLength,
        USB_B_DESCRIPTOR_TYPE_INTERFACE,
        &offset);

    while (intf_desc) {
        if (intf_desc->bInterfaceClass == USB_CLASS_VIDEO) {
            flags |= USB_MON_CLASS_VIDEO;
        } else if (intf_desc->bInterfaceClass == USB_CLASS_HID) {
            flags |= USB_MON_CLASS_HID;
            if (intf_desc->bInterfaceProtocol == HID_PROTOCOL_MOUSE) {
                flags |= USB_MON_CLASS_MOUSE;
            } else if (intf_desc->bInterfaceProtocol == HID_PROTOCOL_KEYBOARD) {
                flags |= USB_MON_CLASS_KEYBOARD;
            }
        } else if (intf_desc->bInterfaceClass == USB_CLASS_HUB) {
            flags |= USB_MON_CLASS_HUB;
        }

        intf_desc = (const usb_intf_desc_t *)usb_parse_next_descriptor_of_type(
            (const usb_standard_desc_t *)intf_desc,
            config_desc->wTotalLength,
            USB_B_DESCRIPTOR_TYPE_INTERFACE,
            &offset);
    }

    return flags;
}

static void usb_string_desc_to_ascii(const usb_str_desc_t *desc, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!desc || desc->bLength <= USB_STR_DESC_SIZE) {
        return;
    }

    const size_t char_count = (desc->bLength - USB_STR_DESC_SIZE) / sizeof(uint16_t);
    size_t out_pos = 0;
    for (size_t i = 0; i < char_count && out_pos + 1 < out_len; ++i) {
        const uint16_t ch = desc->wData[i];
        if (ch >= 0x20 && ch <= 0x7E) {
            out[out_pos++] = static_cast<char>(ch);
        } else if (ch == 0x09 || ch == 0x0A || ch == 0x0D) {
            out[out_pos++] = ' ';
        }
    }
    out[out_pos] = '\0';
}

static void usb_monitor_make_device_name(usb_device_handle_t dev_hdl, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    usb_device_info_t dev_info = {};
    if (usb_host_device_info(dev_hdl, &dev_info) != ESP_OK) {
        return;
    }

    char product[32];
    char manufacturer[24];
    usb_string_desc_to_ascii(dev_info.str_desc_product, product, sizeof(product));
    usb_string_desc_to_ascii(dev_info.str_desc_manufacturer, manufacturer, sizeof(manufacturer));

    if (product[0] != '\0' && manufacturer[0] != '\0') {
        std::snprintf(out, out_len, "%.18s %.26s", manufacturer, product);
    } else if (product[0] != '\0') {
        std::snprintf(out, out_len, "%.46s", product);
    } else if (manufacturer[0] != '\0') {
        std::snprintf(out, out_len, "%.46s", manufacturer);
    }
}

static void usb_monitor_track_device(uint8_t address)
{
    if (!g_usb_monitor_client) {
        return;
    }

    usb_device_handle_t dev_hdl = NULL;
    esp_err_t ret = usb_host_device_open(g_usb_monitor_client, address, &dev_hdl);
    if (ret != ESP_OK || !dev_hdl) {
        ESP_LOGW(TAG, "USB monitor open addr %u failed: %s",
                 static_cast<unsigned>(address), esp_err_to_name(ret));
        return;
    }

    const usb_device_desc_t *dev_desc = NULL;
    ret = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
    if (ret != ESP_OK || !dev_desc) {
        usb_host_device_close(g_usb_monitor_client, dev_hdl);
        ESP_LOGW(TAG, "USB monitor descriptor failed: %s", esp_err_to_name(ret));
        return;
    }

    const bool locked = (g_usb_monitor_mutex == NULL) ||
                        (xSemaphoreTake(g_usb_monitor_mutex, pdMS_TO_TICKS(1000)) == pdTRUE);
    if (!locked) {
        usb_host_device_close(g_usb_monitor_client, dev_hdl);
        ESP_LOGW(TAG, "USB monitor table lock timeout while tracking addr %u",
                 static_cast<unsigned>(address));
        return;
    }

    bool stored = false;
    for (int i = 0; i < USB_MONITOR_MAX_DEVICES; ++i) {
        if (!g_usb_monitor_devices[i].active) {
            g_usb_monitor_devices[i].active = true;
            g_usb_monitor_devices[i].address = address;
            g_usb_monitor_devices[i].vid = dev_desc->idVendor;
            g_usb_monitor_devices[i].pid = dev_desc->idProduct;
            g_usb_monitor_devices[i].class_flags =
                usb_monitor_class_flags_from_desc(dev_hdl, dev_desc);
            usb_monitor_make_device_name(dev_hdl,
                                         g_usb_monitor_devices[i].name,
                                         sizeof(g_usb_monitor_devices[i].name));
            g_usb_monitor_devices[i].dev_hdl = dev_hdl;
            ESP_LOGI(TAG, "USB monitor addr=%u vid=0x%04X pid=0x%04X flags=0x%02X name=%s",
                     static_cast<unsigned>(address),
                     static_cast<unsigned>(g_usb_monitor_devices[i].vid),
                     static_cast<unsigned>(g_usb_monitor_devices[i].pid),
                     static_cast<unsigned>(g_usb_monitor_devices[i].class_flags),
                     g_usb_monitor_devices[i].name[0] ? g_usb_monitor_devices[i].name : "--");
            stored = true;
            break;
        }
    }

    if (g_usb_monitor_mutex != NULL) {
        xSemaphoreGive(g_usb_monitor_mutex);
    }

    if (stored) {
        return;
    }

    usb_host_device_close(g_usb_monitor_client, dev_hdl);
    ESP_LOGW(TAG, "USB monitor device table full");
}

static void usb_monitor_forget_device(usb_device_handle_t dev_hdl)
{
    if (!g_usb_monitor_client || !dev_hdl) {
        return;
    }

    const bool locked = (g_usb_monitor_mutex == NULL) ||
                        (xSemaphoreTake(g_usb_monitor_mutex, pdMS_TO_TICKS(1000)) == pdTRUE);
    if (!locked) {
        ESP_LOGW(TAG, "USB monitor table lock timeout while forgetting device");
        return;
    }

    bool found = false;
    for (int i = 0; i < USB_MONITOR_MAX_DEVICES; ++i) {
        if (g_usb_monitor_devices[i].active &&
            g_usb_monitor_devices[i].dev_hdl == dev_hdl) {
            memset(&g_usb_monitor_devices[i], 0, sizeof(g_usb_monitor_devices[i]));
            found = true;
            break;
        }
    }

    if (g_usb_monitor_mutex != NULL) {
        xSemaphoreGive(g_usb_monitor_mutex);
    }

    if (found) {
        usb_host_device_close(g_usb_monitor_client, dev_hdl);
    }
}

static void usb_monitor_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    (void)arg;
    if (!event_msg) {
        return;
    }

    switch (event_msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            usb_monitor_track_device(event_msg->new_dev.address);
            break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            usb_monitor_forget_device(event_msg->dev_gone.dev_hdl);
            break;
        default:
            break;
    }
}

static void usb_device_monitor_task(void *arg)
{
    (void)arg;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = usb_monitor_client_event_cb,
            .callback_arg = NULL,
        },
    };

    esp_err_t ret = usb_host_client_register(&client_config, &g_usb_monitor_client);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "USB monitor client register failed: %s", esp_err_to_name(ret));
        g_usb_monitor_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        ret = usb_host_client_handle_events(g_usb_monitor_client, pdMS_TO_TICKS(500));
        if (ret == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "USB monitor events failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

struct UsbHostLibraryInitContext {
    SemaphoreHandle_t done = nullptr;
    esp_err_t result = ESP_FAIL;
};

static void complete_usb_host_library_init(UsbHostLibraryInitContext *init,
                                           esp_err_t result)
{
    if (init == nullptr) return;
    init->result = result;
    if (init->done != nullptr) xSemaphoreGive(init->done);
}

// USB Host任务处理函数
static void usb_host_lib_task(void *arg)
{
    auto *init = static_cast<UsbHostLibraryInitContext *>(arg);
    usb_host_config_t host_config;
    memset(&host_config, 0, sizeof(host_config));
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    host_config.peripheral_map = (1U << 0); // ESP32-P4 USB-A 2.0 host uses HS DWC/UTMI.

    esp_err_t err = usb_host_install(&host_config);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "USB Host library already installed");
        complete_usb_host_library_init(init, ESP_OK);
        vTaskDelete(NULL);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB Host library: %s", esp_err_to_name(err));
        complete_usb_host_library_init(init, err);
        vTaskDelete(NULL);
        return;
    }

    complete_usb_host_library_init(init, ESP_OK);

    // 持续处理USB Host事件，允许后续插拔设备
    while (true) {
        uint32_t event_flags = 0;
        err = usb_host_lib_handle_events(pdMS_TO_TICKS(1000), &event_flags);
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_lib_handle_events failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGV(TAG, "USB Host currently has no registered clients");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            err = usb_host_device_free_all();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "USB Host devices freed");
            } else if (err != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "usb_host_device_free_all returned %s", esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static bool get_hid_params(hid_host_device_handle_t hid_device_handle, hid_host_dev_params_t* params)
{
    if (!hid_device_handle || !params) {
        return false;
    }
    memset(params, 0, sizeof(hid_host_dev_params_t));
    return hid_host_device_get_params(hid_device_handle, params) == ESP_OK;
}

static void append_keyboard_modifier_part(char* out, size_t out_len, const char* part)
{
    if (!out || out_len == 0 || !part) {
        return;
    }

    const size_t used = std::strlen(out);
    if (used >= out_len - 1) {
        return;
    }

    std::snprintf(out + used,
                  out_len - used,
                  "%s%s",
                  used > 0 ? "+" : "",
                  part);
}

static void format_keyboard_modifier_name(uint8_t modifier, char* out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }

    out[0] = '\0';
    if (modifier & 0x01) append_keyboard_modifier_part(out, out_len, "LCtrl");
    if (modifier & 0x02) append_keyboard_modifier_part(out, out_len, "LShift");
    if (modifier & 0x04) append_keyboard_modifier_part(out, out_len, "LAlt");
    if (modifier & 0x08) append_keyboard_modifier_part(out, out_len, "LGui");
    if (modifier & 0x10) append_keyboard_modifier_part(out, out_len, "RCtrl");
    if (modifier & 0x20) append_keyboard_modifier_part(out, out_len, "RShift");
    if (modifier & 0x40) append_keyboard_modifier_part(out, out_len, "RAlt");
    if (modifier & 0x80) append_keyboard_modifier_part(out, out_len, "RGui");
}

static void format_keyboard_key_name(uint8_t key_code, char* out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (key_code >= 0x04 && key_code <= 0x1D) {
        std::snprintf(out, out_len, "%c", static_cast<char>('A' + key_code - 0x04));
        return;
    }
    if (key_code >= 0x1E && key_code <= 0x26) {
        std::snprintf(out, out_len, "%c", static_cast<char>('1' + key_code - 0x1E));
        return;
    }

    switch (key_code) {
        case 0x00: std::snprintf(out, out_len, "None"); break;
        case 0x27: std::snprintf(out, out_len, "0"); break;
        case 0x28: std::snprintf(out, out_len, "Enter"); break;
        case 0x29: std::snprintf(out, out_len, "Esc"); break;
        case 0x2A: std::snprintf(out, out_len, "Backspace"); break;
        case 0x2B: std::snprintf(out, out_len, "Tab"); break;
        case 0x2C: std::snprintf(out, out_len, "Space"); break;
        case 0x2D: std::snprintf(out, out_len, "-"); break;
        case 0x2E: std::snprintf(out, out_len, "="); break;
        case 0x2F: std::snprintf(out, out_len, "["); break;
        case 0x30: std::snprintf(out, out_len, "]"); break;
        case 0x31: std::snprintf(out, out_len, "Backslash"); break;
        case 0x32: std::snprintf(out, out_len, "IntlHash"); break;
        case 0x33: std::snprintf(out, out_len, "Semicolon"); break;
        case 0x34: std::snprintf(out, out_len, "Quote"); break;
        case 0x35: std::snprintf(out, out_len, "Grave"); break;
        case 0x36: std::snprintf(out, out_len, "Comma"); break;
        case 0x37: std::snprintf(out, out_len, "Dot"); break;
        case 0x38: std::snprintf(out, out_len, "Slash"); break;
        case 0x39: std::snprintf(out, out_len, "CapsLock"); break;
        case 0x46: std::snprintf(out, out_len, "PrintScr"); break;
        case 0x47: std::snprintf(out, out_len, "ScrollLock"); break;
        case 0x48: std::snprintf(out, out_len, "Pause"); break;
        case 0x49: std::snprintf(out, out_len, "Insert"); break;
        case 0x4A: std::snprintf(out, out_len, "Home"); break;
        case 0x4B: std::snprintf(out, out_len, "PageUp"); break;
        case 0x4C: std::snprintf(out, out_len, "Delete"); break;
        case 0x4D: std::snprintf(out, out_len, "End"); break;
        case 0x4E: std::snprintf(out, out_len, "PageDown"); break;
        case 0x4F: std::snprintf(out, out_len, "Right"); break;
        case 0x50: std::snprintf(out, out_len, "Left"); break;
        case 0x51: std::snprintf(out, out_len, "Down"); break;
        case 0x52: std::snprintf(out, out_len, "Up"); break;
        case 0x53: std::snprintf(out, out_len, "NumLock"); break;
        case 0x54: std::snprintf(out, out_len, "KP /"); break;
        case 0x55: std::snprintf(out, out_len, "KP *"); break;
        case 0x56: std::snprintf(out, out_len, "KP -"); break;
        case 0x57: std::snprintf(out, out_len, "KP +"); break;
        case 0x58: std::snprintf(out, out_len, "KP Enter"); break;
        case 0x59: std::snprintf(out, out_len, "KP 1"); break;
        case 0x5A: std::snprintf(out, out_len, "KP 2"); break;
        case 0x5B: std::snprintf(out, out_len, "KP 3"); break;
        case 0x5C: std::snprintf(out, out_len, "KP 4"); break;
        case 0x5D: std::snprintf(out, out_len, "KP 5"); break;
        case 0x5E: std::snprintf(out, out_len, "KP 6"); break;
        case 0x5F: std::snprintf(out, out_len, "KP 7"); break;
        case 0x60: std::snprintf(out, out_len, "KP 8"); break;
        case 0x61: std::snprintf(out, out_len, "KP 9"); break;
        case 0x62: std::snprintf(out, out_len, "KP 0"); break;
        case 0x63: std::snprintf(out, out_len, "KP ."); break;
        case 0x64: std::snprintf(out, out_len, "IntlBackslash"); break;
        case 0x65: std::snprintf(out, out_len, "App"); break;
        case 0x66: std::snprintf(out, out_len, "Power"); break;
        case 0x67: std::snprintf(out, out_len, "KP ="); break;
        default:
            if (key_code >= 0x3A && key_code <= 0x45) {
                std::snprintf(out, out_len, "F%u", static_cast<unsigned>(key_code - 0x39));
            } else if (key_code >= 0x68 && key_code <= 0x73) {
                std::snprintf(out, out_len, "F%u", static_cast<unsigned>(key_code - 0x5B));
            } else {
                std::snprintf(out, out_len, "0x%02X", key_code);
            }
            break;
    }
}

static void update_keyboard_report(const uint8_t* data, size_t data_length)
{
    if (!data) {
        return;
    }

    const size_t report_offset = data_length >= 9 ? 1 : 0;
    if (data_length < report_offset + 8) {
        return;
    }

    const uint8_t* boot_report = data + report_offset;
    g_keyboard_data.modifier = boot_report[0];
    memcpy(g_keyboard_data.keycodes, &boot_report[2], sizeof(g_keyboard_data.keycodes));
    g_keyboard_data.connected = true;
    g_keyboard_data.sequence++;

    uint8_t last_key = 0;
    for (size_t i = 0; i < sizeof(g_keyboard_data.keycodes); ++i) {
        if (g_keyboard_data.keycodes[i] != 0) {
            last_key = g_keyboard_data.keycodes[i];
            break;
        }
    }

    if (last_key != 0) {
        g_keyboard_data.last_key_code = last_key;
        format_keyboard_key_name(last_key, g_keyboard_data.last_key_name, sizeof(g_keyboard_data.last_key_name));
    } else if (g_keyboard_data.modifier != 0) {
        g_keyboard_data.last_key_code = 0;
        format_keyboard_modifier_name(g_keyboard_data.modifier,
                                      g_keyboard_data.last_key_name,
                                      sizeof(g_keyboard_data.last_key_name));
    } else if (g_keyboard_data.last_key_name[0] == '\0') {
        format_keyboard_key_name(0, g_keyboard_data.last_key_name, sizeof(g_keyboard_data.last_key_name));
    }
}

// USB HID驱动事件回调函数
static void usb_hid_driver_event_callback(hid_host_device_handle_t hid_device_handle,
                                         const hid_host_driver_event_t event,
                                         void *arg) {
    switch (event) {
        case HID_HOST_DRIVER_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "HID Device connected");

            hid_host_dev_params_t dev_params = {};
            const bool params_ok = get_hid_params(hid_device_handle, &dev_params);
            const bool is_keyboard = params_ok && dev_params.proto == HID_PROTOCOL_KEYBOARD;
            const bool is_mouse = params_ok && dev_params.proto == HID_PROTOCOL_MOUSE;
            ESP_LOGI(TAG, "HID params: proto=%u iface=%u addr=%u",
                     params_ok ? static_cast<unsigned>(dev_params.proto) : 0,
                     params_ok ? static_cast<unsigned>(dev_params.iface_num) : 0,
                     params_ok ? static_cast<unsigned>(dev_params.addr) : 0);
            
            // 开始接收HID设备数据
            hid_host_device_config_t dev_config = {
                .callback = usb_hid_interface_event_callback,
                .callback_arg = NULL
            };
            
            esp_err_t ret = hid_host_device_open(hid_device_handle, &dev_config);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "HID Device opened successfully");
                hid_host_device_start(hid_device_handle);
                if (is_keyboard) {
                    g_keyboard_device_handle = hid_device_handle;
                    g_keyboard_connected = true;
                    memset(&g_keyboard_data, 0, sizeof(g_keyboard_data));
                    g_keyboard_data.connected = true;
                    g_keyboard_data.sequence++;
                    snprintf(g_keyboard_data.last_key_name, sizeof(g_keyboard_data.last_key_name), "Connected");
                    ESP_LOGI(TAG, "Keyboard HID interface opened");
                } else if (is_mouse) {
                    g_mouse_device_handle = hid_device_handle;
                    g_mouse_connected = true;  // 标记鼠标已连接
                    // 初始化鼠标位置到屏幕中央
                    g_mouse_data.x = 240;  // 屏幕中央X坐标 (480/2 = 240)
                    g_mouse_data.y = 240;  // 屏幕中央Y坐标 (480/2 = 240)
                    g_mouse_data.connected = true;
                    ESP_LOGI(TAG, "Mouse initialized at screen center (240, 240)");
                } else {
                    ESP_LOGI(TAG, "Generic HID protocol %u opened; raw report parser not configured",
                             params_ok ? static_cast<unsigned>(dev_params.proto) : 0);
                }
            } else {
                ESP_LOGE(TAG, "Failed to open HID Device: %s", esp_err_to_name(ret));
                if (is_keyboard) {
                    g_keyboard_connected = false;
                } else if (is_mouse) {
                    g_mouse_connected = false;
                }
            }
            break;
        }
            
        default:
            ESP_LOGW(TAG, "Unknown HID Driver event: %d", event);
            break;
    }
}

// USB HID接口事件回调函数
static void usb_hid_interface_event_callback(hid_host_device_handle_t hid_device_handle,
                                            const hid_host_interface_event_t event,
                                            void *arg) {
    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
            // 处理输入报告
            uint8_t data[16];
            size_t data_length = 0;
            esp_err_t ret = hid_host_device_get_raw_input_report_data(hid_device_handle, 
                                                                     data, 
                                                                     sizeof(data), 
                                                                     &data_length);
            hid_host_dev_params_t dev_params = {};
            const bool params_ok = get_hid_params(hid_device_handle, &dev_params);
            if (ret == ESP_OK && params_ok && dev_params.proto == HID_PROTOCOL_KEYBOARD && data_length >= 8) {
                update_keyboard_report(data, data_length);
            } else if (ret == ESP_OK && params_ok && dev_params.proto == HID_PROTOCOL_MOUSE && data_length >= 3) {
                // 解析鼠标数据（标准HID鼠标报告格式）
                uint8_t buttons = data[0];
                int8_t dx = (int8_t)data[1];
                int8_t dy = (int8_t)data[2];
                int8_t wheel = (data_length >= 4) ? (int8_t)data[3] : 0;
                
                // 解析按钮状态
                bool left_button = (buttons & 0x01) != 0;
                bool right_button = (buttons & 0x02) != 0;
                bool middle_button = (buttons & 0x04) != 0;
                
                // 更新全局鼠标数据
                g_mouse_data.x += dx;  // 累计X坐标
                g_mouse_data.y += dy;  // 累计Y坐标
                g_mouse_data.wheel = wheel;
                g_mouse_data.left_button = left_button;
                g_mouse_data.right_button = right_button;
                g_mouse_data.middle_button = middle_button;
                g_mouse_data.connected = true;
                
                // 限制坐标范围在屏幕尺寸内 (0-480)
                if (g_mouse_data.x < 0) g_mouse_data.x = 0;
                if (g_mouse_data.x > 480) g_mouse_data.x = 480;
                if (g_mouse_data.y < 0) g_mouse_data.y = 0;
                if (g_mouse_data.y > 480) g_mouse_data.y = 480;
                
                // 只在有按钮按下或移动幅度较大时打印（减少日志）
                // if (buttons != 0 || abs(dx) > 2 || abs(dy) > 2 || wheel != 0) {
                //     ESP_LOGI(TAG, "Mouse: X=%d, Y=%d, buttons=0x%02X, wheel=%d", 
                //              g_mouse_data.x, g_mouse_data.y, buttons, wheel);
                // }
            }
            break;
        }
        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGW(TAG, "HID Interface transfer error");
            break;
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HID Interface disconnected");
        {
            const bool was_mouse = hid_device_handle == g_mouse_device_handle;
            const bool was_keyboard = hid_device_handle == g_keyboard_device_handle;
            hid_host_device_close(hid_device_handle);

            if (was_keyboard) {
                g_keyboard_connected = false;
                g_keyboard_device_handle = NULL;
                memset(&g_keyboard_data, 0, sizeof(g_keyboard_data));
            }

            if (was_mouse) {
                g_mouse_connected = false;  // 标记鼠标已断开
                g_mouse_device_handle = NULL;
                // 清零鼠标数据
                memset(&g_mouse_data, 0, sizeof(g_mouse_data));

                // 发送鼠标断开事件，让鼠标检查任务处理LVGL对象销毁
                if (g_corep4_instance) {
                    g_corep4_instance->notify_mouse_disconnected();
                }
            }
            break;
        }
        default:
            ESP_LOGW(TAG, "Unknown HID Interface event: %d", event);
            break;
    }
}

esp_err_t m5_corep4_tool::usb_hid_mouse_init() {
    if (usb_host_initialized) {
        ESP_LOGW(TAG, "USB Host already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing USB HID Mouse Host...");
    
    // 创建USB Host任务
    UsbHostLibraryInitContext init_context = {};
    init_context.done = xSemaphoreCreateBinary();
    if (init_context.done == nullptr) {
        ESP_LOGE(TAG, "Failed to create USB Host initialization semaphore");
        return ESP_ERR_NO_MEM;
    }
    BaseType_t task_created = xTaskCreate(usb_host_lib_task,
                                         "usb_host_task",
                                         4096,
                                         &init_context,
                                         2,
                                         NULL);
    
    if (task_created != pdTRUE) {
        vSemaphoreDelete(init_context.done);
        ESP_LOGE(TAG, "Failed to create USB Host task");
        return ESP_ERR_NO_MEM;
    }
    
    // 等待USB Host任务完成初始化
    xSemaphoreTake(init_context.done, portMAX_DELAY);
    vSemaphoreDelete(init_context.done);
    init_context.done = nullptr;
    if (init_context.result != ESP_OK) {
        ESP_LOGE(TAG,
                 "USB Host Library initialization failed: %s",
                 esp_err_to_name(init_context.result));
        return init_context.result;
    }
    ESP_LOGI(TAG, "USB Host Library installed successfully");
    
    // 安装HID Host驱动
    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4 * 1024,
        .core_id = 0,
        .callback = usb_hid_driver_event_callback,
        .callback_arg = NULL
    };
    
    esp_err_t ret = hid_host_install(&hid_config);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "USB HID Host already installed");
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB HID Host: %s", esp_err_to_name(ret));
        return ret;
    }
    
    usb_host_initialized = true;
    
    // 初始化鼠标数据结构
    memset(&current_mouse_data, 0, sizeof(usb_mouse_data_t));
    
    ESP_LOGI(TAG, "USB HID Host installed successfully");
    ESP_LOGI(TAG, "Please plug in a USB HID mouse to the USB2.0 port");
    
    return ESP_OK;
}

esp_err_t m5_corep4_tool::get_usb_mouse_data(usb_mouse_data_t* mouse_data) {
    if (!mouse_data) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!usb_host_initialized) {
        ESP_LOGE(TAG, "USB Host not initialized, call usb_hid_mouse_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!g_mouse_connected) {
        return ESP_ERR_NOT_FOUND;
    }
    
    // 复制全局鼠标数据
    memcpy(mouse_data, &g_mouse_data, sizeof(usb_mouse_data_t));
    mouse_data->connected = g_mouse_connected;
    
    return ESP_OK;
}

esp_err_t m5_corep4_tool::get_usb_keyboard_data(usb_keyboard_data_t* keyboard_data) {
    if (!keyboard_data) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!usb_host_initialized) {
        ESP_LOGE(TAG, "USB Host not initialized, call usb_hid_mouse_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    if (!g_keyboard_connected) {
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(keyboard_data, &g_keyboard_data, sizeof(usb_keyboard_data_t));
    keyboard_data->connected = g_keyboard_connected;

    return ESP_OK;
}

bool m5_corep4_tool::is_usb_mouse_connected() {
    return usb_host_initialized && g_mouse_connected;
}

bool m5_corep4_tool::is_usb_keyboard_connected() {
    return usb_host_initialized && g_keyboard_connected;
}

esp_err_t m5_corep4_tool::init_usb_device_monitor() {
    if (g_usb_monitor_mutex == NULL) {
        g_usb_monitor_mutex = xSemaphoreCreateMutex();
        if (g_usb_monitor_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create USB monitor mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_usb_monitor_task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t task_created = xTaskCreate(
        usb_device_monitor_task,
        "usb_monitor",
        4096,
        NULL,
        4,
        &g_usb_monitor_task_handle
    );

    if (task_created != pdTRUE) {
        g_usb_monitor_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create USB monitor task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t m5_corep4_tool::clear_usb_device_monitor_cache() {
    if (g_usb_monitor_mutex == NULL) {
        g_usb_monitor_mutex = xSemaphoreCreateMutex();
        if (g_usb_monitor_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create USB monitor mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(g_usb_monitor_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "USB monitor cache clear lock timeout");
        return ESP_ERR_TIMEOUT;
    }

    int cleared = 0;
    usb_device_handle_t handles[USB_MONITOR_MAX_DEVICES] = {};
    for (int i = 0; i < USB_MONITOR_MAX_DEVICES; ++i) {
        if (g_usb_monitor_devices[i].active) {
            handles[cleared++] = g_usb_monitor_devices[i].dev_hdl;
            memset(&g_usb_monitor_devices[i], 0, sizeof(g_usb_monitor_devices[i]));
        }
    }

    xSemaphoreGive(g_usb_monitor_mutex);

    for (int i = 0; i < cleared; ++i) {
        if (g_usb_monitor_client != NULL && handles[i] != NULL) {
            const esp_err_t close_ret = usb_host_device_close(g_usb_monitor_client, handles[i]);
            if (close_ret != ESP_OK) {
                ESP_LOGW(TAG, "USB monitor cache close handle failed: %s", esp_err_to_name(close_ret));
            }
        }
    }

    ESP_LOGI(TAG, "USB monitor cache cleared, entries=%d", cleared);
    return ESP_OK;
}

int m5_corep4_tool::get_usb_other_device_count() {
    int count = 0;
    const bool locked = (g_usb_monitor_mutex == NULL) ||
                        (xSemaphoreTake(g_usb_monitor_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
    if (!locked) {
        return 0;
    }
    for (int i = 0; i < USB_MONITOR_MAX_DEVICES; ++i) {
        const uint8_t known_flags = USB_MON_CLASS_VIDEO |
                                    USB_MON_CLASS_MOUSE |
                                    USB_MON_CLASS_KEYBOARD |
                                    USB_MON_CLASS_HUB |
                                    USB_MON_CLASS_HID;
        if (g_usb_monitor_devices[i].active &&
            (g_usb_monitor_devices[i].class_flags & known_flags) == 0) {
            ++count;
        }
    }
    if (locked && g_usb_monitor_mutex != NULL) {
        xSemaphoreGive(g_usb_monitor_mutex);
    }
    return count;
}

int m5_corep4_tool::get_usb_device_count() {
    int count = 0;
    const bool locked = (g_usb_monitor_mutex == NULL) ||
                        (xSemaphoreTake(g_usb_monitor_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
    if (!locked) {
        return 0;
    }
    for (int i = 0; i < USB_MONITOR_MAX_DEVICES; ++i) {
        if (g_usb_monitor_devices[i].active) {
            ++count;
        }
    }
    if (locked && g_usb_monitor_mutex != NULL) {
        xSemaphoreGive(g_usb_monitor_mutex);
    }
    return count;
}

bool m5_corep4_tool::get_usb_device_info(char* name,
                                         size_t name_len,
                                         uint16_t* vid,
                                         uint16_t* pid,
                                         uint8_t* class_flags,
                                         int* count) {
    int device_count = 0;
    int first_index = -1;
    usb_monitor_device_t first_device = {};

    const bool locked = (g_usb_monitor_mutex == NULL) ||
                        (xSemaphoreTake(g_usb_monitor_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
    if (!locked) {
        if (count) {
            *count = 0;
        }
        if (name && name_len > 0) {
            name[0] = '\0';
        }
        if (vid) {
            *vid = 0;
        }
        if (pid) {
            *pid = 0;
        }
        if (class_flags) {
            *class_flags = 0;
        }
        return false;
    }
    for (int i = 0; i < USB_MONITOR_MAX_DEVICES; ++i) {
        if (g_usb_monitor_devices[i].active) {
            if (first_index < 0) {
                first_index = i;
                first_device = g_usb_monitor_devices[i];
            }
            ++device_count;
        }
    }
    if (locked && g_usb_monitor_mutex != NULL) {
        xSemaphoreGive(g_usb_monitor_mutex);
    }

    if (count) {
        *count = device_count;
    }
    if (first_index < 0) {
        if (name && name_len > 0) {
            name[0] = '\0';
        }
        if (vid) {
            *vid = 0;
        }
        if (pid) {
            *pid = 0;
        }
        if (class_flags) {
            *class_flags = 0;
        }
        return false;
    }

    if (name && name_len > 0) {
        std::snprintf(name, name_len, "%s", first_device.name[0] ? first_device.name : "USB Device");
    }
    if (vid) {
        *vid = first_device.vid;
    }
    if (pid) {
        *pid = first_device.pid;
    }
    if (class_flags) {
        *class_flags = first_device.class_flags;
    }
    return true;
}

bool m5_corep4_tool::get_usb_other_device_info(char* name,
                                               size_t name_len,
                                               uint16_t* vid,
                                               uint16_t* pid,
                                               int* count) {
    int other_count = 0;
    int first_index = -1;
    usb_monitor_device_t first_device = {};
    const uint8_t known_flags = USB_MON_CLASS_VIDEO |
                                USB_MON_CLASS_MOUSE |
                                USB_MON_CLASS_KEYBOARD |
                                USB_MON_CLASS_HUB |
                                USB_MON_CLASS_HID;

    const bool locked = (g_usb_monitor_mutex == NULL) ||
                        (xSemaphoreTake(g_usb_monitor_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
    if (!locked) {
        if (count) {
            *count = 0;
        }
        if (name && name_len > 0) {
            name[0] = '\0';
        }
        if (vid) {
            *vid = 0;
        }
        if (pid) {
            *pid = 0;
        }
        return false;
    }
    for (int i = 0; i < USB_MONITOR_MAX_DEVICES; ++i) {
        if (g_usb_monitor_devices[i].active &&
            (g_usb_monitor_devices[i].class_flags & known_flags) == 0) {
            if (first_index < 0) {
                first_index = i;
                first_device = g_usb_monitor_devices[i];
            }
            ++other_count;
        }
    }
    if (locked && g_usb_monitor_mutex != NULL) {
        xSemaphoreGive(g_usb_monitor_mutex);
    }

    if (count) {
        *count = other_count;
    }
    if (first_index < 0) {
        if (name && name_len > 0) {
            name[0] = '\0';
        }
        if (vid) {
            *vid = 0;
        }
        if (pid) {
            *pid = 0;
        }
        return false;
    }

    if (name && name_len > 0) {
        std::snprintf(name, name_len, "%s", first_device.name[0] ? first_device.name : "Other USB");
    }
    if (vid) {
        *vid = first_device.vid;
    }
    if (pid) {
        *pid = first_device.pid;
    }
    return true;
}

// 初始化USB HID鼠标支持（包含任务创建）
esp_err_t m5_corep4_tool::init_usb_mouse_support() {
    ESP_LOGI(TAG, "Initializing USB HID Mouse support...");
    
    // 创建鼠标事件组
    if (mouse_event_group == nullptr) {
        mouse_event_group = xEventGroupCreate();
        if (mouse_event_group == nullptr) {
            ESP_LOGE(TAG, "Failed to create mouse event group");
            return ESP_FAIL;
        }
    }
    
    // 初始化USB HID鼠标
    esp_err_t mouse_init_ret = this->usb_hid_mouse_init();
    if (mouse_init_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize USB HID Mouse: %s", esp_err_to_name(mouse_init_ret));
        return mouse_init_ret;
    }
    
    ESP_LOGI(TAG, "USB HID Mouse initialized successfully");
    
    // 创建鼠标检查任务
    BaseType_t task_created = xTaskCreate(
        mouse_check_task,     // 任务函数
        "mouse_check",        // 任务名称
        4096,                 // 堆栈大小
        this,                 // 任务参数 - 传递this指针
        5,                    // 任务优先级
        NULL                  // 任务句柄
    );
    
    if (task_created != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create mouse check task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Mouse check task created successfully");
    return ESP_OK;
}

// 销毁LVGL鼠标支持
esp_err_t m5_corep4_tool::destroy_lvgl_mouse_support() {
    ESP_LOGI(TAG, "Destroying LVGL mouse support...");
    
    // 删除鼠标光标对象 - 使用异步删除避免在回调中删除的问题
    if (lvgl_mouse_cursor != nullptr) {
        ESP_LOGI(TAG, "Deleting LVGL mouse cursor...");
        lv_obj_delete_async(lvgl_mouse_cursor);  // 使用异步删除
        lvgl_mouse_cursor = nullptr;
        g_mouse_cursor = nullptr;
    }
    
    // 删除鼠标输入设备
    if (lvgl_mouse_indev != nullptr) {
        ESP_LOGI(TAG, "Deleting LVGL mouse input device...");
        lv_indev_delete(lvgl_mouse_indev);
        lvgl_mouse_indev = nullptr;
    }
    
    ESP_LOGI(TAG, "LVGL mouse support destroyed successfully");
    return ESP_OK;
}

// 发送鼠标连接事件
void m5_corep4_tool::notify_mouse_connected() {
    if (mouse_event_group) {
        xEventGroupSetBits(mouse_event_group, MOUSE_EVENT_CONNECTED);
        ESP_LOGI(TAG, "Mouse connected event sent");
    }
}

// 发送鼠标断开事件
void m5_corep4_tool::notify_mouse_disconnected() {
    if (mouse_event_group) {
        xEventGroupSetBits(mouse_event_group, MOUSE_EVENT_DISCONNECTED);
        ESP_LOGI(TAG, "Mouse disconnected event sent");
    }
}

// 鼠标检查任务
void m5_corep4_tool::mouse_check_task(void *pvParameters) {
    m5_corep4_tool *self = static_cast<m5_corep4_tool*>(pvParameters);
    static const char *task_tag = "mouse_check";
    static uint32_t last_mouse_check_time = 0;
    
    ESP_LOGI(task_tag, "Mouse check task started");
    
    while (true) {
        // 检查鼠标事件（连接/断开）
        if (self->mouse_event_group) {
            EventBits_t mouse_events = xEventGroupWaitBits(
                self->mouse_event_group,
                self->MOUSE_EVENT_CONNECTED | self->MOUSE_EVENT_DISCONNECTED,
                pdTRUE,  // 清除事件位
                pdFALSE, // 等待任一事件
                pdMS_TO_TICKS(100)  // 100ms超时
            );
            
            // 处理鼠标连接事件
            if (mouse_events & self->MOUSE_EVENT_CONNECTED) {
                ESP_LOGI(task_tag, "Processing mouse connected event...");
                if (self->get_lvgl_display() != nullptr) {
                    esp_err_t mouse_input_ret = self->create_lvgl_mouse_input_device();
                    if (mouse_input_ret == ESP_OK) {
                        ESP_LOGI(task_tag, "LVGL mouse input device created successfully after mouse connection");
                    } else {
                        ESP_LOGW(task_tag, "Failed to create LVGL mouse input device: %s", esp_err_to_name(mouse_input_ret));
                    }
                }
            }
            
            // 处理鼠标断开事件
            if (mouse_events & self->MOUSE_EVENT_DISCONNECTED) {
                ESP_LOGI(task_tag, "Processing mouse disconnected event...");
                if (self->get_lvgl_display() != nullptr) {
                    esp_err_t destroy_ret = self->destroy_lvgl_mouse_support();
                    if (destroy_ret == ESP_OK) {
                        ESP_LOGI(task_tag, "LVGL mouse support destroyed on disconnect");
                    } else {
                        ESP_LOGW(task_tag, "Failed to destroy LVGL mouse support: %s", esp_err_to_name(destroy_ret));
                    }
                }
            }
        }
        
        // 每500ms检查一次鼠标状态
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (current_time - last_mouse_check_time >= 500) {
            last_mouse_check_time = current_time;
            
            // 检查鼠标连接状态
            if (self->is_usb_mouse_connected()) {
                usb_mouse_data_t mouse_data;
                esp_err_t ret = self->get_usb_mouse_data(&mouse_data);
                if (ret == ESP_OK) {
                    // 只有当鼠标数据发生变化时才打印
                    static int16_t last_x = 0, last_y = 0;
                    static bool last_left = false, last_right = false, last_middle = false;
                    
                    if (mouse_data.x != last_x || mouse_data.y != last_y ||
                        mouse_data.left_button != last_left || 
                        mouse_data.right_button != last_right ||
                        mouse_data.middle_button != last_middle) {
                        
                        // ESP_LOGI(task_tag, "Mouse: X=%d, Y=%d, Left=%d, Right=%d, Middle=%d, Wheel=%d", 
                        //          mouse_data.x, mouse_data.y, 
                        //          mouse_data.left_button, mouse_data.right_button, 
                        //          mouse_data.middle_button, mouse_data.wheel);
                        
                        last_x = mouse_data.x;
                        last_y = mouse_data.y;
                        last_left = mouse_data.left_button;
                        last_right = mouse_data.right_button;
                        last_middle = mouse_data.middle_button;
                    }
                }
            } else {
                // 检查是否有鼠标连接，每10秒提示一次
                static uint32_t last_reminder_time = 0;
                if (current_time - last_reminder_time >= 10000) {
                    last_reminder_time = current_time;
                    ESP_LOGI(task_tag, "No USB mouse detected. Please connect a USB mouse to the USB2.0 port.");
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50)); // 每50ms检查一次，提高响应速度
    }
}

esp_err_t m5_corep4_tool::esp_hosted_manual_reset()
{
    ESP_LOGI(TAG, "Executing manual ESP-Hosted reset...");
    
    // 获取 ESP-Hosted 传输层的复位引脚配置
    gpio_pin_t reset_pin = { .port = NULL, .pin = 0 };
    esp_hosted_transport_err_t ret = esp_hosted_transport_get_reset_config(&reset_pin);
    
    if (ret != (esp_hosted_transport_err_t)ESP_OK) {
        ESP_LOGE(TAG, "Failed to get ESP-Hosted reset pin configuration: %d", ret);
        return ESP_FAIL;
    }
    
    // 验证引脚配置是否有效
    if (reset_pin.pin == -1 || reset_pin.pin == 0) {
        ESP_LOGE(TAG, "Invalid reset pin configuration: pin=%d", reset_pin.pin);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Reset pin configuration: port=%p, pin=%d", reset_pin.port, reset_pin.pin);
    
    // 配置 GPIO 为输出模式
    esp_err_t gpio_ret = gpio_reset_pin((gpio_num_t)reset_pin.pin);
    if (gpio_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset GPIO pin: %s", esp_err_to_name(gpio_ret));
        return ESP_FAIL;
    }
    
    gpio_ret = gpio_set_direction((gpio_num_t)reset_pin.pin, GPIO_MODE_OUTPUT);
    if (gpio_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set GPIO direction: %s", esp_err_to_name(gpio_ret));
        return ESP_FAIL;
    }
    
    // 1. 拉低复位引脚（激活复位，假设低电平有效）
    gpio_set_level((gpio_num_t)reset_pin.pin, 0);
    ESP_LOGI(TAG, "Reset: Step 1 - Set to LOW (activate reset)");
    vTaskDelay(pdMS_TO_TICKS(100)); // 保持复位信号 100ms
    
    // 2. 拉高复位引脚（释放复位，让设备开始运行）
    gpio_set_level((gpio_num_t)reset_pin.pin, 1);
    ESP_LOGI(TAG, "Reset: Step 2 - Set to HIGH (release reset, device running)");
    vTaskDelay(pdMS_TO_TICKS(1200)); // 等待设备完全启动（1200ms）
    
    ESP_LOGI(TAG, "ESP-Hosted manual reset completed successfully");
    return ESP_OK;
}

// RTC时间设置测试函数
// timestamp格式: YYYYMMDDHHmmss (例如: 20251118142600 表示 2025-11-18 14:26:00)
void m5_corep4_tool::test_rx8130_time_setting(rx8130_tools& rtc, uint64_t timestamp)
{
    // 读取当前时间
    struct tm current_time;
    rtc.getTime(&current_time);
    ESP_LOGI(TAG, "Current RTC time: %04d-%02d-%02d %02d:%02d:%02d (Day of week: %d)",
             current_time.tm_year + 1900,
             current_time.tm_mon + 1,
             current_time.tm_mday,
             current_time.tm_hour,
             current_time.tm_min,
             current_time.tm_sec,
             current_time.tm_wday);

    // 解析时间戳: YYYYMMDDHHmmss
    uint64_t temp = timestamp;
    int sec = temp % 100; temp /= 100;
    int min = temp % 100; temp /= 100;
    int hour = temp % 100; temp /= 100;
    int day = temp % 100; temp /= 100;
    int mon = temp % 100; temp /= 100;
    int year = temp;

    // 验证时间有效性
    if (year < 2000 || year > 2099 || mon < 1 || mon > 12 || 
        day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) {
        ESP_LOGE(TAG, "Invalid timestamp: %llu", timestamp);
        return;
    }

    // 设置新时间
    struct tm new_time = {};
    new_time.tm_year = year - 1900;  // Years since 1900
    new_time.tm_mon = mon - 1;       // Month (0-11)
    new_time.tm_mday = day;          // Day of month (1-31)
    new_time.tm_hour = hour;         // Hour (0-23)
    new_time.tm_min = min;           // Minute (0-59)
    new_time.tm_sec = sec;           // Second (0-59)
    
    // 使用 Sakamoto 算法计算星期几 (0=Sunday, 1=Monday, ..., 6=Saturday)
    int y_temp = year;
    int m_temp = mon;
    static int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    y_temp -= m_temp < 3;
    new_time.tm_wday = (y_temp + y_temp/4 - y_temp/100 + y_temp/400 + t[m_temp-1] + day) % 7;

    rtc.setTime(&new_time);
    ESP_LOGI(TAG, "RTC time set to: %04d-%02d-%02d %02d:%02d:%02d", 
             year, mon, day, hour, min, sec);

    // 验证设置的时间
    vTaskDelay(pdMS_TO_TICKS(5000)); // 等待5秒以确保时间设置生效
    struct tm verify_time;
    rtc.getTime(&verify_time);
    ESP_LOGI(TAG, "Verified RTC time: %04d-%02d-%02d %02d:%02d:%02d (Day of week: %d)",
             verify_time.tm_year + 1900,
             verify_time.tm_mon + 1,
             verify_time.tm_mday,
             verify_time.tm_hour,
             verify_time.tm_min,
             verify_time.tm_sec,
             verify_time.tm_wday);
}

// RX8130 定时唤醒测试
// 参数可以是秒数或时间戳:
// - 当 >= 19000000000000 时识别为时间戳 (格式: YYYYMMDDHHmmss)
// - 当 < 19000000000000 时识别为秒数
void m5_corep4_tool::test_rx8130_timed_wakeup(rx8130_tools& rtc, uint64_t wake_time)
{
    const uint64_t TIMESTAMP_THRESHOLD = 19000000000000ULL;
    uint32_t sleep_time_sec;
    
    // 智能识别参数类型
    if (wake_time >= TIMESTAMP_THRESHOLD) {
        // 识别为时间戳格式: YYYYMMDDHHmmss
        ESP_LOGI(TAG, "Detected timestamp format: %llu", wake_time);
        
        // 解析目标唤醒时间戳
        uint64_t temp = wake_time;
        int target_sec = temp % 100; temp /= 100;
        int target_min = temp % 100; temp /= 100;
        int target_hour = temp % 100; temp /= 100;
        int target_day = temp % 100; temp /= 100;
        int target_mon = temp % 100; temp /= 100;
        int target_year = temp;
        
        // 验证时间戳有效性
        if (target_year < 2000 || target_year > 2099 || target_mon < 1 || target_mon > 12 || 
            target_day < 1 || target_day > 31 || target_hour > 23 || target_min > 59 || target_sec > 59) {
            ESP_LOGE(TAG, "Invalid timestamp: %llu", wake_time);
            return;
        }
        
        // 获取当前RTC时间
        struct tm current_time;
        rtc.getTime(&current_time);
        
        // 转换为time_t进行时间计算
        time_t current_timestamp = mktime(&current_time);
        
        struct tm target_time = {};
        target_time.tm_year = target_year - 1900;
        target_time.tm_mon = target_mon - 1;
        target_time.tm_mday = target_day;
        target_time.tm_hour = target_hour;
        target_time.tm_min = target_min;
        target_time.tm_sec = target_sec;
        target_time.tm_wday = 0;
        target_time.tm_yday = 0;
        target_time.tm_isdst = -1;
        time_t target_timestamp = mktime(&target_time);
        
        // 计算时间差（秒数）
        int64_t time_diff = (int64_t)target_timestamp - (int64_t)current_timestamp;
        
        if (time_diff <= 0) {
            ESP_LOGE(TAG, "Target wake time is in the past or current time");
            ESP_LOGI(TAG, "Current: %04d-%02d-%02d %02d:%02d:%02d",
                     current_time.tm_year + 1900,
                     current_time.tm_mon + 1,
                     current_time.tm_mday,
                     current_time.tm_hour,
                     current_time.tm_min,
                     current_time.tm_sec);
            ESP_LOGI(TAG, "Target:  %04d-%02d-%02d %02d:%02d:%02d",
                     target_year, target_mon, target_day,
                     target_hour, target_min, target_sec);
            return;
        }
        
        sleep_time_sec = (uint32_t)time_diff;
        ESP_LOGI(TAG, "Wake time: %04d-%02d-%02d %02d:%02d:%02d (in %lu seconds)",
                 target_year, target_mon, target_day,
                 target_hour, target_min, target_sec,
                 sleep_time_sec);
    } else {
        // 识别为秒数
        sleep_time_sec = (uint32_t)wake_time;
        ESP_LOGI(TAG, "Detected seconds format: %lu seconds", sleep_time_sec);
        
        // 验证睡眠时间参数
        if (sleep_time_sec == 0) {
            ESP_LOGW(TAG, "Sleep time is 0, using default 30 seconds");
            sleep_time_sec = 30;
        } else if (sleep_time_sec > 86400) {  // 最大24小时
            ESP_LOGW(TAG, "Sleep time too long (%lu seconds), limiting to 24 hours", sleep_time_sec);
            sleep_time_sec = 86400;
        }
    }
    
    ESP_LOGI(TAG, "Setting up RX8130 wake test with %lu seconds sleep time", sleep_time_sec);
    
    // 清除之前的中断标志
    rtc.clearIrqFlags();
    
    // 设置定时器中断用于唤醒测试
    ESP_LOGI(TAG, "Setting %lu-second timer for wake-up test", sleep_time_sec);
    ESP_LOGI(TAG, "Please double-click the power button within %lu seconds to turn off the device", sleep_time_sec);
    ESP_LOGI(TAG, "The device will automatically wake up after %lu seconds", sleep_time_sec);
    
    rtc.setTimerIrq(sleep_time_sec);
    
    ESP_LOGI(TAG, "RX8130 wake test setup completed successfully");
}

// IR发送任务参数结构体
struct ir_send_task_param {
    ir_tools *ir;
    ir_format_t format;
};

// IR发送任务函数
static void ir_random_send_task(void *param)
{
    auto *task_param = static_cast<ir_send_task_param *>(param);
    ir_tools *ir = task_param->ir;
    ir_format_t format = task_param->format;
    
    constexpr TickType_t kSendInterval = pdMS_TO_TICKS(4000);
    constexpr ir_send_request_t nec_presets[] = {
        { .format = ir_format_t::NEC, .address = 0x00FF, .command = 0x01FE },
        { .format = ir_format_t::NEC, .address = 0x10EF, .command = 0x20DF },
        { .format = ir_format_t::NEC, .address = 0x18E7, .command = 0x58A7 },
    };
    constexpr ir_send_request_t rc5_presets[] = {
        { .format = ir_format_t::RC5, .address = 0x0015, .command = 0x000C },
        { .format = ir_format_t::RC5, .address = 0x0003, .command = 0x0012 },
        { .format = ir_format_t::RC5, .address = 0x0010, .command = 0x0005 },
    };

    while (true) {
        const uint32_t random_index = esp_random();
        ir_send_request_t request{};
        
        if (format == ir_format_t::NEC) {
            const size_t idx = random_index % (sizeof(nec_presets) / sizeof(nec_presets[0]));
            request = nec_presets[idx];
        } else if (format == ir_format_t::RC5) {
            const size_t idx = random_index % (sizeof(rc5_presets) / sizeof(rc5_presets[0]));
            request = rc5_presets[idx];
        } else {
            ESP_LOGW("IR_TX", "Unsupported IR format");
            vTaskDelay(kSendInterval);
            continue;
        }

        esp_err_t ret = ir ? ir->send(request) : ESP_ERR_INVALID_STATE;
        if (ret == ESP_OK) {
            ESP_LOGI("IR_TX", "Send [%s] addr=0x%04X cmd=0x%04X",
                     ir_format_name(request.format),
                     static_cast<unsigned>(request.address & 0xFFFFu),
                     static_cast<unsigned>(request.command & 0xFFFFu));
        } else {
            ESP_LOGW("IR_TX", "Failed to send %s frame: %s",
                     ir_format_name(request.format),
                     esp_err_to_name(ret));
        }

        vTaskDelay(kSendInterval);
    }
}

// 启动红外发送任务
esp_err_t m5_corep4_tool::start_ir_send_task(ir_format_t format)
{
    // 分配任务参数（使用堆内存以确保任务运行期间参数有效）
    static ir_send_task_param *task_param = nullptr;
    if (task_param == nullptr) {
        task_param = new ir_send_task_param{
            .ir = &otool_tools.ir,
            .format = format
        };
    } else {
        // 更新格式（如果需要重启任务）
        task_param->format = format;
    }
    
    BaseType_t ret = xTaskCreate(
        ir_random_send_task,
        "ir_send_task",
        4096,
        task_param,
        4,
        nullptr
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create IR send task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "IR send task started with format: %s", ir_format_name(format));
    return ESP_OK;
}

// 低功耗测试函数实现
void m5_corep4_tool::test_low_power_sleep(void)
{
    ESP_LOGI(TAG, "=== Starting Low Power Sleep Test ===");

    esp_err_t demo_stop_ret = stop_lvgl_custom_demo_task(1500);
    if (demo_stop_ret != ESP_OK) {
        ESP_LOGW(TAG, "LVGL custom demo task did not stop cleanly before low power test: %s", esp_err_to_name(demo_stop_ret));
    }

    // 低功耗测试路径可能绕过 sleep_before_work()，因此这里也必须收敛 SD。
    esp_err_t sd_sleep_ret = corep4_sd_card_deinit_for_sleep(otool_tools.sd_card);
    if (sd_sleep_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD prepare for low power test reported: %s", esp_err_to_name(sd_sleep_ret));
    }
    device_status.sdcard.sd_initialized = false;
    device_status.sdcard.sd_total_bytes = 0;
    device_status.sdcard.sd_free_bytes = 0;
    corep4_sd_detect_disable_for_sleep(pyb);

    // 1. 关闭BMI270 - 直接写寄存器进入休眠模式
    // PWR_CTRL寄存器(0x7D): bit2=ACC, bit1=GYR, bit0=AUX, 写0x00关闭所有传感器
    {
        i2c_bus_device_handle_t bmi270_dev = otool_tools.i2c_tools.get_i2c_device_handle(I2C_BMI270_ADDR);
        if (bmi270_dev != nullptr) {
            uint8_t pwr_ctrl_off = 0x00;  // 关闭ACC、GYR、AUX所有传感器
            i2c_bus_write_byte(bmi270_dev, 0x7D, pwr_ctrl_off);  // 写入PWR_CTRL寄存器
            ESP_LOGI(TAG, "BMI270 entered suspend mode (PWR_CTRL=0x00)");
        } else {
            ESP_LOGW(TAG, "BMI270 device not found, skipping...");
        }
    }

    // 2. 关闭RTC中断（RTC芯片本身无法关闭，但可以禁用其中断输出）
    {
        ESP_LOGI(TAG, "Initializing RTC and disabling IRQ...");
        auto rtc_device_handle = otool_tools.i2c_tools.get_i2c_device_handle(I2C_ADDR_RTC);
        bool ret = rtc.init(otool_tools.i2c_tools.get_i2c_bus_handle(), &rtc_device_handle, GPIO_NUM_NC);
        if (ret) {
            ESP_LOGI(TAG, "RTC initialized successfully");
            rtc.disableIrq();      // 禁用RTC中断输出
            ESP_LOGI(TAG, "RTC IRQ disabled");
            rtc.clearIrqFlags();   // 清除RTC中断标志
            ESP_LOGI(TAG, "RTC IRQ flags cleared");
        } else {
            ESP_LOGE(TAG, "Failed to initialize RTC");
        }
    }

    // 3. 先禁用LVGL触摸输入，防止后台任务访问休眠的触摸屏
    if (lvgl_touch_indev != nullptr) {
        ESP_LOGI(TAG, "Disabling LVGL touch input device...");
        if (lvgl_port_lock(1000)) {
            lv_indev_enable(lvgl_touch_indev, false);
            lvgl_port_unlock();
            ESP_LOGI(TAG, "LVGL touch input device disabled");
        } else {
            lv_indev_enable(lvgl_touch_indev, false);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 4. 初始化触摸屏并进入深度休眠
    // touch_init(); // 初始化触摸屏
    // touch_enter_sleep(); // 进入触摸屏休眠模式

    ESP_LOGI(TAG, "Power: PYB L3B -> OFF (low power test)");
    set_status(StatusParam::POWER_PYB_L3B, false, StatusConfigFlag::UPDATE_STATUS | StatusConfigFlag::APPLY_HARDWARE);

    //等待500ms以确保所有设备进入休眠
    vTaskDelay(pdMS_TO_TICKS(500));

    // 4. 配置PM1和PYB I2C休眠参数
    pm1.setI2cSleepTime(6);           // 设置PM1 I2C休眠时间为6s，降低功耗
    pyb.setI2cConfig(5, M5IOE1_I2C_SPEED_100K, M5IOE1_WAKE_EDGE_FALLING, M5IOE1_PULL_ENABLED); // 配置PYB I2C总线，频率400kHz
    ESP_LOGI(TAG, "PM1 and PYB I2C sleep parameters configured");

    // 测试路径也需要保持外设复位脚为低，否则深睡后 GPIO 输出驱动关闭会释放 PHY/C6 复位。
    corep4_gpio_output_low(Pin_P4_RMII_RST);
    corep4_gpio_output_low(Pin_P4_WIFI_RESET);
    corep4_hold_sleep_reset_pins();
    corep4_gpio_set_high_z(Pin_COREP4_INTERNAL_I2C_SCL);
    corep4_gpio_set_high_z(Pin_COREP4_INTERNAL_I2C_SDA);

    // 5. 进入ESP32深度休眠
    ESP_LOGI(TAG, "Entering ESP32 deep sleep...");
    esp_deep_sleep_start(); // 进入深度休眠（此函数不会返回）
}
