/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2025 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief ESP LCD touch: cst3xx
 */

#pragma once

#include "esp_lcd_touch.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief CST3XX 工作模式枚举
 */
typedef enum {
    CST3XX_MODE_NORMAL    = 0,  ///< 正常模式
    CST3XX_MODE_GESTURE   = 1,  ///< 手势模式
    CST3XX_MODE_LP        = 2,  ///< 低功耗模式
    CST3XX_MODE_DIFF      = 3,  ///< 差分模式
    CST3XX_MODE_RAWDATA   = 4,  ///< 原始数据模式
    CST3XX_MODE_FAC_TEST  = 5,  ///< 工厂测试模式
    CST3XX_MODE_DEEPSLEEP = 6,  ///< 深度休眠模式
} cst3xx_work_mode_t;

typedef struct {
    uint8_t tx_channels;
    uint8_t rx_channels;
    uint8_t key_count;
    uint16_t resolution_x;
    uint16_t resolution_y;
    uint32_t project_id;
    uint32_t chip_id;
    uint32_t fw_version;
    uint16_t checksum;
} cst3xx_info_t;

/**
 * @brief 创建 CST3XX 触摸屏驱动
 */
esp_err_t esp_lcd_touch_new_i2c_cst3xx(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config,
                                       esp_lcd_touch_handle_t *out_touch);

esp_err_t esp_lcd_touch_cst3xx_read_info(esp_lcd_touch_handle_t tp, cst3xx_info_t *out_info);

/**
 * @brief 设置 CST3XX 工作模式
 * 
 * @param tp 触摸屏句柄
 * @param mode 工作模式 (参见 cst3xx_work_mode_t)
 * @return esp_err_t 
 */
esp_err_t esp_lcd_touch_cst3xx_set_workmode(esp_lcd_touch_handle_t tp, cst3xx_work_mode_t mode);

/**
 * @brief CST3XX 进入深度休眠
 * 
 * @param tp 触摸屏句柄
 * @return esp_err_t 
 */
esp_err_t esp_lcd_touch_cst3xx_enter_sleep(esp_lcd_touch_handle_t tp);

/**
 * @brief CST3XX 从休眠中恢复 (需要硬件复位)
 * 
 * @param tp 触摸屏句柄
 * @return esp_err_t 
 */
esp_err_t esp_lcd_touch_cst3xx_resume(esp_lcd_touch_handle_t tp);

#define ESP_LCD_TOUCH_IO_I2C_CST3XX_ADDRESS (0x58)

#define ESP_LCD_TOUCH_IO_I2C_CST3XX_CONFIG()                                                           \
    {                                                                                                  \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST3XX_ADDRESS, .control_phase_bytes = 1, .dc_bit_offset = 0, \
        .lcd_cmd_bits = 8,                                                                             \
        .flags        = {                                                                              \
                   .dc_low_on_data = 0,                                                                \
                   .disable_control_phase = 1,                                                         \
        },                                                                                      \
    }

#ifdef __cplusplus
}
#endif
