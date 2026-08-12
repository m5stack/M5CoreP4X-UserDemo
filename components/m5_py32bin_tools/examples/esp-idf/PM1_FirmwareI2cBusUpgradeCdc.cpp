/**
 * @file PM1_FirmwareI2cBusUpgradeCdc.cpp
 * @brief PM1 firmware upgrade example - source: USB-CDC (ESP-IDF)
 *
 * PM1 is an I2C slave device. This example receives firmware via USB Serial
 * JTAG from a PC, then upgrades PM1 over I2C.
 *
 * PC tool: python send_pm1_fw.py --port COMx --file firmware.bin
 *
 * Hardware connection:
 * - ESP32 USB -> PC (receive firmware data)
 * - ESP32 I2C SDA/SCL -> PM1 SDA/SCL
 */

#include "m5_py32bin_tools.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "cdc_upgrade";

using namespace m5::py32bin;
using namespace m5::py32bin::devices;

// I2C defaults (MCU -> PM1)
static constexpr uint8_t PM1_I2C_ADDRESS = 0x6E;
static constexpr uint32_t I2C_FREQUENCY_HZ = 100000;
static constexpr uint8_t PM1_HW_REG = 0x02;
static constexpr uint8_t PM1_SW_REG = 0x03;

// USB read/write callbacks
static esp_err_t usb_write(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (data == nullptr || len == 0) {
        return ESP_OK;
    }
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    size_t written = 0;
    while (written < len) {
        int ret = usb_serial_jtag_write_bytes((const char *)(data + written), len - written, pdMS_TO_TICKS(50));
        if (ret > 0) {
            written += ret;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if ((xTaskGetTickCount() - start) > timeout) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

static esp_err_t usb_read(uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (data == nullptr || len == 0) {
        return ESP_OK;
    }
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    size_t read_total = 0;
    while (read_total < len) {
        int ret = usb_serial_jtag_read_bytes((char *)(data + read_total), len - read_total, pdMS_TO_TICKS(50));
        if (ret > 0) {
            read_total += ret;
        }
        if ((xTaskGetTickCount() - start) > timeout) {
            break;
        }
    }
    return (read_total > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void run_cdc_upgrade(i2c_bus_handle_t i2c_bus, i2c_bus_device_handle_t pm1_dev) {
    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "Invalid I2C bus handle");
        return;
    }

    ESP_LOGI(TAG, "PM1 firmware upgrade (USB-CDC -> I2C)");

    // === Init USB Serial JTAG ===
    usb_serial_jtag_driver_config_t usb_cfg = {
        .rx_buffer_size = 4096,
        .tx_buffer_size = 4096,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    // === Configure ContextBusAdapter ===
    ContextBusAdapter user_input;
    Py32ContextBusConfig ctx_cfg{};
    ctx_cfg.write_func = usb_write;
    ctx_cfg.read_func = usb_read;
    ctx_cfg.default_timeout_ms = 1000;
    ESP_ERROR_CHECK(user_input.configure(ctx_cfg));
    ESP_ERROR_CHECK(user_input.begin());

    // === Optional: scan I2C for visibility ===
    uint8_t addrs[16] = {0};
    uint8_t count = i2c_bus_scan(i2c_bus, addrs, sizeof(addrs));
    if (count == 0) {
        ESP_LOGW(TAG, "No I2C devices found. Check wiring and power.");
    } else {
        ESP_LOGI(TAG, "I2C scan found %u device(s)", count);
        for (uint8_t i = 0; i < count; ++i) {
            uint8_t addr = addrs[i];
            if (addr == PM1_I2C_ADDRESS) {
                ESP_LOGI(TAG, "- 0x%02X (PM1 app)", addr);
            } else if (addr == 0x54) {
                ESP_LOGI(TAG, "- 0x54 (PM1 bootloader)");
            } else {
                ESP_LOGI(TAG, "- 0x%02X", addr);
            }
        }
    }

    // === Ensure PM1 device handle ===
    bool created_dev = false;
    if (pm1_dev == nullptr) {
        pm1_dev = i2c_bus_device_create(i2c_bus, PM1_I2C_ADDRESS, I2C_FREQUENCY_HZ);
        created_dev = (pm1_dev != nullptr);
        if (!pm1_dev) {
            ESP_LOGE(TAG, "Failed to create PM1 device handle (addr 0x%02X)", PM1_I2C_ADDRESS);
            user_input.end();
            return;
        }
    }

    // === Read PM1 version (optional) ===
    uint8_t hw = 0, sw = 0;
    if (i2c_bus_read_byte(pm1_dev, PM1_HW_REG, &hw) == ESP_OK &&
        i2c_bus_read_byte(pm1_dev, PM1_SW_REG, &sw) == ESP_OK) {
        ESP_LOGI(TAG, "PM1 detected @0x%02X (HW:0x%02X SW:0x%02X)", PM1_I2C_ADDRESS, hw, sw);
    } else {
        ESP_LOGW(TAG, "PM1 version read failed @0x%02X", PM1_I2C_ADDRESS);
    }

    // === Upgrade options ===
    Py32Pm1IapOptions iap_opts{};
    iap_opts.chunk_size = 128;
    iap_opts.chunk_delay_ms = 50;
    iap_opts.send_jump_command = true;

    Py32Pm1SerialInputOptions input_opts{};
    input_opts.max_input_size = 512 * 1024;       // allow larger images
    input_opts.expect_md5_prefix = true;
    input_opts.per_byte_timeout_ms = 200;
    input_opts.idle_timeout_ms = 3000;
    input_opts.post_receive_delay_ms = 2000;

    ESP_LOGI(TAG, "Waiting for firmware... Run: python send_pm1_fw.py --port COMx --file fw.bin");
    ESP_LOGI(TAG, "Expect: <32 hex MD5> + <newline> + <binary firmware>");

    // === Execute upgrade ===
    esp_err_t ret = Py32Pm1Device::contextBusToI2cBusUpgrade(
        user_input, i2c_bus, iap_opts, input_opts);

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

    // === Cleanup ===
    user_input.end();
    usb_serial_jtag_driver_uninstall();

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
    run_cdc_upgrade(i2c_bus, nullptr);
}
*/

