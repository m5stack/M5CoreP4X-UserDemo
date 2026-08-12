#pragma once

#include "../base/device_base.h"

#if !defined(ARDUINO)
#include "i2c_bus.h"
#endif

namespace m5::py32bin::devices {

/**
 * @brief 固件镜像元数据（数组/二进制指针）
 */
struct Py32Pm1FirmwareImage {
    const uint8_t *data = nullptr;      // 固件数据指针
    size_t length = 0;                  // 固件长度（字节）
    uint32_t base_address = 0x08001000; // 默认应用起始地址
};

/**
 * @brief IAP 传输参数
 */
struct Py32Pm1IapOptions {
    uint32_t chunk_size = 128;          // 单次写入块大小
    uint32_t chunk_delay_ms = 50;       // 分块写入间隔
    uint32_t transfer_timeout_ms = 1000;// 每次传输超时
    uint8_t write_opcode = 0x06;        // 写指令 opcode
    uint8_t jump_opcode = 0x77;         // 跳转 opcode
    bool send_jump_command = true;      // 升级后是否发送跳转
};

/**
 * @brief 串口/CDC 输入侧参数
 */
struct Py32Pm1SerialInputOptions {
    size_t max_input_size = 512 * 1024;       // 最大可接收固件大小
    uint32_t per_byte_timeout_ms = 200;       // 每字节超时（需考虑传输速率）
    uint32_t idle_timeout_ms = 3000;          // 数据流中断超时
    uint32_t post_receive_delay_ms = 2000;    // 接收完成后的稳定延迟
    bool expect_md5_prefix = true;            // 是否期望前缀 MD5 行
    bool md5_ascii = true;                    // MD5 是否为 ASCII(hex)
};

class Py32Pm1Device : public DeviceBase {
public:
    const char *name() const override { return "py32_pm1_dev"; }
    BusType preferredBus() const override { return BusType::I2C; }

    esp_err_t detect() override;
    esp_err_t configureDefaults() override;

    // ========== 通用 I2C 升级函数 (Arduino & ESP-IDF 双平台) ==========
    
    /**
     * @brief 使用 I2CBusAdapter 直接升级固件（数组方式）
     * 
     * 通过 I2CBusAdapter 与 PM1 通信，适用于 Arduino 和 ESP-IDF 双平台。
     * 固件数据来源于内存中的数组。
     * 
     * @param pm1_adapter 已配置并初始化的 I2CBusAdapter（连接 PM1 应用地址 0x6E）
     * @param image 固件镜像结构
     * @param options IAP 选项
     * @return esp_err_t 
     */
    static esp_err_t i2cArrayUpgrade(I2CBusAdapter &pm1_adapter,
                                     const Py32Pm1FirmwareImage &image,
                                     const Py32Pm1IapOptions &options = Py32Pm1IapOptions{});

    /**
     * @brief 使用 I2CBusAdapter 进行交互式固件升级（从 BusAdapter 读取固件）
     * 
     * 通过 user_input（串口/CDC/自定义数据源）接收固件数据，然后通过 I2C 写入 PM1。
     * 适用于 Arduino 和 ESP-IDF 双平台。
     * 
     * @param pm1_adapter 已配置并初始化的 I2CBusAdapter（连接 PM1 应用地址 0x6E）
     * @param user_input 用户输入 BusAdapter（串口、CDC 或 ContextBusAdapter）
     * @param iap_options IAP 选项
     * @param input_options 串口输入选项
     * @return esp_err_t 
     */
    static esp_err_t i2cBridgeUpgrade(I2CBusAdapter &pm1_adapter,
                                      BusAdapter &user_input,
                                      const Py32Pm1IapOptions &iap_options = Py32Pm1IapOptions{},
                                      const Py32Pm1SerialInputOptions &input_options = Py32Pm1SerialInputOptions{});

#if !defined(ARDUINO)
    // ========== ESP-IDF 专用函数 (使用 i2c_bus API) ==========

    /**
     * @brief 使用 UART 串口作为输入源进行固件升级 (ESP-IDF 专用)
     * 
     * 通过硬件 UART 接收固件数据，然后通过 I2C 写入 PM1。
     * 内部会自动配置 UART 驱动。
     * 
     * @param uart_config UART 配置参数 (端口号、引脚、波特率等)
     * @param bus 已初始化的 I2C 总线句柄
     * @param iap_options IAP 选项
     * @param input_options 串口输入选项
     * @return esp_err_t 
     */
    static esp_err_t uartToI2cBusUpgrade(const Py32SerialConfig &uart_config,
                                            i2c_bus_handle_t bus,
                                            const Py32Pm1IapOptions &iap_options = Py32Pm1IapOptions{},
                                            const Py32Pm1SerialInputOptions &input_options = Py32Pm1SerialInputOptions{});

    /**
     * @brief 使用 ContextBusAdapter 进行固件升级 (ESP-IDF 专用)
     * 
     * 支持 USB Serial JTAG、CDC 或其他自定义数据源作为输入，通过 I2C 写入 PM1。
     * 
     * @param user_input 已初始化的用户输入 BusAdapter
     * @param bus 已初始化的 I2C 总线句柄
     * @param iap_options IAP 选项
     * @param input_options 串口输入选项
     * @return esp_err_t 
     */
    static esp_err_t contextBusToI2cBusUpgrade(BusAdapter &user_input,
                                                   i2c_bus_handle_t bus,
                                                   const Py32Pm1IapOptions &iap_options = Py32Pm1IapOptions{},
                                                   const Py32Pm1SerialInputOptions &input_options = Py32Pm1SerialInputOptions{});

    /**
     * @brief 直接使用内存中的固件数组进行升级 (ESP-IDF 专用)
     * 
     * 无需外部输入源，直接将内存中的二进制数据写入 PM1。
     * 
     * @param firmware_data 固件二进制数据指针
     * @param firmware_length 固件长度
     * @param bus 已初始化的 I2C 总线句柄
     * @param iap_options IAP 选项
     * @return esp_err_t 
     */
    static esp_err_t directArrayToI2cBusUpgrade(const uint8_t *firmware_data,
                                              size_t firmware_length,
                                              i2c_bus_handle_t bus,
                                              const Py32Pm1IapOptions &iap_options = Py32Pm1IapOptions{});
#endif  // !defined(ARDUINO)
};

}  // namespace m5::py32bin::devices
