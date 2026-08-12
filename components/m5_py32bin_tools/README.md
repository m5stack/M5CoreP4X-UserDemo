# M5 Py32Bin Tools Component

[English](README_EN.md) | [中文](README.md)

这是一个用于管理和升级 M5Stack PM1 (PY32) 固件的 ESP-IDF 组件和 Arduino 库。它提供了通过 I2C 接口进行固件升级、版本检查和设备信息读取的功能。

## 功能特性

*   **固件升级**: 支持通过 I2C 总线升级 PY32 芯片固件。
*   **多种输入源**: 支持从内存数组、串口 (YModem/Raw) 或其他自定义流升级。
*   **多平台支持**: 兼容 ESP-IDF (原生 I2C 或 i2c_bus 组件) 和 Arduino 开发环境。
*   **GUI 工具**: 提供基于 PyQt6 的图形化工具，用于生成固件头文件。

## 目录结构

```text
m5_py32bin_tools/
├── examples/
│   ├── arduino/
│   │   ├── PM1_FirmwareUpgradeArray/       # 从固件数组升级 (Arduino)
│   │   ├── PM1_FirmwareUpgradeCDC/         # 通过 USB CDC 接收固件并升级
│   │   └── PM1_FirmwareUpgradeSerial/      # 通过串口接收固件并升级
│   └── esp-idf/
│       ├── PM1_FirmwareI2cBusUpgradeArray.cpp  # 使用 i2c_bus 组件从数组升级
│       ├── PM1_FirmwareI2cBusUpgradeCdc.cpp    # 使用 i2c_bus + USB CDC 升级
│       ├── PM1_FirmwareI2cBusUpgradeSerial.cpp # 使用 i2c_bus + 串口升级
│       └── PM1_FirmwareI2cMasterUpgradeArray.cpp # 使用 I2CBusAdapter 从数组升级
├── gui/
│   └── m5_py32bin_tools_gui.py             # PyQt6 固件打包工具
├── src/
│   ├── devices/pm1/
│   │   ├── py32_pm1_dev.h                  # PM1 设备定义和 API
│   │   └── app_without_flash_*.h           # 预编译固件数组
│   ├── m5_py32bin_bus.h                    # 总线适配器 (I2CBusAdapter 等)
│   └── m5_py32bin_tools.h                  # 主头文件
├── CMakeLists.txt                          # ESP-IDF 构建脚本
├── library.properties                      # Arduino 库描述文件
└── README.md                               # 说明文档
```

## 安装

### ESP-IDF

将此组件克隆到你的项目的 `components` 目录下：

```bash
cd your_project/components
git clone https://github.com/m5stack/m5_py32bin_tools.git
```

### Arduino

1.  下载本仓库的 ZIP 压缩包。
2.  在 Arduino IDE 中，选择 `Sketch` -> `Include Library` -> `Add .ZIP Library...`。
3.  选择下载的 ZIP 文件。

## 使用方法 (ESP-IDF)

### 1. 配置项目

在 `CMakeLists.txt` 中添加组件依赖：

```cmake
idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES m5_py32bin_tools i2c_bus)
```

### 2. 代码示例

#### 方式 A: 使用 I2CBusAdapter 从数组升级

参考 `examples/esp-idf/PM1_FirmwareI2cMasterUpgradeArray.cpp`

```cpp
#include "m5_py32bin_tools.h"
#include "driver/i2c.h"
#include "esp_log.h"

// 上游提供的固件头文件，定义了 appHexData[] 和 appHexLength
#include "devices/pm1/app_without_flash_hw5_sw6_20251209.h"

using namespace m5::py32bin;
using namespace m5::py32bin::devices;

void run_upgrade() {
    // 1. 配置 I2C 适配器
    Py32I2CConfig i2c_cfg{};
    i2c_cfg.address = 0x6E;           // PM1 应用地址
    i2c_cfg.frequency_hz = 100000;    // 100kHz
    i2c_cfg.sda_pin = GPIO_NUM_21;
    i2c_cfg.scl_pin = GPIO_NUM_22;
    i2c_cfg.pullup = true;
    i2c_cfg.port = I2C_NUM_0;

    I2CBusAdapter pm1_adapter;
    ESP_ERROR_CHECK(pm1_adapter.configure(i2c_cfg));
    ESP_ERROR_CHECK(pm1_adapter.begin());

    // 2. 准备固件镜像
    Py32Pm1FirmwareImage image{};
    image.data = appHexData;
    image.length = appHexLength;
    image.base_address = 0x08001000;

    // 3. 配置 IAP 选项
    Py32Pm1IapOptions opts{};
    opts.chunk_size = 128;
    opts.chunk_delay_ms = 50;
    opts.send_jump_command = true;

    // 4. 执行升级
    esp_err_t ret = Py32Pm1Device::i2cArrayUpgrade(pm1_adapter, image, opts);
    
    if (ret == ESP_OK) {
        ESP_LOGI("upgrade", "Success!");
    } else {
        ESP_LOGE("upgrade", "Failed: %s", esp_err_to_name(ret));
    }

    pm1_adapter.end();
}
```

#### 方式 B: 使用 `i2c_bus` 组件从数组升级

参考 `examples/esp-idf/PM1_FirmwareI2cBusUpgradeArray.cpp`

```cpp
#include "m5_py32bin_tools.h"
#include "i2c_bus.h"

#include "devices/pm1/app_without_flash_hw5_sw6_20251209.h"

using namespace m5::py32bin::devices;

void run_upgrade(i2c_bus_handle_t i2c_bus) {
    Py32Pm1IapOptions opts{};
    opts.chunk_size = 128;
    opts.chunk_delay_ms = 50;
    opts.send_jump_command = true;

    esp_err_t ret = Py32Pm1Device::directArrayToI2cBusUpgrade(
        appHexData, appHexLength, i2c_bus, opts);
    
    if (ret == ESP_OK) {
        ESP_LOGI("upgrade", "Success!");
    }
}
```

## 使用方法 (Arduino)

参考 `examples/arduino/PM1_FirmwareUpgradeArray/PM1_FirmwareUpgradeArray.ino`

```cpp
#include <m5_py32bin_tools.h>
#include <Wire.h>

// 上游提供的固件头文件
#include <devices/pm1/app_without_flash_hw5_sw6_20251209.h>

using namespace m5::py32bin;
using namespace m5::py32bin::devices;

#define PM1_I2C_ADDRESS     0x6E
#define I2C_SDA_PIN         21
#define I2C_SCL_PIN         22
#define I2C_FREQUENCY       100000

I2CBusAdapter pm1I2c;

void setup() {
    Serial.begin(115200);
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(I2C_FREQUENCY);
    
    // 配置 PM1 I2C 适配器
    Py32I2CConfig i2c_cfg;
    i2c_cfg.address = PM1_I2C_ADDRESS;
    i2c_cfg.wire = &Wire;
    i2c_cfg.frequency_hz = I2C_FREQUENCY;
    
    pm1I2c.configure(i2c_cfg);
    pm1I2c.begin();
    
    // 读取当前版本
    uint8_t hw_ver = 0, sw_ver = 0;
    pm1I2c.readRegister(0x02, &hw_ver, 1);
    pm1I2c.readRegister(0x03, &sw_ver, 1);
    Serial.printf("PM1 HW:0x%02X SW:0x%02X\n", hw_ver, sw_ver);

    // 准备固件镜像
    Py32Pm1FirmwareImage image{};
    image.data = appHexData;
    image.length = appHexLength;
    image.base_address = 0x08001000;

    // 配置 IAP 选项
    Py32Pm1IapOptions opts{};
    opts.chunk_size = 128;
    opts.chunk_delay_ms = 50;
    opts.send_jump_command = true;

    // 执行升级
    Serial.println("Starting upgrade...");
    esp_err_t ret = Py32Pm1Device::i2cArrayUpgrade(pm1I2c, image, opts);
    
    if (ret == ESP_OK) {
        Serial.println("Upgrade Success!");
    } else {
        Serial.printf("Upgrade Failed: %s\n", esp_err_to_name(ret));
    }
}

void loop() {}
```

## 工具 (PyQt6)

位于 `gui/m5_py32bin_tools_gui.py` 的工具可以帮助你将 `.bin` 固件文件转换为 C 数组头文件，方便嵌入到代码中。

### 依赖安装

```bash
pip install PyQt6 esptool
```

### 运行

```bash
cd gui
python m5_py32bin_tools_gui.py
```

### 打包为可执行文件

```bash
cd gui
.\builder.ps1   # Windows PowerShell
```

## 许可证

MIT
