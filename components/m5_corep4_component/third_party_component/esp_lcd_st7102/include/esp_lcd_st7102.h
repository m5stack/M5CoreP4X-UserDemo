/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "soc/soc_caps.h"

#define ESP_LCD_ST7102_VER_MAJOR 1
#define ESP_LCD_ST7102_VER_MINOR 0
#define ESP_LCD_ST7102_VER_PATCH 0

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LCD panel initialization commands.
 *
 */
typedef struct {
    int cmd;                /*<! The specific LCD command */
    const void *data;       /*<! Buffer that holds the command specific data */
    size_t data_bytes;      /*<! Size of `data` in memory, in bytes */
    unsigned int delay_ms;  /*<! Delay in milliseconds after this command */
} st7102_lcd_init_cmd_t;

/**
 * @brief LCD panel vendor configuration.
 *
 * @note  This structure needs to be passed to the `vendor_config` field in `esp_lcd_panel_dev_config_t`.
 *
 */
typedef struct {
    const st7102_lcd_init_cmd_t *init_cmds;         /*!< Pointer to initialization commands array. Set to NULL if using default commands.
                                                     *   The array should be declared as `static const` and positioned outside the function.
                                                     *   Please refer to `vendor_specific_init_default` in source file.
                                                     */
    uint16_t init_cmds_size;                        /*<! Number of commands in above array */
    bool init_in_command_mode;                      /*!< Send LCD initialization commands in command-mode, default in video-mode */
    struct {
        esp_lcd_dsi_bus_handle_t dsi_bus;               /*!< MIPI-DSI bus configuration */
        const esp_lcd_dpi_panel_config_t *dpi_config;   /*!< MIPI-DPI panel configuration */
    } mipi_config;
} st7102_vendor_config_t;

/**
 * @brief Create LCD panel for model ST7102
 *
 * @note  Vendor specific initialization can be different between manufacturers, should consult the LCD supplier for initialization sequence code.
 *
 * @param[in]  io LCD panel IO handle
 * @param[in]  panel_dev_config General panel device configuration
 * @param[out] ret_panel Returned LCD panel handle
 * @return
 *      - ESP_ERR_INVALID_ARG   if parameter is invalid
 *      - ESP_OK                on success
 *      - Otherwise             on fail
 */
esp_err_t esp_lcd_new_panel_st7102(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief MIPI-DSI bus configuration structure
 *
 */
#define ST7102_PANEL_BUS_DSI_2CH_CONFIG()                \
    {                                                    \
        .bus_id = 0,                                     \
        .num_data_lanes = 2,                             \
        .lane_bit_rate_mbps = 900,                       \
    }

/**
 * @brief MIPI-DBI panel IO configuration structure
 *
 */
#define ST7102_PANEL_IO_DBI_CONFIG()  \
    {                                 \
        .virtual_channel = 0,         \
        .lcd_cmd_bits = 8,            \
        .lcd_param_bits = 8,          \
    }

/**
 * @brief MIPI DPI configuration structure
 *
 * @note  refresh_rate = (dpi_clock_freq_mhz * 1000000) / (h_res + hsync_pulse_width + hsync_back_porch + hsync_front_porch)
 *                                                      / (v_res + vsync_pulse_width + vsync_back_porch + vsync_front_porch)
 *
 * @param[in] px_format Pixel format of the panel
 *
 */
#define ST7102_720_1280_PANEL_60HZ_DPI_CONFIG(px_format) \
    {                                                    \
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,     \
        .dpi_clock_freq_mhz = 60,                        \
        .virtual_channel = 0,                            \
        .pixel_format = px_format,                       \
        .num_fbs = 1,                                    \
        .video_timing = {                                \
            .h_size = 720,                               \
            .v_size = 1280,                              \
            .hsync_back_porch = 40,                      \
            .hsync_pulse_width = 4,                      \
            .hsync_front_porch = 40,                     \
            .vsync_back_porch = 12,                      \
            .vsync_pulse_width = 4,                      \
            .vsync_front_porch = 16,                     \
        },                                               \
        .flags.use_dma2d = true,                         \
    }


static const st7102_lcd_init_cmd_t st7102_init_cmds[] = {
    {.cmd = 0x99, .data = (uint8_t[]){0x71, 0x02, 0xA2}, .data_bytes = 3, .delay_ms = 0},
    {.cmd = 0x99, .data = (uint8_t[]){0x71, 0x02, 0xA3}, .data_bytes = 3, .delay_ms = 0},
    {.cmd = 0x99, .data = (uint8_t[]){0x71, 0x02, 0xA4}, .data_bytes = 3, .delay_ms = 0},
    {.cmd = 0x78, .data = (uint8_t[]){0x21}, .data_bytes = 1, .delay_ms = 0},
    {.cmd = 0x79, .data = (uint8_t[]){0xCF}, .data_bytes = 1, .delay_ms = 0},
    {.cmd = 0xB0, .data = (uint8_t[]){0x22, 0x43, 0x1E, 0x43, 0x2F, 0x57, 0x57}, .data_bytes = 7, .delay_ms = 0},
    {.cmd = 0xB7, .data = (uint8_t[]){0x7D, 0x7D}, .data_bytes = 2, .delay_ms = 0},
    {.cmd = 0xBF, .data = (uint8_t[]){0x7A, 0x7A}, .data_bytes = 2, .delay_ms = 0},
    {.cmd        = 0xC8,
     .data       = (uint8_t[]){0x00, 0x00, 0x13, 0x23, 0x3E, 0x00, 0x6A, 0x03, 0xB0, 0x06, 0x11, 0x0F, 0x07,
                                      0x85, 0x03, 0x21, 0xD5, 0x01, 0x18, 0x00, 0x22, 0x56, 0x0F, 0x98, 0x0A, 0x32,
                                      0xF8, 0x0D, 0x48, 0x0F, 0xF3, 0x80, 0x0F, 0xAC, 0xC1, 0x03, 0xC4},
     .data_bytes = 37,
     .delay_ms   = 0},
    {.cmd        = 0xC9,
     .data       = (uint8_t[]){0x00, 0x00, 0x13, 0x23, 0x3E, 0x00, 0x6A, 0x03, 0xB0, 0x06, 0x11, 0x0F, 0x07,
                                      0x85, 0x03, 0x21, 0xD5, 0x01, 0x18, 0x00, 0x22, 0x56, 0x0F, 0x98, 0x0A, 0x32,
                                      0xF8, 0x0D, 0x48, 0x0F, 0xF3, 0x80, 0x0F, 0xAC, 0xC1, 0x03, 0xC4},
     .data_bytes = 37,
     .delay_ms   = 0},
    {.cmd = 0xD7, .data = (uint8_t[]){0x10, 0x0C, 0x02, 0x19, 0x40, 0x40}, .data_bytes = 6, .delay_ms = 0},
    {.cmd        = 0xA3,
     .data       = (uint8_t[]){0x40, 0x03, 0x80, 0xCF, 0x44, 0x00, 0x00, 0x00, 0x02, 0x05, 0x6F,
                                      0x6F, 0x00, 0x1A, 0x00, 0x45, 0x05, 0x00, 0x00, 0x00, 0x00, 0x46,
                                      0x00, 0x00, 0x02, 0x20, 0x52, 0x00, 0x05, 0x00, 0x00, 0xFF},
     .data_bytes = 32,
     .delay_ms   = 0},
    {.cmd = 0xA6,
     .data =
         (uint8_t[]){0x02, 0x00, 0x24, 0x55, 0x35, 0x00, 0x38, 0x00, 0x97, 0x97, 0x00, 0x24, 0x55, 0x36, 0x00,
                            0x37, 0x00, 0x97, 0x97, 0x02, 0xAC, 0x51, 0x3A, 0x00, 0x00, 0x00, 0x97, 0x97, 0x00, 0xAC,
                            0x21, 0x00, 0x0B, 0x00, 0x00, 0x97, 0x97, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00},
     .data_bytes = 44,
     .delay_ms   = 0},
    {.cmd        = 0xA7,
     .data       = (uint8_t[]){0x19, 0x19, 0x00, 0x64, 0x40, 0x07, 0x16, 0x40, 0x00, 0x04, 0x03, 0x97,
                                      0x97, 0x00, 0x64, 0x40, 0x25, 0x34, 0x00, 0x00, 0x02, 0x01, 0x97, 0x97,
                                      0x00, 0x64, 0x40, 0x4B, 0x5A, 0x00, 0x00, 0x02, 0x01, 0x97, 0x97, 0x00,
                                      0x24, 0x40, 0x69, 0x78, 0x00, 0x00, 0x00, 0x00, 0x97, 0x97, 0x00, 0x44},
     .data_bytes = 48,
     .delay_ms   = 0},
    {.cmd        = 0xAC,
     .data       = (uint8_t[]){0x11, 0x08, 0x13, 0x0A, 0x18, 0x1A, 0x1B, 0x00, 0x06, 0x03, 0x19, 0x1B, 0x1B,
                                      0x1B, 0x18, 0x1B, 0x10, 0x09, 0x12, 0x0B, 0x18, 0x1A, 0x1B, 0x02, 0x06, 0x01,
                                      0x19, 0x1B, 0x1B, 0x1B, 0x18, 0x1B, 0xFF, 0x67, 0xFF, 0x67, 0x00},
     .data_bytes = 37,
     .delay_ms   = 0},
    {.cmd = 0xAD, .data = (uint8_t[]){0xCC, 0x40, 0x46, 0x11, 0x04, 0x6F, 0x6F}, .data_bytes = 7, .delay_ms = 0},
    {.cmd  = 0xE8,
     .data = (uint8_t[]){0x30, 0x07, 0x00, 0xB3, 0xB3, 0x9C, 0x00, 0xE2, 0x04, 0x00, 0x00, 0x00, 0x00, 0xEF},
     .data_bytes = 14,
     .delay_ms   = 0},
    {.cmd = 0x75, .data = (uint8_t[]){0x03, 0x04}, .data_bytes = 2, .delay_ms = 0},
    {.cmd        = 0xE7,
     .data       = (uint8_t[]){0x8B, 0x3C, 0x00, 0x0C, 0xF0, 0x5D, 0x00, 0x5D, 0x00, 0x5D, 0x00,
                                      0x5D, 0x00, 0xFF, 0x00, 0x08, 0x7B, 0x00, 0x00, 0xC8, 0x6A, 0x5A,
                                      0x08, 0x1A, 0x3C, 0x00, 0x71, 0x01, 0x8C, 0x01, 0x7F, 0xF0, 0x22},
     .data_bytes = 33,
     .delay_ms   = 0},
    {.cmd        = 0xE9,
     .data       = (uint8_t[]){0x3C, 0x7F, 0x08, 0x07, 0x1A, 0x7A, 0x22, 0x1A, 0x33},
     .data_bytes = 9,
     .delay_ms   = 0},
    {.cmd = 0x11, .data = NULL, .data_bytes = 0, .delay_ms = 120}, // SLPOUT: 需等待 >= 120ms，内部电荷泵稳定后才能接收下一命令
    {.cmd = 0x36, .data = (uint8_t[]){0b11}, .data_bytes = 1, .delay_ms = 0},
    {.cmd = 0x29, .data = NULL, .data_bytes = 0, .delay_ms = 20},
    {.cmd = 0x35, .data = (uint8_t[]){0x00}, .data_bytes = 1, .delay_ms = 0},
    {.cmd = 0x29, .data = NULL, .data_bytes = 0, .delay_ms = 0}};

#ifdef __cplusplus
}
#endif

#endif
