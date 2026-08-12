/**
 * @file IOE1_FirmwareUpgradeArray.ino
 * @brief IOE1 Firmware Upgrade Example - Source: Compiled Array (Arduino)
 * 
 * IOE1 is an I2C slave device. This example uses a firmware array compiled into the code
 * to upgrade IOE1 via I2C.
 * 
 * Hardware Connection:
 * - ESP32 I2C SDA (Default GPIO21) -> IOE1 SDA
 * - ESP32 I2C SCL (Default GPIO22) -> IOE1 SCL
 */

#include <m5_py32bin_tools.h>
#include <Wire.h>

using namespace m5::py32bin;
using namespace m5::py32bin::devices;

// ============ Firmware Data ============
// Include the firmware header file provided by upstream
// This file defines appHexData[] array and appHexLength
#include <devices/ioe1/app_without_flash_swa_0x6f_20251125.h>

// ============ I2C Configuration ============
#define IOE1_I2C_ADDRESS    0x6F    // IOE1 Application I2C Address
#define I2C_SDA_PIN         11      // Modify according to your hardware
#define I2C_SCL_PIN         9       // Modify according to your hardware
#define I2C_FREQUENCY       100000  // 100kHz

I2CBusAdapter ioe1I2c;

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("=====================================");
    Serial.println("IOE1 Firmware Upgrade - Source: Array");
    Serial.println("=====================================");
    Serial.printf("Firmware Size: %u bytes\n", appHexLength);
    Serial.println();

    // Initialize I2C
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(I2C_FREQUENCY);
    
    // Configure IOE1 I2C Adapter
    Py32I2CConfig i2c_cfg;
    i2c_cfg.address = IOE1_I2C_ADDRESS;
    i2c_cfg.wire = &Wire;
    i2c_cfg.frequency_hz = I2C_FREQUENCY;
    
    esp_err_t err = ioe1I2c.configure(i2c_cfg);
    if (err != ESP_OK) {
        Serial.printf("Error: I2C Config Failed (%s)\n", esp_err_to_name(err));
        return;
    }
    
    err = ioe1I2c.begin();
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
            if (address == IOE1_I2C_ADDRESS) {
                uint8_t uid_l = 0;
                uint8_t uid_h = 0;
                uint8_t rev = 0;
                if (ioe1I2c.readRegister(0x00, &uid_l, 1) == ESP_OK &&
                    ioe1I2c.readRegister(0x01, &uid_h, 1) == ESP_OK &&
                    ioe1I2c.readRegister(0x02, &rev, 1) == ESP_OK) {
                    uint16_t uid = static_cast<uint16_t>(uid_h << 8) | uid_l;
                    Serial.printf(" (IOE1 App, UID:0x%04X REV:0x%02X)", uid, rev);
                } else {
                    Serial.print(" (IOE1 App, UID/REV read failed)");
                }
            } else if (address == 0x5F) { // Bootloader default address
                Serial.print(" (IOE1 Bootloader)");
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
    Py32Ioe1FirmwareImage image;
    image.data = appHexData;
    image.length = appHexLength;
    image.base_address = 0x08001000;

    // Configure Upgrade Options
    Py32Ioe1IapOptions opts;
    opts.chunk_size = 128;           // Bytes per chunk
    opts.chunk_delay_ms = 50;        // Delay between chunks
    opts.send_jump_command = true;   // Jump to app after upgrade

    Serial.println("Upgrading IOE1...");
    Serial.println("Ensure IOE1 is connected to I2C bus.");
    Serial.println();

    // Execute Firmware Upgrade
    err = Py32Ioe1Device::i2cArrayUpgrade(ioe1I2c, image, opts);
    
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
        Serial.println("2. Check if IOE1 is powered on");
        Serial.println("3. Check I2C address (default 0x6F)");
        Serial.println("4. Try lowering I2C frequency");
    }

    ioe1I2c.end();
}

void loop() {
    delay(1000);
}
