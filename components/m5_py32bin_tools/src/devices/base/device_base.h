#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "esp_err.h"

#include "../../m5_py32bin_bus.h"

namespace m5::py32bin::devices {

class DeviceBase {
public:
    virtual ~DeviceBase() = default;

    virtual const char *name() const = 0;
    virtual BusType preferredBus() const { return BusType::Unknown; }

    virtual esp_err_t begin() {
        esp_err_t err = detect();
        if (err != ESP_OK) {
            return err;
        }
        return configureDefaults();
    }

    virtual esp_err_t detect() = 0;
    virtual esp_err_t configureDefaults() { return ESP_OK; }

    esp_err_t attachBus(std::shared_ptr<BusAdapter> adapter) {
        if (!adapter) {
            return ESP_ERR_INVALID_ARG;
        }
        bus_ = std::move(adapter);
        return ESP_OK;
    }

    std::shared_ptr<BusAdapter> bus() const { return bus_; }

protected:
    esp_err_t writeRegister(uint32_t reg, const uint8_t *data, size_t length, uint32_t timeout_ms = 1000) {
        if (!bus_) {
            return ESP_ERR_INVALID_STATE;
        }
        return bus_->writeRegister(reg, data, length, timeout_ms);
    }

    esp_err_t readRegister(uint32_t reg, uint8_t *data, size_t length, uint32_t timeout_ms = 1000) {
        if (!bus_) {
            return ESP_ERR_INVALID_STATE;
        }
        return bus_->readRegister(reg, data, length, timeout_ms);
    }

    template <typename T>
    esp_err_t writeValue(uint32_t reg, const T &value, uint32_t timeout_ms = 1000) {
        return writeRegister(reg, reinterpret_cast<const uint8_t *>(&value), sizeof(T), timeout_ms);
    }

    template <typename T>
    esp_err_t readValue(uint32_t reg, T &value, uint32_t timeout_ms = 1000) {
        return readRegister(reg, reinterpret_cast<uint8_t *>(&value), sizeof(T), timeout_ms);
    }

private:
    std::shared_ptr<BusAdapter> bus_;
};

}  // namespace m5::py32bin::devices
