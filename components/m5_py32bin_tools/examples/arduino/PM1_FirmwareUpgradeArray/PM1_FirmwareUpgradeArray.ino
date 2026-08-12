/**
 * @file PM1_FirmwareUpgradeArray.ino
 * @brief PM1 Firmware Upgrade Example - Source: Compiled Array (Arduino)
 * 
 * PM1 is an I2C slave device. This example uses a firmware array compiled into the code
 * to upgrade PM1 via I2C.
 * 
 * Hardware Connection:
 * - ESP32 I2C SDA (Default GPIO21) -> PM1 SDA
 * - ESP32 I2C SCL (Default GPIO22) -> PM1 SCL
 */

#include <m5_py32bin_tools.h>
#include <Wire.h>

using namespace m5::py32bin;
using namespace m5::py32bin::devices;

// ============ Firmware Data ============
// Include the firmware header file provided by upstream
// This file defines appHexData[] array and appHexLength
#include <devices/pm1/app_without_flash_hw5_sw6_20251209.h>

// ============ I2C Configuration ============
#define PM1_I2C_ADDRESS     0x6E    // PM1 Application I2C Address
#define I2C_SDA_PIN         53      // Modify according to your hardware
#define I2C_SCL_PIN         54      // Modify according to your hardware
#define I2C_FREQUENCY       100000  // 100kHz

I2CBusAdapter pm1I2c;

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("=====================================");
    Serial.println("PM1 Firmware Upgrade - Source: Array");
    Serial.println("=====================================");
    Serial.printf("Firmware Size: %u bytes\n", appHexLength);
    Serial.println();

    // Initialize I2C
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(I2C_FREQUENCY);
    
    // Configure PM1 I2C Adapter
    Py32I2CConfig i2c_cfg;
    i2c_cfg.address = PM1_I2C_ADDRESS;
    i2c_cfg.wire = &Wire;
    i2c_cfg.frequency_hz = I2C_FREQUENCY;
    
    esp_err_t err = pm1I2c.configure(i2c_cfg);
    if (err != ESP_OK) {
        Serial.printf("Error: I2C Config Failed (%s)\n", esp_err_to_name(err));
        return;
    }
    
    err = pm1I2c.begin();
    if (err != ESP_OK) {
        Serial.printf("Error: I2C Init Failed (%s)\n", esp_err_to_name(err));
        return;
    }
    
    Serial.println("I2C Init Success");
    Serial.println();

    // === Scan I2C Devices ===
    Serial.println("Scanning I2C bus...");
    int nDevices = 0;
    for (byte address = 1; address < 127; ++address) {
        Wire.beginTransmission(address);
        byte error = Wire.endTransmission();
        if (error == 0) {
            Serial.printf("Found I2C Device: 0x%02X", address);
            if (address == PM1_I2C_ADDRESS) {
                uint8_t hw_ver = 0;
                uint8_t sw_ver = 0;
                if (pm1I2c.readRegister(0x02, &hw_ver, 1) == ESP_OK &&
                    pm1I2c.readRegister(0x03, &sw_ver, 1) == ESP_OK) {
                    Serial.printf(" (PM1 App, HW:0x%02X SW:0x%02X)", hw_ver, sw_ver);
                } else {
                    Serial.print(" (PM1 App, version read failed)");
                }
            } else if (address == 0x54) { // Bootloader default address
                Serial.print(" (PM1 Bootloader)");
            }
            Serial.println();
            nDevices++;
        }
    }
    if (nDevices == 0) {
        Serial.println("No I2C devices found! Check wiring.");
    } else {
        Serial.println("Scan done.");
    }
    Serial.println();

    // === Wait for User Confirmation ===
    Serial.println(">>> Press Enter (or send any char) to start upgrade <<<");
    while (Serial.available() == 0) {
        delay(100);
    }
    // Clear input buffer
    while (Serial.available()) {
        Serial.read();
    }
    Serial.println("Starting...");
    Serial.println();

    // Prepare Firmware Image
    Py32Pm1FirmwareImage image;
    image.data = appHexData;
    image.length = appHexLength;
    image.base_address = 0x08001000;

    // Configure Upgrade Options
    Py32Pm1IapOptions opts;
    opts.chunk_size = 128;           // Bytes per chunk
    opts.chunk_delay_ms = 50;        // Delay between chunks
    opts.send_jump_command = true;   // Jump to app after upgrade

    Serial.println("Upgrading PM1...");
    Serial.println("Ensure PM1 is connected to I2C bus.");
    Serial.println();

    // Execute Firmware Upgrade
    err = Py32Pm1Device::i2cArrayUpgrade(pm1I2c, image, opts);
    
    Serial.println();
    if (err == ESP_OK) {
        Serial.println("=====================================");
        Serial.println("       Upgrade Success!       ");
        Serial.println("=====================================");
    } else {
        Serial.println("=====================================");
        Serial.printf("  Upgrade Failed: %s\n", esp_err_to_name(err));
        Serial.println("=====================================");
        Serial.println();
        Serial.println("Troubleshooting:");
        Serial.println("1. Check I2C connections (SDA/SCL/GND)");
        Serial.println("2. Check if PM1 is powered on");
        Serial.println("3. Check I2C address (default 0x6E)");
        Serial.println("4. Try lowering I2C frequency");
    }

    pm1I2c.end();
}

void loop() {
    delay(1000);
}
