/**
 * @file PM1_FirmwareI2cBusUpgradeSerial.cpp
 * @brief PM1 firmware upgrade example - source: hardware UART (ESP-IDF)
 *
 * PC sends firmware over UART to the MCU; the MCU bridges it to PM1 over I2C.
 * PM1 stays an I2C slave on the MCU bus.
 *
 * Hardware connection:
 * - PC UART TX/RX -> ESP32 UART1 RX/TX (firmware input)
 * - ESP32 I2C SDA/SCL -> PM1 SDA/SCL
 */

#include "m5_py32bin_tools.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "serial_upgrade";

// UART input defaults (PC -> MCU)
static constexpr uart_port_t UART_PORT = UART_NUM_1;
static constexpr int UART_TX_PIN = GPIO_NUM_17;
static constexpr int UART_RX_PIN = GPIO_NUM_18;
static constexpr int UART_BAUD = 115200;

// I2C defaults (MCU -> PM1)
static constexpr uint8_t PM1_I2C_ADDRESS = 0x6E;
static constexpr uint32_t I2C_FREQUENCY_HZ = 100000;
static constexpr uint8_t PM1_HW_REG = 0x02;
static constexpr uint8_t PM1_SW_REG = 0x03;

using namespace m5::py32bin;
using namespace m5::py32bin::devices;

// UART read/write callbacks for ContextBusAdapter
// (Removed manual callbacks in favor of uartToI2cBridgeUpgrade)

void run_serial_upgrade(i2c_bus_handle_t i2c_bus, i2c_bus_device_handle_t pm1_dev) {
    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "Invalid I2C bus handle");
        return;
    }

    ESP_LOGI(TAG, "PM1 firmware upgrade (UART -> I2C bridge)");

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

    // === Configure UART Input ===
    Py32SerialConfig serial_cfg;
    serial_cfg.port = UART_PORT;
    serial_cfg.tx_pin = UART_TX_PIN;
    serial_cfg.rx_pin = UART_RX_PIN;
    serial_cfg.baudrate = UART_BAUD;
    // serial_cfg.hw_config is already set to defaults in constructor (8N1)

    // === Upgrade options ===
    Py32Pm1IapOptions iap_opts{};
    iap_opts.chunk_size = 128;
    iap_opts.chunk_delay_ms = 50;
    iap_opts.send_jump_command = true;

    Py32Pm1SerialInputOptions input_opts{};
    input_opts.max_input_size = 512 * 1024;
    input_opts.expect_md5_prefix = true;
    input_opts.per_byte_timeout_ms = 200;
    input_opts.idle_timeout_ms = 3000;
    input_opts.post_receive_delay_ms = 2000;

    ESP_LOGI(TAG, "Waiting for firmware over UART...");
    ESP_LOGI(TAG, "Expect: <32 hex MD5> + <newline> + <binary firmware>");

    // === Execute upgrade using helper ===
    esp_err_t ret = Py32Pm1Device::uartToI2cBusUpgrade(
        serial_cfg,
        i2c_bus,
        iap_opts,
        input_opts);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Upgrade success");
    } else {
        ESP_LOGE(TAG, "Upgrade failed: %s", esp_err_to_name(ret));
        ESP_LOGI(TAG, "Troubleshooting:");
        ESP_LOGI(TAG, "1) Check UART wiring/baud (115200)");
        ESP_LOGI(TAG, "2) Ensure PM1 is powered");
        ESP_LOGI(TAG, "3) Confirm I2C address (app 0x6E, bootloader 0x54)");
        ESP_LOGI(TAG, "4) Try lowering I2C frequency");
    }

    // === Cleanup ===
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
    run_serial_upgrade(i2c_bus, nullptr);
}
*/

