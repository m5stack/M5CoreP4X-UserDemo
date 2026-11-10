/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology Co., Ltd
 *
 * SPDX-License-Identifier: MIT
 */

#include "lsm6ds3trc_tools.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* TAG = "lsm6ds3trc";

constexpr uint8_t REG_INT1_CTRL = 0x0D;
constexpr uint8_t REG_WHO_AM_I = 0x0F;
constexpr uint8_t REG_CTRL1_XL = 0x10;
constexpr uint8_t REG_CTRL2_G = 0x11;
constexpr uint8_t REG_CTRL3_C = 0x12;
constexpr uint8_t REG_WAKE_UP_SRC = 0x1B;
constexpr uint8_t REG_STATUS_REG = 0x1E;
constexpr uint8_t REG_OUTX_L_G = 0x22;
constexpr uint8_t REG_TAP_CFG = 0x58;
constexpr uint8_t REG_WAKE_UP_THS = 0x5B;
constexpr uint8_t REG_WAKE_UP_DUR = 0x5C;
constexpr uint8_t REG_MD1_CFG = 0x5E;
constexpr uint8_t REG_MD2_CFG = 0x5F;

constexpr uint8_t CTRL3_C_SW_RESET = 1U << 0;
constexpr uint8_t CTRL3_C_IF_INC = 1U << 2;
constexpr uint8_t CTRL3_C_PP_OD = 1U << 4;
constexpr uint8_t CTRL3_C_H_LACTIVE = 1U << 5;
constexpr uint8_t CTRL3_C_BDU = 1U << 6;

constexpr uint8_t STATUS_XLDA = 1U << 0;
constexpr uint8_t STATUS_GDA = 1U << 1;
constexpr uint8_t WAKE_UP_SRC_WU_IA = 1U << 3;
constexpr uint8_t TAP_CFG_INTERRUPTS_ENABLE = 1U << 7;
constexpr uint8_t TAP_CFG_LIR = 1U << 0;
constexpr uint8_t MD_CFG_WAKE_UP = 1U << 5;

constexpr uint8_t WHO_AM_I_BIT_MASK = 0x7F;

int16_t decode_le16(const uint8_t* data)
{
    return static_cast<int16_t>(static_cast<uint16_t>(data[0]) |
                                (static_cast<uint16_t>(data[1]) << 8));
}

} // namespace

lsm6ds3trc_tools::lsm6ds3trc_tools(uint8_t addr, uint32_t bus_freq_hz)
    : address_(addr), bus_freq_hz_(bus_freq_hz)
{
}

lsm6ds3trc_tools::~lsm6ds3trc_tools()
{
    deinit();
}

esp_err_t lsm6ds3trc_tools::init(i2c_bus_handle_t bus,
                                 i2c_bus_device_handle_t* i2c_device,
                                 uint8_t address_hint)
{
    if (initialized_) return ESP_OK;
    if (!bus) return ESP_ERR_INVALID_ARG;

    bus_ = bus;
    if (address_hint != 0) address_ = address_hint;
    esp_err_t ret = select_device(i2c_device);
    if (ret != ESP_OK) return ret;

    uint8_t chip_id = 0;
    ret = read_regs(REG_WHO_AM_I, &chip_id, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed at 0x%02X: %s",
                 address_, esp_err_to_name(ret));
        deinit();
        return ret;
    }
    if ((chip_id & WHO_AM_I_BIT_MASK) != LSM6DS3TRC_WHO_AM_I_VALUE) {
        ESP_LOGE(TAG, "Unexpected WHO_AM_I 0x%02X at 0x%02X (expected 0x%02X)",
                 chip_id, address_, LSM6DS3TRC_WHO_AM_I_VALUE);
        deinit();
        return ESP_ERR_NOT_FOUND;
    }

    ret = write_reg(REG_CTRL3_C, CTRL3_C_SW_RESET);
    if (ret != ESP_OK) {
        deinit();
        return ret;
    }

    // Reset is self-clearing. Bound the wait so a stuck sensor cannot block boot.
    const int64_t start_us = esp_timer_get_time();
    uint8_t ctrl3 = CTRL3_C_SW_RESET;
    do {
        vTaskDelay(pdMS_TO_TICKS(2));
        ret = read_regs(REG_CTRL3_C, &ctrl3, 1);
        if (ret != ESP_OK) {
            deinit();
            return ret;
        }
    } while ((ctrl3 & CTRL3_C_SW_RESET) != 0 &&
             (esp_timer_get_time() - start_us) < 100000);
    if ((ctrl3 & CTRL3_C_SW_RESET) != 0) {
        deinit();
        return ESP_ERR_TIMEOUT;
    }

    initialized_ = true;
    ret = configure_base_registers();
    if (ret != ESP_OK) {
        deinit();
        return ret;
    }

    ESP_LOGI(TAG, "LSM6DS3TR-C ready at 0x%02X (WHO_AM_I=0x%02X)",
             address_, chip_id);
    return ESP_OK;
}

esp_err_t lsm6ds3trc_tools::deinit()
{
    esp_err_t first_err = ESP_OK;
    if (device_) {
        const uint8_t addr = address_;
        if (owns_device_) {
            i2c_bus_device_handle_t handle = device_;
            const esp_err_t ret = i2c_bus_device_delete(&handle);
            if (ret != ESP_OK && first_err == ESP_OK) first_err = ret;
            ESP_LOGD(TAG, "Released LSM6DS3TR-C device handle at 0x%02X", addr);
        }
        device_ = nullptr;
    }
    bus_ = nullptr;
    address_ = 0;
    initialized_ = false;
    owns_device_ = false;
    acc_enabled_ = false;
    gyr_enabled_ = false;
    return first_err;
}

esp_err_t lsm6ds3trc_tools::select_device(
    i2c_bus_device_handle_t* external_device)
{
    device_ = nullptr;
    owns_device_ = false;

    if (external_device && *external_device) {
        if (address_ == 0) address_ = I2C_LSM6DS3TRC_PRIMARY_ADDR;
        device_ = *external_device;
        return ESP_OK;
    }

    if (address_ != 0) {
        device_ = i2c_bus_device_create(bus_, address_, bus_freq_hz_);
        if (!device_) return ESP_ERR_NO_MEM;
        owns_device_ = true;
        return ESP_OK;
    }

    const uint8_t candidates[] = {
        I2C_LSM6DS3TRC_PRIMARY_ADDR,
        I2C_LSM6DS3TRC_SECONDARY_ADDR,
    };
    esp_err_t last_error = ESP_ERR_NOT_FOUND;
    for (const uint8_t candidate : candidates) {
        i2c_bus_device_handle_t handle =
            i2c_bus_device_create(bus_, candidate, bus_freq_hz_);
        if (!handle) {
            last_error = ESP_ERR_NO_MEM;
            continue;
        }

        uint8_t chip_id = 0;
        const esp_err_t ret = i2c_bus_read_byte(handle, REG_WHO_AM_I, &chip_id);
        if (ret == ESP_OK &&
            (chip_id & WHO_AM_I_BIT_MASK) == LSM6DS3TRC_WHO_AM_I_VALUE) {
            device_ = handle;
            address_ = candidate;
            owns_device_ = true;
            return ESP_OK;
        }

        if (ret != ESP_OK) last_error = ret;
        i2c_bus_device_delete(&handle);
    }
    return last_error;
}

esp_err_t lsm6ds3trc_tools::read_regs(uint8_t reg, uint8_t* data,
                                      size_t length)
{
    if (!device_ || !data || length == 0) return ESP_ERR_INVALID_ARG;
    return i2c_bus_read_bytes(device_, reg, length, data);
}

esp_err_t lsm6ds3trc_tools::write_reg(uint8_t reg, uint8_t value)
{
    if (!device_) return ESP_ERR_INVALID_STATE;
    return i2c_bus_write_byte(device_, reg, value);
}

esp_err_t lsm6ds3trc_tools::configure_base_registers()
{
    // BDU avoids torn samples; IF_INC keeps multi-byte reads contiguous.
    esp_err_t ret = write_reg(REG_CTRL3_C, CTRL3_C_BDU | CTRL3_C_IF_INC);
    if (ret != ESP_OK) return ret;
    return configure_sensors(config_);
}

esp_err_t lsm6ds3trc_tools::configure_sensors(const sensor_config_t& config)
{
    if (!initialized_) return ESP_ERR_INVALID_STATE;

    config_ = config;
    uint8_t ctrl1 = static_cast<uint8_t>(
        (static_cast<uint8_t>(config.acc_odr) << 4) |
        (static_cast<uint8_t>(config.acc_range) << 2));
    uint8_t ctrl2 = static_cast<uint8_t>(
        (static_cast<uint8_t>(config.gyr_odr) << 4) |
        (static_cast<uint8_t>(config.gyr_range) << 1));

    esp_err_t ret = write_reg(REG_CTRL1_XL, ctrl1);
    if (ret != ESP_OK) return ret;
    ret = write_reg(REG_CTRL2_G, ctrl2);
    if (ret != ESP_OK) return ret;

    acc_enabled_ = config.acc_odr != 0;
    gyr_enabled_ = config.gyr_odr != 0;
    return ESP_OK;
}

esp_err_t lsm6ds3trc_tools::enable_default_sensors()
{
    return configure_sensors(config_);
}

esp_err_t lsm6ds3trc_tools::enable_sensors(const sensor_type_t* sensors,
                                           uint8_t count)
{
    if (!initialized_ || (!sensors && count != 0)) return ESP_ERR_INVALID_ARG;
    for (uint8_t i = 0; i < count; ++i) {
        if (sensors[i] == SENSOR_ACCEL) acc_enabled_ = true;
        if (sensors[i] == SENSOR_GYRO) gyr_enabled_ = true;
    }

    const uint8_t ctrl1 = acc_enabled_
        ? static_cast<uint8_t>((static_cast<uint8_t>(config_.acc_odr) << 4) |
                               (static_cast<uint8_t>(config_.acc_range) << 2))
        : 0;
    const uint8_t ctrl2 = gyr_enabled_
        ? static_cast<uint8_t>((static_cast<uint8_t>(config_.gyr_odr) << 4) |
                               (static_cast<uint8_t>(config_.gyr_range) << 1))
        : 0;
    esp_err_t ret = write_reg(REG_CTRL1_XL, ctrl1);
    if (ret != ESP_OK) return ret;
    return write_reg(REG_CTRL2_G, ctrl2);
}

esp_err_t lsm6ds3trc_tools::disable_sensors(const sensor_type_t* sensors,
                                            uint8_t count)
{
    if (!initialized_ || (!sensors && count != 0)) return ESP_ERR_INVALID_ARG;
    for (uint8_t i = 0; i < count; ++i) {
        if (sensors[i] == SENSOR_ACCEL) acc_enabled_ = false;
        if (sensors[i] == SENSOR_GYRO) gyr_enabled_ = false;
    }
    const uint8_t ctrl1 = acc_enabled_
        ? static_cast<uint8_t>((static_cast<uint8_t>(config_.acc_odr) << 4) |
                               (static_cast<uint8_t>(config_.acc_range) << 2))
        : 0;
    const uint8_t ctrl2 = gyr_enabled_
        ? static_cast<uint8_t>((static_cast<uint8_t>(config_.gyr_odr) << 4) |
                               (static_cast<uint8_t>(config_.gyr_range) << 1))
        : 0;
    const esp_err_t accel_ret = write_reg(REG_CTRL1_XL, ctrl1);
    const esp_err_t gyro_ret = write_reg(REG_CTRL2_G, ctrl2);
    return accel_ret != ESP_OK ? accel_ret : gyro_ret;
}

esp_err_t lsm6ds3trc_tools::get_sensor_data(sensor_data_t& data)
{
    data = {};
    if (!initialized_) return ESP_ERR_INVALID_STATE;

    uint8_t status = 0;
    esp_err_t ret = read_regs(REG_STATUS_REG, &status, 1);
    if (ret != ESP_OK) return ret;

    data.acc_valid = (status & STATUS_XLDA) != 0;
    data.gyr_valid = (status & STATUS_GDA) != 0;
    data.sensor_time = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if (!data.acc_valid && !data.gyr_valid) return ESP_OK;

    uint8_t raw[12] = {};
    ret = read_regs(REG_OUTX_L_G, raw, sizeof(raw));
    if (ret != ESP_OK) {
        data.acc_valid = false;
        data.gyr_valid = false;
        return ret;
    }

    const int16_t gx = decode_le16(&raw[0]);
    const int16_t gy = decode_le16(&raw[2]);
    const int16_t gz = decode_le16(&raw[4]);
    const int16_t ax = decode_le16(&raw[6]);
    const int16_t ay = decode_le16(&raw[8]);
    const int16_t az = decode_le16(&raw[10]);

    data.gyr_x = convert_gyro_lsb_to_dps(gx, config_.gyr_range);
    data.gyr_y = convert_gyro_lsb_to_dps(gy, config_.gyr_range);
    data.gyr_z = convert_gyro_lsb_to_dps(gz, config_.gyr_range);
    data.acc_x = convert_accel_lsb_to_g(ax, config_.acc_range);
    data.acc_y = convert_accel_lsb_to_g(ay, config_.acc_range);
    data.acc_z = convert_accel_lsb_to_g(az, config_.acc_range);
    return ESP_OK;
}

bool lsm6ds3trc_tools::check_interrupt_status()
{
    if (!initialized_) return false;
    uint8_t wake_src = 0;
    if (read_regs(REG_WAKE_UP_SRC, &wake_src, 1) != ESP_OK) return false;
    return (wake_src & WAKE_UP_SRC_WU_IA) != 0;
}

esp_err_t lsm6ds3trc_tools::clear_interrupt()
{
    if (!initialized_) return ESP_ERR_INVALID_STATE;
    uint8_t status = 0;
    // Reading the source registers clears their latched flags.
    esp_err_t ret = read_regs(REG_WAKE_UP_SRC, &status, 1);
    if (ret != ESP_OK) return ret;
    return read_regs(REG_STATUS_REG, &status, 1);
}

esp_err_t lsm6ds3trc_tools::route_wakeup_interrupt(int_pin_t pin)
{
    if (pin == INT_PIN_1) return write_reg(REG_MD1_CFG, MD_CFG_WAKE_UP);
    if (pin == INT_PIN_2) return write_reg(REG_MD2_CFG, MD_CFG_WAKE_UP);
    return ESP_ERR_INVALID_ARG;
}

esp_err_t lsm6ds3trc_tools::configure_motion_interrupt(
    const motion_config_t& config, int_pin_t pin, bool active_low,
    bool open_drain, bool latched)
{
    if (!initialized_) return ESP_ERR_INVALID_STATE;

    uint8_t ctrl3 = CTRL3_C_BDU | CTRL3_C_IF_INC;
    if (active_low) ctrl3 |= CTRL3_C_H_LACTIVE;
    if (open_drain) ctrl3 |= CTRL3_C_PP_OD;
    esp_err_t ret = write_reg(REG_CTRL3_C, ctrl3);
    if (ret != ESP_OK) return ret;

    uint8_t tap_cfg = TAP_CFG_INTERRUPTS_ENABLE;
    if (latched) tap_cfg |= TAP_CFG_LIR;
    ret = write_reg(REG_TAP_CFG, tap_cfg);
    if (ret != ESP_OK) return ret;

    ret = write_reg(REG_WAKE_UP_THS, config.threshold & 0x3F);
    if (ret != ESP_OK) return ret;

    uint8_t wake_dur = 0;
    ret = read_regs(REG_WAKE_UP_DUR, &wake_dur, 1);
    if (ret != ESP_OK) return ret;
    wake_dur = static_cast<uint8_t>((wake_dur & 0xFC) | (config.duration & 0x03));
    ret = write_reg(REG_WAKE_UP_DUR, wake_dur);
    if (ret != ESP_OK) return ret;

    ret = write_reg(REG_INT1_CTRL, 0);
    if (ret != ESP_OK) return ret;
    ret = route_wakeup_interrupt(pin);
    if (ret != ESP_OK) return ret;

    return clear_interrupt();
}

esp_err_t lsm6ds3trc_tools::disable_interrupt(int_pin_t pin)
{
    if (!initialized_) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = write_reg(REG_INT1_CTRL, 0);
    if (ret != ESP_OK) return ret;
    if (pin == INT_PIN_1) return write_reg(REG_MD1_CFG, 0);
    if (pin == INT_PIN_2) return write_reg(REG_MD2_CFG, 0);
    return ESP_ERR_INVALID_ARG;
}

esp_err_t lsm6ds3trc_tools::prepare_for_sleep(bool keep_motion_wake)
{
    if (!initialized_) return ESP_ERR_INVALID_STATE;

    if (!keep_motion_wake) {
        esp_err_t ret = write_reg(REG_CTRL1_XL, 0);
        if (ret != ESP_OK) return ret;
        ret = write_reg(REG_CTRL2_G, 0);
        if (ret != ESP_OK) return ret;
        ret = disable_interrupt(INT_PIN_1);
        if (ret != ESP_OK) return ret;
        acc_enabled_ = false;
        gyr_enabled_ = false;
        return ESP_OK;
    }

    sensor_type_t accel = SENSOR_ACCEL;
    esp_err_t ret = enable_sensors(&accel, 1);
    if (ret != ESP_OK) return ret;
    ret = write_reg(REG_CTRL2_G, 0);
    if (ret != ESP_OK) return ret;
    gyr_enabled_ = false;

    motion_config_t motion = {};
    motion.threshold = 0x04;
    motion.duration = 0x01;
    return configure_motion_interrupt(motion, INT_PIN_1, true, true, true);
}

float lsm6ds3trc_tools::convert_accel_lsb_to_g(int16_t lsb,
                                               accel_range_t range)
{
    float mg_per_lsb = 0.061f;
    switch (range) {
        case ACCEL_RANGE_2G: mg_per_lsb = 0.061f; break;
        case ACCEL_RANGE_16G: mg_per_lsb = 0.488f; break;
        case ACCEL_RANGE_4G: mg_per_lsb = 0.122f; break;
        case ACCEL_RANGE_8G: mg_per_lsb = 0.244f; break;
    }
    return static_cast<float>(lsb) * mg_per_lsb / 1000.0f;
}

float lsm6ds3trc_tools::convert_gyro_lsb_to_dps(int16_t lsb,
                                                gyro_range_t range)
{
    float mdps_per_lsb = 70.0f;
    switch (range) {
        case GYRO_RANGE_250: mdps_per_lsb = 8.75f; break;
        case GYRO_RANGE_125: mdps_per_lsb = 4.375f; break;
        case GYRO_RANGE_500: mdps_per_lsb = 17.50f; break;
        case GYRO_RANGE_1000: mdps_per_lsb = 35.0f; break;
        case GYRO_RANGE_2000: mdps_per_lsb = 70.0f; break;
    }
    return static_cast<float>(lsb) * mdps_per_lsb / 1000.0f;
}

esp_err_t lsm6ds3trc_tools::get_chip_id(uint8_t& chip_id)
{
    chip_id = 0;
    if (!initialized_) return ESP_ERR_INVALID_STATE;
    return read_regs(REG_WHO_AM_I, &chip_id, 1);
}

