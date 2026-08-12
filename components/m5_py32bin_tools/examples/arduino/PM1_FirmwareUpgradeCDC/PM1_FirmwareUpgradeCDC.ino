/**
 * @file PM1_FirmwareUpgradeCDC.ino
 * @brief PM1 firmware upgrade example - source: USB-CDC (Arduino)
 *
 * PM1 is an I2C slave device. This example receives firmware via USB-CDC
 * from a PC and upgrades PM1 over I2C.
 *
 * PC tool: python send_pm1_fw.py --port COMx --file firmware.bin
 *
 * Hardware connection:
 * - ESP32 USB -> PC (receive firmware data)
 * - ESP32 I2C SDA (default GPIO21) -> PM1 SDA
 * - ESP32 I2C SCL (default GPIO22) -> PM1 SCL
 */

#include <m5_py32bin_tools.h>
#include <Wire.h>

using namespace m5::py32bin;
using namespace m5::py32bin::devices;

// ============ I2C configuration ============
#define PM1_I2C_ADDRESS     0x6E    // PM1 application I2C address
#define I2C_SDA_PIN         53      // Adjust to your hardware
#define I2C_SCL_PIN         54      // Adjust to your hardware
#define I2C_FREQUENCY       100000  // 100kHz, consistent with the validated example

I2CBusAdapter pm1I2c;
ContextBusAdapter userInput;

// USB read/write callbacks
esp_err_t usbWrite(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    return (Serial.write(data, len) == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t usbRead(uint8_t *data, size_t len, uint32_t timeout_ms) {
    uint32_t start = millis();
    size_t count = 0;
    while (count < len && (millis() - start) < timeout_ms) {
        if (Serial.available()) {
            data[count++] = Serial.read();
        } 
    }
    return (count > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void setup() {
    Serial.setRxBufferSize(8192);  // Larger buffer for USB-CDC
    Serial.setTxBufferSize(1024);  // Smaller TX to reduce blocking
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("=====================================" );
    Serial.println("PM1 Firmware Upgrade - Source: USB-CDC");
    Serial.println("=====================================");
    Serial.println();

    // Initialize I2C
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(I2C_FREQUENCY);
    
    // Configure PM1 I2C adapter
    Py32I2CConfig i2c_cfg;
    i2c_cfg.address = PM1_I2C_ADDRESS;
    i2c_cfg.wire = &Wire;
    i2c_cfg.frequency_hz = I2C_FREQUENCY;
    
    esp_err_t err = pm1I2c.configure(i2c_cfg);
    if (err != ESP_OK) {
        Serial.printf("错误: I2C 配置失败 (%s)\n", esp_err_to_name(err));
        Serial.printf("Error: I2C config failed (%s)\n", esp_err_to_name(err));
    }
    
    err = pm1I2c.begin();
    if (err != ESP_OK) {
        Serial.printf("错误: I2C 初始化失败 (%s)\n", esp_err_to_name(err));
        Serial.printf("Error: I2C init failed (%s)\n", esp_err_to_name(err));
    }
    
    // 配置 USB 输入适配器
    // Configure USB input adapter
    Py32ContextBusConfig ctx_cfg;
    ctx_cfg.write_func = usbWrite;
    ctx_cfg.read_func = usbRead;
    userInput.configure(ctx_cfg);
    userInput.begin();
    
    Serial.println("I2C 初始化成功");
    Serial.println("I2C init success");

    // 扫描 I2C 设备，便于确认连线和地址
    // Scan I2C devices for wiring and address confirmation
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
            } else if (address == 0x54) { // Bootloader 默认地址
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

    Serial.println("Starting...");
    Serial.println();

    // Configure upgrade options
    Py32Pm1IapOptions iap_opts;
    iap_opts.chunk_size = 128;
    iap_opts.chunk_delay_ms = 50;
    iap_opts.send_jump_command = true;

    Py32Pm1SerialInputOptions input_opts;
    input_opts.max_input_size = 256 * 1024;
    input_opts.expect_md5_prefix = true;
    input_opts.idle_timeout_ms = 10000; // Increase timeout to handle PC script delays

    Serial.println("Waiting for firmware...");
    Serial.println("Run: python send_pm1_fw.py --port COMx --file fw.bin");
    Serial.println();

    // 执行升级
    err = Py32Pm1Device::i2cBridgeUpgrade(pm1I2c, userInput, iap_opts, input_opts);
    
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
        Serial.println("2. Ensure PM1 is powered");
        Serial.println("3. Confirm I2C address (default 0x6E)");
        Serial.println("4. Try lowering I2C frequency");
    }

    pm1I2c.end();
    userInput.end();
}

void loop() {
    delay(1000);
}
