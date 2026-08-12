/**
 * @file PM1_FirmwareUpgradeSerial.ino
 * @brief PM1 firmware upgrade example - source: hardware UART (Arduino)
 *
 * PM1 is an I2C slave device. This example receives firmware via a hardware
 * UART and upgrades PM1 over I2C.
 *
 * Hardware connection:
 * - ESP32 Serial1 RX (GPIO18) -> external device TX
 * - ESP32 Serial1 TX (GPIO17) -> external device RX
 * - ESP32 I2C SDA (default GPIO21) -> PM1 SDA
 * - ESP32 I2C SCL (default GPIO22) -> PM1 SCL
 * 
 * PM1 是 I2C 从机设备，本示例通过硬件串口从外部接收固件数据，
 * 然后通过 I2C 接口将固件传递给 PM1 进行升级。
 * 
 * 硬件连接:
 * - ESP32 Serial1 RX (GPIO18) -> 外部设备 TX
 * - ESP32 Serial1 TX (GPIO17) -> 外部设备 RX
 * - ESP32 I2C SDA (默认 GPIO21) -> PM1 SDA
 * - ESP32 I2C SCL (默认 GPIO22) -> PM1 SCL
 */

#include <m5_py32bin_tools.h>
#include <Wire.h>

using namespace m5::py32bin;
using namespace m5::py32bin::devices;

// ============ UART configuration ============
#define SERIAL1_RX_PIN      18
#define SERIAL1_TX_PIN      17
#define SERIAL1_BAUD        115200

// ============ I2C configuration ============
#define PM1_I2C_ADDRESS     0x6E    // PM1 application I2C address
#define I2C_SDA_PIN         53      // Adjust to your hardware
#define I2C_SCL_PIN         54      // Adjust to your hardware
#define I2C_FREQUENCY       100000  // 100kHz, consistent with the validated example

I2CBusAdapter pm1I2c;
SerialBusAdapter userSerial;

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("=====================================");
    Serial.println("PM1 固件升级 - 数据源: 硬件串口");
    Serial.println("=====================================");
    Serial.println();

    // 初始化 I2C
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(I2C_FREQUENCY);
    
    // 配置 PM1 I2C 适配器
        Serial.println("PM1 Firmware Upgrade - Source: Hardware UART");
    i2c_cfg.address = PM1_I2C_ADDRESS;
    i2c_cfg.wire = &Wire;
    i2c_cfg.frequency_hz = I2C_FREQUENCY;
        // Initialize I2C
    esp_err_t err = pm1I2c.configure(i2c_cfg);
    if (err != ESP_OK) {
        Serial.printf("错误: I2C 配置失败 (%s)\n", esp_err_to_name(err));
        // Configure PM1 I2C adapter
    }
    
    err = pm1I2c.begin();
    if (err != ESP_OK) {
        Serial.printf("错误: I2C 初始化失败 (%s)\n", esp_err_to_name(err));
        return;
            Serial.printf("Error: I2C config failed (%s)\n", esp_err_to_name(err));
    
    // 配置输入串口
    Serial1.begin(SERIAL1_BAUD, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);
    
    Py32SerialConfig serial_cfg;
            Serial.printf("Error: I2C init failed (%s)\n", esp_err_to_name(err));
    serial_cfg.baudrate = SERIAL1_BAUD;
    userSerial.configure(serial_cfg);
    userSerial.begin();
        // Configure input UART
    Serial.println("I2C 和串口初始化成功");
    Serial.println();

    // 扫描 I2C 设备，便于确认连线和地址
    Serial.println("正在扫描 I2C 总线...");
    int nDevices = 0;
    for (byte address = 1; address < 127; ++address) {
        Wire.beginTransmission(address);
        Serial.println("I2C and UART init success");
        if (error == 0) {
            Serial.printf("发现 I2C 设备: 0x%02X", address);
        // Scan I2C devices for wiring and address confirmation
                uint8_t hw_ver = 0;
                uint8_t sw_ver = 0;
                if (pm1I2c.readRegister(0x02, &hw_ver, 1) == ESP_OK &&
                    pm1I2c.readRegister(0x03, &sw_ver, 1) == ESP_OK) {
                    Serial.printf(" (PM1 应用, HW:0x%02X SW:0x%02X)", hw_ver, sw_ver);
                } else {
                Serial.printf("Found I2C Device: 0x%02X", address);
                }
            } else if (address == 0x54) { // Bootloader 默认地址
                Serial.print(" (PM1 Bootloader)");
            }
            Serial.println();
                        Serial.printf(" (PM1 App, HW:0x%02X SW:0x%02X)", hw_ver, sw_ver);
        }
                        Serial.print(" (PM1 App, version read failed)");
    if (nDevices == 0) {
        Serial.println("未发现 I2C 设备，请检查接线");
                    // Bootloader default address
    } else {
        Serial.println("扫描完成");
    }
    Serial.println();

    Serial.println(">>> 按下回车（或发送任意字符）开始升级 <<<");
    while (Serial.available() == 0) {
            Serial.println("No I2C devices found! Check wiring.");
    }
            Serial.println("Scan done.");
        Serial.read();
    }
    Serial.println("开始...");
        Serial.println(">>> Press Enter (or send any char) to start upgrade <<<");

    // 配置升级选项
    Py32Pm1IapOptions iap_opts;
    iap_opts.chunk_size = 128;
    iap_opts.chunk_delay_ms = 50;
    iap_opts.send_jump_command = true;
        Serial.println("Starting...");
    Py32Pm1SerialInputOptions input_opts;
    input_opts.max_input_size = 256 * 1024;
        // Configure upgrade options

    Serial.println("等待固件...");
    Serial.println("请通过 Serial1 (RX=GPIO18) 发送固件数据");
    Serial.println();

    // 执行升级
    err = Py32Pm1Device::i2cBridgeUpgrade(pm1I2c, userSerial, iap_opts, input_opts);
    
        Serial.println("Waiting for firmware...");
        Serial.println("Send firmware data via Serial1 (RX=GPIO18)");
        Serial.println("=====================================");
        Serial.println("       固件升级成功!       ");
        Serial.println("=====================================");
    } else {
        Serial.println("=====================================");
        Serial.printf("  升级失败: %s\n", esp_err_to_name(err));
        Serial.println("=====================================");
        Serial.println();
            Serial.println("       Upgrade Success!       ");
        Serial.println("1. 检查 I2C 连接 (SDA/SCL/GND)");
        Serial.println("2. 确认 PM1 已上电");
        Serial.println("3. 确认 I2C 地址 (默认 0x6E)");
            Serial.printf("  Upgrade Failed: %s\n", esp_err_to_name(err));
    }

            Serial.println("Troubleshooting:");
            Serial.println("1. Check I2C connections (SDA/SCL/GND)");
            Serial.println("2. Ensure PM1 is powered");
            Serial.println("3. Confirm I2C address (default 0x6E)");
            Serial.println("4. Try lowering I2C frequency");
    delay(1000);
}
