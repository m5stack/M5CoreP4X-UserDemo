#pragma once

#include "../base/device_base.h"

#if !defined(ARDUINO)
#include "i2c_bus.h"
#endif

namespace m5::py32bin::devices {

/**
 * @brief IOE1 固件镜像元数据（数组/二进制指针）
 */
struct Py32Ioe1FirmwareImage {
    const uint8_t *data = nullptr;      // 固件数据指针
    size_t length = 0;                  // 固件长度（字节）
    uint32_t base_address = 0x08001000; // 默认应用起始地址
};

/**
 * @brief IOE1 IAP 传输参数
 */
struct Py32Ioe1IapOptions {
    uint32_t chunk_size = 128;          // 单次写入块大小（字节），需 ≤255
    uint32_t chunk_delay_ms = 50;       // 分块写入间隔（ms）
    uint32_t transfer_timeout_ms = 1000;// 每次传输超时（ms）
    uint8_t write_opcode = 0x06;        // 写指令 opcode（PY32 IAP 协议）
    uint8_t jump_opcode = 0x77;         // 跳转 opcode（PY32 IAP 协议）
    bool send_jump_command = true;      // 升级后是否发送跳转指令
};

/**
 * @brief 串口/CDC 输入侧参数（用于 bridge 升级模式）
 */
struct Py32Ioe1SerialInputOptions {
    size_t max_input_size = 512 * 1024;       // 最大可接收固件大小
    uint32_t per_byte_timeout_ms = 200;       // 每字节超时（需考虑传输速率）
    uint32_t idle_timeout_ms = 3000;          // 数据流中断超时
    uint32_t post_receive_delay_ms = 2000;    // 接收完成后的稳定延迟
    bool expect_md5_prefix = true;            // 是否期望前缀 MD5 行
    bool md5_ascii = true;                    // MD5 是否为 ASCII(hex)
};

class Py32Ioe1Device : public DeviceBase {
public:
    const char *name() const override { return "py32_ioe1_dev"; }
    BusType preferredBus() const override { return BusType::I2C; }

    esp_err_t detect() override;
    esp_err_t configureDefaults() override;

    // GPIO 操作函数
    esp_err_t configureOutputs(uint8_t mask, bool as_output);
    esp_err_t writeOutputs(uint8_t mask, bool high);
    esp_err_t readInputs(uint8_t &state);

    // ========== 通用 I2C 升级函数 (Arduino & ESP-IDF 双平台) ==========

    /**
     * @brief 使用 I2CBusAdapter 直接升级固件（数组方式）
     *
     * 固件数据来源于内存数组，通过 I2CBusAdapter 写入 IOE1。
     * 适用于 Arduino 和 ESP-IDF 双平台。
     *
     * @param ioe1_adapter 已配置的 I2CBusAdapter（指向 IOE1 应用地址，如 0x4F 或 0x6F；IAP 固定为 0x5F）
     * @param image 固件镜像结构
     * @param options IAP 选项
     * @return esp_err_t
     */
    static esp_err_t i2cArrayUpgrade(I2CBusAdapter &ioe1_adapter,
                                     const Py32Ioe1FirmwareImage &image,
                                     const Py32Ioe1IapOptions &options = Py32Ioe1IapOptions{});

    /**
     * @brief 使用 I2CBusAdapter 进行交互式固件升级（从 BusAdapter 读取固件）
     *
     * 通过 user_input（串口/CDC/自定义数据源）接收固件数据，然后通过 I2C 写入 IOE1。
     * 适用于 Arduino 和 ESP-IDF 双平台。
     *
     * @param ioe1_adapter 已配置的 I2CBusAdapter（指向 IOE1 应用地址）
     * @param user_input 用户输入 BusAdapter
     * @param iap_options IAP 选项
     * @param input_options 串口输入选项
     * @return esp_err_t
     */
    static esp_err_t i2cBridgeUpgrade(I2CBusAdapter &ioe1_adapter,
                                      BusAdapter &user_input,
                                      const Py32Ioe1IapOptions &iap_options = Py32Ioe1IapOptions{},
                                      const Py32Ioe1SerialInputOptions &input_options = Py32Ioe1SerialInputOptions{});

#if !defined(ARDUINO)
    // ========== ESP-IDF 专用函数 (使用 i2c_bus API) ==========

    /**
     * @brief 使用 UART 串口作为输入源进行固件升级 (ESP-IDF 专用)
     *
     * @param uart_config UART 配置参数
     * @param bus 已初始化的 I2C 总线句柄
     * @param iap_options IAP 选项
     * @param input_options 串口输入选项
     * @return esp_err_t
     */
    static esp_err_t uartToI2cBusUpgrade(const Py32SerialConfig &uart_config,
                                          i2c_bus_handle_t bus,
                                          const Py32Ioe1IapOptions &iap_options = Py32Ioe1IapOptions{},
                                          const Py32Ioe1SerialInputOptions &input_options = Py32Ioe1SerialInputOptions{});

    /**
     * @brief 使用 ContextBusAdapter 进行固件升级 (ESP-IDF 专用)
     *
     * @param user_input 已初始化的用户输入 BusAdapter
     * @param bus 已初始化的 I2C 总线句柄
     * @param iap_options IAP 选项
     * @param input_options 串口输入选项
     * @return esp_err_t
     */
    static esp_err_t contextBusToI2cBusUpgrade(BusAdapter &user_input,
                                                i2c_bus_handle_t bus,
                                                const Py32Ioe1IapOptions &iap_options = Py32Ioe1IapOptions{},
                                                const Py32Ioe1SerialInputOptions &input_options = Py32Ioe1SerialInputOptions{});

    /**
     * @brief 直接使用内存固件数组升级 (ESP-IDF 专用)
     *
     * 最常用的升级接口：将 appHexData[] 直接通过 i2c_bus 写入 IOE1。
     * 函数内部会自动触发 bootloader、等待就绪、流式写入并跳转到应用。
     *
     * @param app_i2c_addr   IOE1 应用层 I2C 地址（如 0x4F 或 0x6F；IAP 固定为 0x5F）
     * @param firmware_data  固件二进制数据指针（appHexData）
     * @param firmware_length 固件长度（appHexLength）
     * @param bus            已初始化的 I2C 总线句柄
     * @param iap_options    IAP 选项
     * @return esp_err_t
     */
    static esp_err_t directArrayToI2cBusUpgrade(uint8_t app_i2c_addr,
                                                 const uint8_t *firmware_data,
                                                 size_t firmware_length,
                                                 i2c_bus_handle_t bus,
                                                 const Py32Ioe1IapOptions &iap_options = Py32Ioe1IapOptions{});
#endif  // !defined(ARDUINO)
};

}  // namespace m5::py32bin::devices
