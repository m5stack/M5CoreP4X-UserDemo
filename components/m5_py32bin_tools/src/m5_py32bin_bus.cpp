#include "m5_py32bin_bus.h"

#include <algorithm>
#include <vector>

#include "esp_log.h"

namespace m5::py32bin {

namespace {
constexpr char TAG[] = "py32bus";
}

I2CBusAdapter::I2CBusAdapter() {}
I2CBusAdapter::~I2CBusAdapter() { end(); }

esp_err_t I2CBusAdapter::configure(const Py32I2CConfig &config) {
    config_ = config;
    return ESP_OK;
}

esp_err_t I2CBusAdapter::begin() {
#if defined(ARDUINO)
    if (config_.wire == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // 设置足够大的 I2C 缓冲区以支持 IAP 数据帧 (8 字节头 + 128 字节数据)
    config_.wire->setBufferSize(256);
    if (config_.sda_pin >= 0 && config_.scl_pin >= 0) {
        config_.wire->begin(config_.sda_pin, config_.scl_pin);
    } else {
        config_.wire->begin();
    }
    config_.wire->setClock(config_.frequency_hz);
    ready_ = true;
    return ESP_OK;
#else
    if (config_.bus == nullptr) {
        i2c_config_t hw_cfg = {};
        hw_cfg.mode = I2C_MODE_MASTER;
        hw_cfg.sda_io_num = config_.sda_pin;
        hw_cfg.scl_io_num = config_.scl_pin;
        hw_cfg.sda_pullup_en = config_.pullup;
        hw_cfg.scl_pullup_en = config_.pullup;
        hw_cfg.master.clk_speed = config_.frequency_hz;
        config_.bus = i2c_bus_create(config_.port, &hw_cfg);
        if (config_.bus == nullptr) {
            ESP_LOGE(TAG, "Failed to create I2C bus");
            return ESP_FAIL;
        }
        owns_bus_ = true;
    }
    if (config_.device == nullptr) {
        config_.device = i2c_bus_device_create(config_.bus, config_.address, config_.frequency_hz);
        if (config_.device == nullptr) {
            ESP_LOGE(TAG, "Failed to create I2C device");
            return ESP_FAIL;
        }
        owns_device_ = true;
    }
    ready_ = true;
    return ESP_OK;
#endif
}

void I2CBusAdapter::end() {
#if defined(ARDUINO)
    ready_ = false;
#else
    if (owns_device_ && config_.device) {
        i2c_bus_device_delete(&config_.device);
        config_.device = nullptr;
        owns_device_ = false;
    }
    if (owns_bus_ && config_.bus) {
        i2c_bus_delete(&config_.bus);
        config_.bus = nullptr;
        owns_bus_ = false;
    }
    ready_ = false;
#endif
}

esp_err_t I2CBusAdapter::writeRegister(uint32_t reg, const uint8_t *data, size_t length, uint32_t) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(ARDUINO)
    config_.wire->beginTransmission(config_.address);
    if (reg != PY32_REGISTER_NONE) {
        if (reg > 0xFF) {
            config_.wire->write(static_cast<uint8_t>((reg >> 8) & 0xFF));
        }
        config_.wire->write(static_cast<uint8_t>(reg & 0xFF));
    }
    if (length > 0 && data != nullptr) {
        config_.wire->write(data, length);
    }
    return config_.wire->endTransmission(true) == 0 ? ESP_OK : ESP_FAIL;
#else
    if (config_.device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (reg == PY32_REGISTER_NONE) {
        return i2c_bus_write_bytes(config_.device, NULL_I2C_MEM_ADDR, length, data);
    }
    if (reg <= 0xFF) {
        return i2c_bus_write_bytes(config_.device, static_cast<uint8_t>(reg & 0xFF), length, data);
    }
    return i2c_bus_write_reg16(config_.device, static_cast<uint16_t>(reg), length, data);
#endif
}

esp_err_t I2CBusAdapter::readRegister(uint32_t reg, uint8_t *data, size_t length, uint32_t) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(ARDUINO)
    if (reg != PY32_REGISTER_NONE) {
        config_.wire->beginTransmission(config_.address);
        if (reg > 0xFF) {
            config_.wire->write(static_cast<uint8_t>((reg >> 8) & 0xFF));
        }
        config_.wire->write(static_cast<uint8_t>(reg & 0xFF));
        if (config_.wire->endTransmission(false) != 0) {
            return ESP_FAIL;
        }
    }
    size_t read_len = config_.wire->requestFrom(config_.address, static_cast<uint8_t>(length));
    if (read_len != length) {
        return ESP_FAIL;
    }
    for (size_t i = 0; i < length; ++i) {
        data[i] = config_.wire->read();
    }
    return ESP_OK;
#else
    if (config_.device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (reg == PY32_REGISTER_NONE) {
        return i2c_bus_read_bytes(config_.device, NULL_I2C_MEM_ADDR, length, data);
    }
    if (reg <= 0xFF) {
        return i2c_bus_read_bytes(config_.device, static_cast<uint8_t>(reg & 0xFF), length, data);
    }
    return i2c_bus_read_reg16(config_.device, static_cast<uint16_t>(reg), length, data);
#endif
}

esp_err_t I2CBusAdapter::writeRaw(const uint8_t *data, size_t length, uint32_t) {
    return writeRegister(PY32_REGISTER_NONE, data, length);
}

esp_err_t I2CBusAdapter::readRaw(uint8_t *data, size_t length, uint32_t) {
    return readRegister(PY32_REGISTER_NONE, data, length);
}

esp_err_t I2CBusAdapter::transferRaw(const uint8_t *tx_data, size_t tx_length, uint8_t *rx_data, size_t rx_length, uint32_t) {
    esp_err_t err = ESP_OK;
    if (tx_length > 0 && tx_data != nullptr) {
        err = writeRaw(tx_data, tx_length);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (rx_length > 0 && rx_data != nullptr) {
        err = readRaw(rx_data, rx_length);
    }
    return err;
}

SPIBusAdapter::SPIBusAdapter() = default;
SPIBusAdapter::~SPIBusAdapter() { end(); }

esp_err_t SPIBusAdapter::configure(const Py32SPIConfig &config) {
    config_ = config;
#if defined(ARDUINO)
    config_.settings = SPISettings(config_.clock_hz, MSBFIRST, SPI_MODE0);
#endif
    return ESP_OK;
}

esp_err_t SPIBusAdapter::begin() {
#if defined(ARDUINO)
    if (config_.spi == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    config_.spi->begin();
    if (config_.cs_pin >= 0) {
        pinMode(config_.cs_pin, OUTPUT);
        assert_cs(false);
    }
    ready_ = true;
    return ESP_OK;
#else
    if (config_.device == nullptr) {
        if (!config_.auto_init_bus) {
            ESP_LOGE(TAG, "SPI device handle missing");
            return ESP_ERR_INVALID_STATE;
        }
        spi_bus_config_t bus_cfg = {};
        bus_cfg.mosi_io_num = config_.mosi_pin;
        bus_cfg.miso_io_num = config_.miso_pin;
        bus_cfg.sclk_io_num = config_.sclk_pin;
        bus_cfg.quadwp_io_num = -1;
        bus_cfg.quadhd_io_num = -1;
        bus_cfg.max_transfer_sz = 4096;
        bus_cfg.flags = 0;
        bus_cfg.intr_flags = 0;
        esp_err_t err = spi_bus_initialize(config_.host, &bus_cfg, config_.dma_chan);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi bus init failed: %s", esp_err_to_name(err));
            return err;
        }
        owns_bus_ = true;
        spi_device_interface_config_t dev_cfg = {};
        dev_cfg.command_bits = config_.command_length_bits;
        dev_cfg.address_bits = 0;
        dev_cfg.dummy_bits = 0;
        dev_cfg.mode = 0;
        dev_cfg.clock_speed_hz = static_cast<int>(config_.clock_hz);
        dev_cfg.spics_io_num = config_.cs_pin;
        dev_cfg.queue_size = 2;
        dev_cfg.flags = 0;
        dev_cfg.pre_cb = nullptr;
        dev_cfg.post_cb = nullptr;
        err = spi_bus_add_device(config_.host, &dev_cfg, &config_.device);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi add dev failed: %s", esp_err_to_name(err));
            return err;
        }
        owns_device_ = true;
    }
    ready_ = true;
    return ESP_OK;
#endif
}

void SPIBusAdapter::end() {
#if defined(ARDUINO)
    ready_ = false;
#else
    if (owns_device_ && config_.device) {
        spi_bus_remove_device(config_.device);
        config_.device = nullptr;
        owns_device_ = false;
    }
    if (owns_bus_) {
        spi_bus_free(config_.host);
        owns_bus_ = false;
    }
    ready_ = false;
#endif
}

esp_err_t SPIBusAdapter::assert_cs(bool active) const {
#if defined(ARDUINO)
    if (config_.cs_pin < 0) {
        return ESP_OK;
    }
    digitalWrite(config_.cs_pin, (active ^ config_.cs_active_high) ? LOW : HIGH);
#else
    // Hardware CS handled by driver when using esp-idf
    (void)active;
#endif
    return ESP_OK;
}

esp_err_t SPIBusAdapter::writeRegister(uint32_t reg, const uint8_t *data, size_t length, uint32_t timeout_ms) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    std::vector<uint8_t> frame;
    if (reg != PY32_REGISTER_NONE) {
        frame.push_back(static_cast<uint8_t>((reg & 0xFF) | config_.write_flag_mask));
    }
    if (length > 0 && data != nullptr) {
        frame.insert(frame.end(), data, data + length);
    }
    if (frame.empty()) {
        return ESP_OK;
    }
    return transferRaw(frame.data(), frame.size(), nullptr, 0, timeout_ms);
}

esp_err_t SPIBusAdapter::readRegister(uint32_t reg, uint8_t *data, size_t length, uint32_t timeout_ms) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    std::vector<uint8_t> tx(length + (reg == PY32_REGISTER_NONE ? 0 : 1), 0x00);
    if (reg != PY32_REGISTER_NONE) {
        tx[0] = static_cast<uint8_t>((reg & 0xFF) | config_.read_flag_mask);
    }
    std::vector<uint8_t> rx(tx.size(), 0x00);
    esp_err_t err = transferRaw(tx.data(), tx.size(), rx.data(), rx.size(), timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    if (length > 0 && data != nullptr) {
        size_t offset = (reg == PY32_REGISTER_NONE) ? 0 : 1;
        if (offset + length > rx.size()) {
            return ESP_ERR_INVALID_SIZE;
        }
        std::copy(rx.begin() + offset, rx.begin() + offset + length, data);
    }
    return ESP_OK;
}

esp_err_t SPIBusAdapter::writeRaw(const uint8_t *data, size_t length, uint32_t timeout_ms) {
    return transferRaw(data, length, nullptr, 0, timeout_ms);
}

esp_err_t SPIBusAdapter::readRaw(uint8_t *data, size_t length, uint32_t timeout_ms) {
    std::vector<uint8_t> zeros(length, 0x00);
    return transferRaw(zeros.data(), zeros.size(), data, length, timeout_ms);
}

esp_err_t SPIBusAdapter::transferRaw(const uint8_t *tx_data, size_t tx_length, uint8_t *rx_data, size_t rx_length, uint32_t) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(ARDUINO)
    if (config_.spi == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    config_.spi->beginTransaction(config_.settings);
    assert_cs(true);
    for (size_t i = 0; i < std::max(tx_length, rx_length); ++i) {
        uint8_t tx = 0x00;
        if (tx_data && i < tx_length) {
            tx = tx_data[i];
        }
        uint8_t received = config_.spi->transfer(tx);
        if (rx_data && i < rx_length) {
            rx_data[i] = received;
        }
    }
    assert_cs(false);
    config_.spi->endTransaction();
    return ESP_OK;
#else
    if (config_.device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    spi_transaction_t trans = {};
    trans.length = tx_length * 8;
    trans.rxlength = rx_length * 8;
    trans.tx_buffer = tx_data;
    trans.rx_buffer = rx_data;
    return spi_device_transmit(config_.device, &trans);
#endif
}

SerialBusAdapter::SerialBusAdapter() = default;
SerialBusAdapter::~SerialBusAdapter() { end(); }

esp_err_t SerialBusAdapter::configure(const Py32SerialConfig &config) {
    config_ = config;
    return ESP_OK;
}

esp_err_t SerialBusAdapter::begin() {
#if defined(ARDUINO)
    if (config_.serial == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    config_.serial->begin(config_.baudrate, config_.serial_config);
    ready_ = true;
    return ESP_OK;
#else
    if (config_.port >= UART_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = uart_driver_install(config_.port, config_.rx_buffer_size, config_.tx_buffer_size, config_.event_queue_size, &config_.event_queue, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (err == ESP_OK) {
        driver_installed_ = true;
    }
    err = uart_param_config(config_.port, &config_.hw_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = uart_set_pin(config_.port, config_.tx_pin, config_.rx_pin, config_.rts_pin, config_.cts_pin);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    ready_ = true;
    return ESP_OK;
#endif
}

void SerialBusAdapter::end() {
#if defined(ARDUINO)
    ready_ = false;
#else
    if (driver_installed_) {
        uart_driver_delete(config_.port);
        driver_installed_ = false;
    }
    ready_ = false;
#endif
}

esp_err_t SerialBusAdapter::writeRegister(uint32_t reg, const uint8_t *data, size_t length, uint32_t timeout_ms) {
    std::vector<uint8_t> frame;
    if (reg != PY32_REGISTER_NONE) {
        frame.push_back(static_cast<uint8_t>(reg & 0xFF));
    }
    if (length > 0 && data != nullptr) {
        frame.insert(frame.end(), data, data + length);
    }
    return writeRaw(frame.data(), frame.size(), timeout_ms);
}

esp_err_t SerialBusAdapter::readRegister(uint32_t reg, uint8_t *data, size_t length, uint32_t timeout_ms) {
    if (reg != PY32_REGISTER_NONE) {
        esp_err_t err = writeRegister(reg, nullptr, 0, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
    }
    return readRaw(data, length, timeout_ms);
}

esp_err_t SerialBusAdapter::writeRaw(const uint8_t *data, size_t length, uint32_t) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == nullptr || length == 0) {
        return ESP_OK;
    }
#if defined(ARDUINO)
    size_t written = config_.serial->write(data, length);
    return written == length ? ESP_OK : ESP_FAIL;
#else
    int sent = uart_write_bytes(config_.port, data, length);
    return sent == static_cast<int>(length) ? ESP_OK : ESP_FAIL;
#endif
}

esp_err_t SerialBusAdapter::readRaw(uint8_t *data, size_t length, uint32_t timeout_ms) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == nullptr || length == 0) {
        return ESP_OK;
    }
#if defined(ARDUINO)
    uint32_t start = millis();
    size_t read_count = 0;
    while (read_count < length) {
        if (config_.serial->available()) {
            data[read_count++] = static_cast<uint8_t>(config_.serial->read());
        } else if (millis() - start > timeout_ms) {
            break;
        }
    }
    return read_count == length ? ESP_OK : ESP_ERR_TIMEOUT;
#else
    int read_bytes = uart_read_bytes(config_.port, data, length, pdMS_TO_TICKS(timeout_ms));
    return read_bytes == static_cast<int>(length) ? ESP_OK : ESP_ERR_TIMEOUT;
#endif
}

esp_err_t SerialBusAdapter::transferRaw(const uint8_t *tx_data, size_t tx_length, uint8_t *rx_data, size_t rx_length, uint32_t timeout_ms) {
    esp_err_t err = ESP_OK;
    if (tx_data && tx_length > 0) {
        err = writeRaw(tx_data, tx_length, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (rx_data && rx_length > 0) {
        err = readRaw(rx_data, rx_length, timeout_ms);
    }
    return err;
}

ContextBusAdapter::ContextBusAdapter() = default;
ContextBusAdapter::~ContextBusAdapter() { end(); }

esp_err_t ContextBusAdapter::configure(const Py32ContextBusConfig &config) {
    if (config.write_func == nullptr || config.read_func == nullptr) {
        ESP_LOGE(TAG, "write_func and read_func must not be null");
        return ESP_ERR_INVALID_ARG;
    }
    config_ = config;
    return ESP_OK;
}

esp_err_t ContextBusAdapter::begin() {
    if (config_.write_func == nullptr || config_.read_func == nullptr) {
        ESP_LOGE(TAG, "Bridge not properly configured");
        return ESP_ERR_INVALID_STATE;
    }
    ready_ = true;
    return ESP_OK;
}

void ContextBusAdapter::end() {
    ready_ = false;
}

esp_err_t ContextBusAdapter::writeRegister(uint32_t reg, const uint8_t *data, size_t length, uint32_t timeout_ms) {
    std::vector<uint8_t> frame;
    if (reg != PY32_REGISTER_NONE) {
        frame.push_back(static_cast<uint8_t>(reg & 0xFF));
    }
    if (length > 0 && data != nullptr) {
        frame.insert(frame.end(), data, data + length);
    }
    return writeRaw(frame.data(), frame.size(), timeout_ms);
}

esp_err_t ContextBusAdapter::readRegister(uint32_t reg, uint8_t *data, size_t length, uint32_t timeout_ms) {
    if (reg != PY32_REGISTER_NONE) {
        esp_err_t err = writeRegister(reg, nullptr, 0, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
    }
    return readRaw(data, length, timeout_ms);
}

esp_err_t ContextBusAdapter::writeRaw(const uint8_t *data, size_t length, uint32_t timeout_ms) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == nullptr || length == 0) {
        return ESP_OK;
    }
    if (config_.write_func == nullptr) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return config_.write_func(data, length, timeout_ms);
}

esp_err_t ContextBusAdapter::readRaw(uint8_t *data, size_t length, uint32_t timeout_ms) {
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == nullptr || length == 0) {
        return ESP_OK;
    }
    if (config_.read_func == nullptr) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return config_.read_func(data, length, timeout_ms);
}

esp_err_t ContextBusAdapter::transferRaw(const uint8_t *tx_data, size_t tx_length, uint8_t *rx_data, size_t rx_length, uint32_t timeout_ms) {
    esp_err_t err = ESP_OK;
    if (tx_data && tx_length > 0) {
        err = writeRaw(tx_data, tx_length, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (rx_data && rx_length > 0) {
        err = readRaw(rx_data, rx_length, timeout_ms);
    }
    return err;
}

}  // namespace m5::py32bin
