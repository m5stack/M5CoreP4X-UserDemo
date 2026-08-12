/**
 * @file PM1_FirmwareI2cMasterUpgradeArray.cpp
 * @brief PM1 firmware upgrade example - source: compiled array via I2CBusAdapter (ESP-IDF)
 *
 * Uses I2CBusAdapter directly (no i2c_bus helper calls in user code) to
 * push a compiled-in firmware image to PM1 over I2C.
 *
 * Firmware data is compiled into the binary; upstream typically provides
 * app_without_flash*.h that declares appHexData[] and appHexLength.
 *
 * Hardware connection:
 * - ESP32 I2C SDA/SCL -> PM1 SDA/SCL (PM1 as I2C slave at 0x6E)
 */

#include "m5_py32bin_tools.h"

#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"

// Firmware array compiled into the image
#include "app_without_flash_pm1.h"

using namespace m5::py32bin;
using namespace m5::py32bin::devices;

static const char *TAG = "pm1_i2c_array";

// I2C defaults aligned with other examples
static constexpr uint8_t PM1_I2C_ADDRESS = 0x6E;
static constexpr uint32_t I2C_FREQUENCY_HZ = 100000;
static constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_21;
static constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_22;
static constexpr uint8_t PM1_HW_REG = 0x02;
static constexpr uint8_t PM1_SW_REG = 0x03;

static void log_pm1_version(I2CBusAdapter &adapter) {
    uint8_t hw = 0, sw = 0;
    if (adapter.readRegister(PM1_HW_REG, &hw, 1) == ESP_OK &&
        adapter.readRegister(PM1_SW_REG, &sw, 1) == ESP_OK) {
        ESP_LOGI(TAG, "PM1 detected @0x%02X (HW:0x%02X SW:0x%02X)", PM1_I2C_ADDRESS, hw, sw);
    } else {
        ESP_LOGW(TAG, "PM1 version read failed @0x%02X", PM1_I2C_ADDRESS);
    }
}

void run_i2c_master_array_upgrade() {
    ESP_LOGI(TAG, "PM1 firmware upgrade (I2C master, array source)");
    ESP_LOGI(TAG, "Firmware size: %u bytes", static_cast<unsigned>(appHexLength));

    // Configure I2C adapter (creates bus/device internally on begin)
    Py32I2CConfig i2c_cfg{};
    i2c_cfg.address = PM1_I2C_ADDRESS;
    i2c_cfg.frequency_hz = I2C_FREQUENCY_HZ;
    i2c_cfg.sda_pin = I2C_SDA_PIN;
    i2c_cfg.scl_pin = I2C_SCL_PIN;
    i2c_cfg.pullup = true;
    i2c_cfg.port = I2C_NUM_0;

    I2CBusAdapter pm1_adapter;
    ESP_ERROR_CHECK(pm1_adapter.configure(i2c_cfg));
    ESP_ERROR_CHECK(pm1_adapter.begin());

    log_pm1_version(pm1_adapter);

    Py32Pm1FirmwareImage image{};
    image.data = appHexData;
    image.length = appHexLength;
    image.base_address = 0x08001000;

    Py32Pm1IapOptions opts{};
    opts.chunk_size = 128;
    opts.chunk_delay_ms = 50;
    opts.send_jump_command = true;

    ESP_LOGI(TAG, "Upgrading PM1 via I2C (addr 0x%02X, %u Hz)...", PM1_I2C_ADDRESS, I2C_FREQUENCY_HZ);
    esp_err_t ret = Py32Pm1Device::i2cArrayUpgrade(pm1_adapter, image, opts);

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

    pm1_adapter.end();
}

/*
// Example usage in app_main:
extern "C" void app_main(void) {
    // Run upgrade (will init I2C bus internally)
    run_i2c_master_array_upgrade();
}
*/
