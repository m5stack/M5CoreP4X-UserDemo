/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology Co., Ltd
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __LSM6DS3TRC_TOOLS_H__
#define __LSM6DS3TRC_TOOLS_H__

#include "i2c_bus.h"

#include "esp_err.h"

#include <cstdint>

inline constexpr uint8_t I2C_LSM6DS3TRC_PRIMARY_ADDR = 0x6A;
inline constexpr uint8_t I2C_LSM6DS3TRC_SECONDARY_ADDR = 0x6B;
inline constexpr uint8_t LSM6DS3TRC_WHO_AM_I_VALUE = 0x6A;

/**
 * @brief Minimal LSM6DS3TR-C accelerometer/gyroscope driver.
 *
 * The driver keeps the sensor in its main user bank and exposes only the
 * features required by the device firmware: ACC/GYR sampling, wake-up
 * (any-motion) interrupt configuration, and sleep convergence.
 */
class lsm6ds3trc_tools {
public:
    enum sensor_type_t {
        SENSOR_ACCEL = 0,
        SENSOR_GYRO = 1,
    };

    enum accel_range_t {
        ACCEL_RANGE_2G = 0,
        ACCEL_RANGE_16G = 1,
        ACCEL_RANGE_4G = 2,
        ACCEL_RANGE_8G = 3,
    };

    enum gyro_range_t {
        GYRO_RANGE_250 = 0,
        GYRO_RANGE_125 = 1,
        GYRO_RANGE_500 = 2,
        GYRO_RANGE_1000 = 4,
        GYRO_RANGE_2000 = 6,
    };

    enum odr_t {
        ODR_12_5HZ = 1,
        ODR_26HZ = 2,
        ODR_52HZ = 3,
        ODR_104HZ = 4,
        ODR_208HZ = 5,
        ODR_416HZ = 6,
        ODR_833HZ = 7,
        ODR_1660HZ = 8,
        ODR_3330HZ = 9,
        ODR_6660HZ = 10,
    };

    enum int_pin_t {
        INT_PIN_1 = 0,
        INT_PIN_2 = 1,
    };

    struct sensor_data_t {
        float acc_x = 0.0f;       ///< Acceleration in g.
        float acc_y = 0.0f;
        float acc_z = 0.0f;
        float gyr_x = 0.0f;       ///< Angular rate in degrees/second.
        float gyr_y = 0.0f;
        float gyr_z = 0.0f;
        uint32_t sensor_time = 0; ///< Host timestamp in milliseconds.
        bool acc_valid = false;
        bool gyr_valid = false;
    };

    struct sensor_config_t {
        accel_range_t acc_range = ACCEL_RANGE_2G;
        gyro_range_t gyr_range = GYRO_RANGE_2000;
        odr_t acc_odr = ODR_104HZ;
        odr_t gyr_odr = ODR_104HZ;
    };

    struct motion_config_t {
        uint8_t threshold = 0x04; ///< WAKE_UP_THS[5:0].
        uint8_t duration = 0x01;  ///< WAKE_UP_DUR.wake_dur[1:0].
    };

    /**
     * @param addr 0 lets init() auto-detect 0x6A/0x6B. A non-zero address
     *             forces that address and is useful for diagnostics.
     */
    explicit lsm6ds3trc_tools(uint8_t addr = 0,
                              uint32_t bus_freq_hz = 400000);
    ~lsm6ds3trc_tools();

    esp_err_t init(i2c_bus_handle_t bus,
                   i2c_bus_device_handle_t* i2c_device = nullptr,
                   uint8_t address_hint = 0);
    esp_err_t deinit();

    esp_err_t enable_sensors(const sensor_type_t* sensors, uint8_t count);
    esp_err_t disable_sensors(const sensor_type_t* sensors, uint8_t count);
    esp_err_t configure_sensors(const sensor_config_t& config);
    esp_err_t enable_default_sensors();

    esp_err_t get_sensor_data(sensor_data_t& data);

    bool check_interrupt_status();
    esp_err_t clear_interrupt();
    esp_err_t configure_motion_interrupt(const motion_config_t& config,
                                         int_pin_t pin = INT_PIN_1,
                                         bool active_low = true,
                                         bool open_drain = true,
                                         bool latched = false);
    esp_err_t disable_interrupt(int_pin_t pin = INT_PIN_1);
    esp_err_t prepare_for_sleep(bool keep_motion_wake);

    static float convert_accel_lsb_to_g(int16_t lsb, accel_range_t range);
    static float convert_gyro_lsb_to_dps(int16_t lsb, gyro_range_t range);

    esp_err_t get_chip_id(uint8_t& chip_id);

    i2c_bus_device_handle_t device() const { return device_; }
    uint8_t address() const { return address_; }
    bool is_initialized() const { return initialized_; }

private:
    esp_err_t read_regs(uint8_t reg, uint8_t* data, size_t length);
    esp_err_t write_reg(uint8_t reg, uint8_t value);
    esp_err_t select_device(i2c_bus_device_handle_t* external_device);
    esp_err_t configure_base_registers();
    esp_err_t route_wakeup_interrupt(int_pin_t pin);

    i2c_bus_handle_t bus_ = nullptr;
    i2c_bus_device_handle_t device_ = nullptr;
    uint8_t address_ = 0;
    uint32_t bus_freq_hz_ = 400000;
    bool owns_device_ = false;
    bool initialized_ = false;
    sensor_config_t config_ = {};
    bool acc_enabled_ = false;
    bool gyr_enabled_ = false;
};

#endif // __LSM6DS3TRC_TOOLS_H__
