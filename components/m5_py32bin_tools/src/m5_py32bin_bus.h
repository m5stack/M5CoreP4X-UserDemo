#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "esp_err.h"

#if defined(ARDUINO)
#include <Arduino.h>
#include <HardwareSerial.h>
#include <SPI.h>
#include <Wire.h>
#else
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#endif

namespace m5::py32bin {

constexpr uint32_t PY32_REGISTER_NONE = 0xFFFF'FFFFUL;

enum class BusType : uint8_t {
    Unknown = 0,
    Serial,
    I2C,
    SPI,
};

struct TransferTimeout {
    uint32_t write_ms = 1000;
    uint32_t read_ms = 1000;
};

class BusAdapter {
public:
    virtual ~BusAdapter() = default;

    virtual BusType type() const = 0;
    virtual esp_err_t begin() = 0;
    virtual void end() = 0;
    virtual bool ready() const = 0;

    virtual esp_err_t writeRegister(uint32_t reg, const uint8_t *data, size_t length, uint32_t timeout_ms = 1000) = 0;
    virtual esp_err_t readRegister(uint32_t reg, uint8_t *data, size_t length, uint32_t timeout_ms = 1000) = 0;

    virtual esp_err_t writeRaw(const uint8_t *data, size_t length, uint32_t timeout_ms = 1000) = 0;
    virtual esp_err_t readRaw(uint8_t *data, size_t length, uint32_t timeout_ms = 1000) = 0;
    virtual esp_err_t transferRaw(const uint8_t *tx_data, size_t tx_length, uint8_t *rx_data, size_t rx_length, uint32_t timeout_ms = 1000) = 0;
};

struct Py32I2CConfig {
    uint8_t address = 0x6E;
    uint32_t frequency_hz = 400000;
    int sda_pin = -1;
    int scl_pin = -1;
    bool pullup = true;
#if defined(ARDUINO)
    TwoWire *wire = &Wire;
#else
    i2c_port_t port = I2C_NUM_0;
    i2c_bus_handle_t bus = nullptr;
    i2c_bus_device_handle_t device = nullptr;
#endif
};

class I2CBusAdapter : public BusAdapter {
public:
    I2CBusAdapter();
    ~I2CBusAdapter() override;

    esp_err_t configure(const Py32I2CConfig &config);

    BusType type() const override { return BusType::I2C; }
    esp_err_t begin() override;
    void end() override;
    bool ready() const override { return ready_; }

    esp_err_t writeRegister(uint32_t reg, const uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;
    esp_err_t readRegister(uint32_t reg, uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;

    esp_err_t writeRaw(const uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;
    esp_err_t readRaw(uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;
    esp_err_t transferRaw(const uint8_t *tx_data, size_t tx_length, uint8_t *rx_data, size_t rx_length, uint32_t timeout_ms = 1000) override;

    // 获取当前配置（用于创建共享底层总线的新 adapter）
    const Py32I2CConfig &config() const { return config_; }

    // 创建一个共享底层总线但使用不同地址的新配置
    Py32I2CConfig createConfigWithAddress(uint8_t new_address) const {
        Py32I2CConfig new_config = config_;
        new_config.address = new_address;
#if !defined(ARDUINO)
        // ESP-IDF: 需要重新创建 device，但共享 bus
        new_config.device = nullptr;
#endif
        return new_config;
    }

private:
    Py32I2CConfig config_{};
    bool ready_ = false;
#ifndef ARDUINO
    bool owns_bus_ = false;
    bool owns_device_ = false;
#endif
};

struct Py32SPIConfig {
    bool cs_active_high = false;
    uint32_t clock_hz = 8'000'000;
    uint8_t command_length_bits = 8;
    uint8_t read_flag_mask = 0x80;
    uint8_t write_flag_mask = 0x00;
#if defined(ARDUINO)
    SPIClass *spi = &SPI;
    SPISettings settings;
    int cs_pin = -1;
#else
    spi_host_device_t host = SPI2_HOST;
    int mosi_pin = -1;
    int miso_pin = -1;
    int sclk_pin = -1;
    int cs_pin = -1;
    int dma_chan = SPI_DMA_CH_AUTO;
    bool auto_init_bus = false;
    spi_device_handle_t device = nullptr;
#endif
};

class SPIBusAdapter : public BusAdapter {
public:
    SPIBusAdapter();
    ~SPIBusAdapter() override;

    esp_err_t configure(const Py32SPIConfig &config);

    BusType type() const override { return BusType::SPI; }
    esp_err_t begin() override;
    void end() override;
    bool ready() const override { return ready_; }

    esp_err_t writeRegister(uint32_t reg, const uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;
    esp_err_t readRegister(uint32_t reg, uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;

    esp_err_t writeRaw(const uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;
    esp_err_t readRaw(uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;
    esp_err_t transferRaw(const uint8_t *tx_data, size_t tx_length, uint8_t *rx_data, size_t rx_length, uint32_t timeout_ms = 1000) override;

private:
    Py32SPIConfig config_{};
    bool ready_ = false;
#if !defined(ARDUINO)
    bool owns_bus_ = false;
    bool owns_device_ = false;
#endif
    esp_err_t assert_cs(bool active) const;
};

struct Py32SerialConfig {
    Py32SerialConfig();
    uint32_t baudrate = 115200;
    uint32_t timeout_ms = 1000;
#if defined(ARDUINO)
    HardwareSerial *serial = &Serial1;
    uint32_t serial_config = SERIAL_8N1;
#else
    uart_port_t port = UART_NUM_1;
    int tx_pin = -1;
    int rx_pin = -1;
    int rts_pin = UART_PIN_NO_CHANGE;
    int cts_pin = UART_PIN_NO_CHANGE;
    int event_queue_size = 10;
    size_t rx_buffer_size = 1024;
    size_t tx_buffer_size = 1024;
    uart_config_t hw_config{};
    QueueHandle_t event_queue = nullptr;
#endif
};

#if defined(ARDUINO)
inline Py32SerialConfig::Py32SerialConfig() = default;
#else
inline Py32SerialConfig::Py32SerialConfig() {
    hw_config.baud_rate = 115200;
    hw_config.data_bits = UART_DATA_8_BITS;
    hw_config.parity = UART_PARITY_DISABLE;
    hw_config.stop_bits = UART_STOP_BITS_1;
    hw_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    hw_config.rx_flow_ctrl_thresh = 0;
    hw_config.source_clk = UART_SCLK_DEFAULT;
}
#endif

class SerialBusAdapter : public BusAdapter {
public:
    SerialBusAdapter();
    ~SerialBusAdapter() override;

    esp_err_t configure(const Py32SerialConfig &config);

    BusType type() const override { return BusType::Serial; }
    esp_err_t begin() override;
    void end() override;
    bool ready() const override { return ready_; }

    esp_err_t writeRegister(uint32_t reg, const uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;
    esp_err_t readRegister(uint32_t reg, uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;

    esp_err_t writeRaw(const uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;
    esp_err_t readRaw(uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;
    esp_err_t transferRaw(const uint8_t *tx_data, size_t tx_length, uint8_t *rx_data, size_t rx_length, uint32_t timeout_ms = 1000) override;

private:
    Py32SerialConfig config_{};
    bool ready_ = false;
#if !defined(ARDUINO)
    bool driver_installed_ = false;
#endif
};

// Callback function types for ContextBusAdapter
using ContextBusWriteCallback = std::function<esp_err_t(const uint8_t *data, size_t length, uint32_t timeout_ms)>;
using ContextBusReadCallback = std::function<esp_err_t(uint8_t *data, size_t length, uint32_t timeout_ms)>;
using ContextBusAvailableCallback = std::function<size_t()>;

struct Py32ContextBusConfig {
    ContextBusWriteCallback write_func = nullptr;
    ContextBusReadCallback read_func = nullptr;
    ContextBusAvailableCallback available_func = nullptr;
    uint32_t default_timeout_ms = 1000;
};

class ContextBusAdapter : public BusAdapter {
public:
    ContextBusAdapter();
    ~ContextBusAdapter() override;

    esp_err_t configure(const Py32ContextBusConfig &config);

    BusType type() const override { return BusType::Serial; }
    esp_err_t begin() override;
    void end() override;
    bool ready() const override { return ready_; }

    esp_err_t writeRegister(uint32_t reg, const uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;
    esp_err_t readRegister(uint32_t reg, uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;

    esp_err_t writeRaw(const uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;
    esp_err_t readRaw(uint8_t *data, size_t length, uint32_t timeout_ms = 1000) override;
    esp_err_t transferRaw(const uint8_t *tx_data, size_t tx_length, uint8_t *rx_data, size_t rx_length, uint32_t timeout_ms = 1000) override;

private:
    Py32ContextBusConfig config_{};
    bool ready_ = false;
};

}  // namespace m5::py32bin
