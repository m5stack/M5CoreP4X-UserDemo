/**
 * @file PM1_FirmwareI2cBusUpgradeArray.cpp
 * @brief PM1 固件升级示例 - 数据源: 编译时固件数组 (ESP-IDF)
 * 
 * PM1 是 I2C 从机设备，本示例使用编译在代码中的固件数组，
 * 通过 I2C 接口将固件传递给 PM1 进行升级。
 * 
 * 固件数据直接编译到程序中，无需外部输入。
 * 上游通常提供 app_without_flash*.h 格式的固件头文件。
 * 
 * 硬件连接:
 * - ESP32 I2C (SDA/SCL) -> PM1 I2C (将固件写入PM1)
 */

#include "m5_py32bin_tools.h"
#include "esp_log.h"
#include "esp_err.h"
#include "i2c_bus.h"

static const char *TAG = "array_upgrade";

// Firmware data compiled into the binary
// Upstream provides app_without_flash*.h that defines appHexData[] and appHexLength
#include "app_without_flash_pm1.h"

using namespace m5::py32bin;
using namespace m5::py32bin::devices;

// I2C defaults aligned with the validated Arduino example
static constexpr uint8_t PM1_I2C_ADDRESS = 0x6E;
static constexpr uint32_t I2C_FREQUENCY_HZ = 100000;
static constexpr uint8_t PM1_HW_REG = 0x02;
static constexpr uint8_t PM1_SW_REG = 0x03;

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

void run_array_upgrade(i2c_bus_handle_t i2c_bus, i2c_bus_device_handle_t pm1_dev) {
    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "Invalid I2C bus handle");
        return;
    }

    ESP_LOGI(TAG, "PM1 firmware upgrade (array source)");
    ESP_LOGI(TAG, "Firmware size: %u bytes", appHexLength);
    scan_i2c_bus(i2c_bus);

    bool created_dev = false;
    if (pm1_dev == nullptr) {
        pm1_dev = i2c_bus_device_create(i2c_bus, PM1_I2C_ADDRESS, I2C_FREQUENCY_HZ);
        created_dev = (pm1_dev != nullptr);
        if (!pm1_dev) {
            ESP_LOGE(TAG, "Failed to create PM1 device handle (addr 0x%02X)", PM1_I2C_ADDRESS);
            return;
        }
    }

    uint8_t hw = 0, sw = 0;
    if (i2c_bus_read_byte(pm1_dev, PM1_HW_REG, &hw) == ESP_OK &&
        i2c_bus_read_byte(pm1_dev, PM1_SW_REG, &sw) == ESP_OK) {
        ESP_LOGI(TAG, "PM1 detected @0x%02X (HW:0x%02X SW:0x%02X)", PM1_I2C_ADDRESS, hw, sw);
    } else {
        ESP_LOGW(TAG, "PM1 version read failed @0x%02X", PM1_I2C_ADDRESS);
    }

    Py32Pm1IapOptions opts{};
    opts.chunk_size = 128;
    opts.chunk_delay_ms = 50;  // match Arduino example pacing
    opts.send_jump_command = true;

    ESP_LOGI(TAG, "Upgrading PM1 via I2C (addr 0x%02X, %u Hz)...", PM1_I2C_ADDRESS, I2C_FREQUENCY_HZ);
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

    if (created_dev) {
        i2c_bus_device_delete(&pm1_dev);
    }
}

/*
// Example usage in app_main:
extern "C" void app_main(void) {
    // Initialize I2C bus
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_21,
        .scl_io_num = GPIO_NUM_22,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {
            .clk_speed = 100000,
        },
        .clk_flags = 0,
    };
    i2c_bus_handle_t i2c_bus = i2c_bus_create(I2C_NUM_0, &conf);

    // Run upgrade
    run_array_upgrade(i2c_bus, nullptr);
}
*/

