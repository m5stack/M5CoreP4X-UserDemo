/**
 * @file main.cpp
 * @brief PM1 固件升级主程序 - ESP32-C61
 * 
 * PM1 是 I2C 从机设备，本程序使用编译在代码中的固件数组，
 * 通过 I2C 接口将固件传递给 PM1 进行升级。
 * 
 * 硬件连接:
 * - ESP32-C61 I2C (SDA/SCL) -> PM1 I2C (将固件写入PM1)
 * - ESP32-C61 USB CDC 用于按键触发升级
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "i2c_bus.h"
#include "m5_py32bin_tools.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"

// 固件数据直接引用组件中的最新版本
// #include "devices/pm1/app_without_flash_hw5_sw5_20251104.h"
#include "devices/pm1/app_without_flash_hw5_sw6_20251209.h"

using namespace m5::py32bin;
using namespace m5::py32bin::devices;

static const char *TAG = "main";

// ESP32-C61 I2C GPIO 配置
// 注意: 请根据实际硬件连接修改这些引脚
static constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_0;
static constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_1;
static constexpr uint32_t I2C_FREQ_HZ = 100000;

// I2C 设备地址和寄存器定义
static constexpr uint8_t PM1_I2C_ADDRESS = 0x6E;
static constexpr uint8_t PM1_HW_REG = 0x02;
static constexpr uint8_t PM1_SW_REG = 0x03;

/**
 * @brief 扫描I2C总线上的设备
 */
static void scan_i2c_bus(i2c_bus_handle_t i2c_bus) {
    uint8_t found_addrs[16] = {0};
    uint8_t count = i2c_bus_scan(i2c_bus, found_addrs, sizeof(found_addrs));
    if (count == 0) {
        ESP_LOGW(TAG, "No I2C devices found. Check wiring and power.");
        return;
    }

    ESP_LOGI(TAG, "I2C scan found %u device(s):", count);
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t addr = found_addrs[i];
        if (addr == PM1_I2C_ADDRESS) {
            ESP_LOGI(TAG, "- 0x%02X (PM1 app address)", addr);
        } else if (addr == 0x54) {
            ESP_LOGI(TAG, "- 0x54 (PM1 bootloader address)");
        } else {
            ESP_LOGI(TAG, "- 0x%02X", addr);
        }
    }
}

/**
 * @brief 执行PM1固件升级
 */
static void run_pm1_upgrade(i2c_bus_handle_t i2c_bus) {
    ESP_LOGI(TAG, "PM1 firmware upgrade (array source)");
    ESP_LOGI(TAG, "Firmware size: %u bytes", appHexLength);
    scan_i2c_bus(i2c_bus);

    // 创建PM1设备句柄
    i2c_bus_device_handle_t pm1_dev = i2c_bus_device_create(i2c_bus, PM1_I2C_ADDRESS, I2C_FREQ_HZ);
    if (!pm1_dev) {
        ESP_LOGE(TAG, "Failed to create PM1 device handle (addr 0x%02X)", PM1_I2C_ADDRESS);
        return;
    }

    // 读取PM1版本信息
    uint8_t hw = 0, sw = 0;
    if (i2c_bus_read_byte(pm1_dev, PM1_HW_REG, &hw) == ESP_OK &&
        i2c_bus_read_byte(pm1_dev, PM1_SW_REG, &sw) == ESP_OK) {
        ESP_LOGI(TAG, "PM1 detected @0x%02X (HW:0x%02X SW:0x%02X)", PM1_I2C_ADDRESS, hw, sw);
    } else {
        ESP_LOGW(TAG, "PM1 version read failed @0x%02X", PM1_I2C_ADDRESS);
    }

    // 等待USB CDC输入任意数据后开始烧录
    ESP_LOGI(TAG, "Press any key to start firmware upgrade...");
    uint8_t buf[1];
    while (usb_serial_jtag_read_bytes(buf, 1, portMAX_DELAY) == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "Starting upgrade...");

    // 配置升级参数
    Py32Pm1IapOptions opts{};
    opts.chunk_size = 128;
    opts.chunk_delay_ms = 50;
    opts.send_jump_command = true;

    // 执行固件升级
    ESP_LOGI(TAG, "Upgrading PM1 via I2C (addr 0x%02X, %u Hz)...", PM1_I2C_ADDRESS, I2C_FREQ_HZ);
    esp_err_t ret = Py32Pm1Device::directArrayToI2cBusUpgrade(appHexData, appHexLength, i2c_bus, opts);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Upgrade success");
    } else {
        ESP_LOGE(TAG, "Upgrade failed: %s", esp_err_to_name(ret));
        ESP_LOGI(TAG, "Troubleshooting:");
        ESP_LOGI(TAG, "1) Check I2C wiring (SDA/SCL/GND)");
        ESP_LOGI(TAG, "2) Ensure PM1 is powered");
        ESP_LOGI(TAG, "3) Confirm I2C address (app 0x6E, bootloader 0x54)");
        ESP_LOGI(TAG, "4) Try lowering I2C frequency");
    }

    i2c_bus_device_delete(&pm1_dev);
}

extern "C" void app_main(void)
{
    // 初始化USB Serial JTAG (CDC)
    usb_serial_jtag_driver_config_t usb_serial_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_serial_config));
    
    ESP_LOGI(TAG, "PM1 Firmware Upgrade - ESP32-C61");
    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());
    
    // 初始化I2C总线
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_SDA_PIN;
    conf.scl_io_num = I2C_SCL_PIN;
    conf.sda_pullup_en = true;
    conf.scl_pullup_en = true;
    conf.master.clk_speed = I2C_FREQ_HZ;
    conf.clk_flags = 0;
    
    ESP_LOGI(TAG, "Initializing I2C bus (SDA=%d, SCL=%d, Freq=%lu Hz)",
             I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
    
    i2c_bus_handle_t i2c_bus = i2c_bus_create(I2C_NUM_0, &conf);
    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "Failed to create I2C bus");
        return;
    }
    
    ESP_LOGI(TAG, "I2C bus initialized successfully");
    
    // 延时确保设备稳定
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // 执行固件升级
    run_pm1_upgrade(i2c_bus);
    
    // 清理I2C总线
    i2c_bus_delete(&i2c_bus);
    
    ESP_LOGI(TAG, "Program complete");
}
