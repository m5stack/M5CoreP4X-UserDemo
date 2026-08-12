/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_st7102.h"

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save surrent value of LCD_CMD_COLMOD register
    const st7102_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    bool init_in_command_mode;
    struct {
        unsigned int reset_level: 1;
    } flags;
    // To save the original functions of MIPI DPI panel
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} st7102_panel_t;

static const char *TAG = "st7102";

static esp_err_t panel_st7102_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7102_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7102_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7102_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7102_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7102_swap_xy(esp_lcd_panel_t *panel, bool swap_xy);
static esp_err_t panel_st7102_disp_on_off(esp_lcd_panel_t *panel, bool on_off);
// Helper: read RDDID (0x04) and log
static void st7102_read_and_log_id(st7102_panel_t *st7102);

esp_err_t esp_lcd_new_panel_st7102(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    ESP_LOGI(TAG, "version: %d.%d.%d", ESP_LCD_ST7102_VER_MAJOR, ESP_LCD_ST7102_VER_MINOR,
             ESP_LCD_ST7102_VER_PATCH);
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    st7102_vendor_config_t *vendor_config = (st7102_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->mipi_config.dpi_config && vendor_config->mipi_config.dsi_bus, ESP_ERR_INVALID_ARG, TAG,
                        "invalid vendor config");

    esp_err_t ret = ESP_OK;
    st7102_panel_t *st7102 = (st7102_panel_t *)calloc(1, sizeof(st7102_panel_t));
    ESP_RETURN_ON_FALSE(st7102, ESP_ERR_NO_MEM, TAG, "no mem for st7102 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    switch (panel_dev_config->rgb_ele_order) {
#else
    switch (panel_dev_config->color_space) {
#endif
    case LCD_RGB_ELEMENT_ORDER_RGB:
        st7102->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        st7102->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        st7102->colmod_val = 0x55;
        break;
    case 18: // RGB666
        st7102->colmod_val = 0x66;
        break;
    case 24: // RGB888
        st7102->colmod_val = 0x77;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st7102->io = io;
    st7102->init_cmds = vendor_config->init_cmds;
    st7102->init_cmds_size = vendor_config->init_cmds_size;
    st7102->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7102->flags.reset_level = panel_dev_config->flags.reset_active_high;
    st7102->init_in_command_mode = vendor_config->init_in_command_mode;

    // Create MIPI DPI panel
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus, vendor_config->mipi_config.dpi_config, &panel_handle), err, TAG,
                      "create MIPI DPI panel failed");
    ESP_LOGD(TAG, "new MIPI DPI panel @%p", panel_handle);

    // Save the original functions of MIPI DPI panel
    st7102->del = panel_handle->del;
    st7102->init = panel_handle->init;
    // Overwrite the functions of MIPI DPI panel
    panel_handle->del = panel_st7102_del;
    panel_handle->init = panel_st7102_init;
    panel_handle->reset = panel_st7102_reset;
    panel_handle->mirror = panel_st7102_mirror;
    panel_handle->swap_xy = panel_st7102_swap_xy;
    panel_handle->invert_color = panel_st7102_invert_color;
    panel_handle->disp_on_off = panel_st7102_disp_on_off;
    panel_handle->user_data = st7102;
    *ret_panel = panel_handle;
    ESP_LOGD(TAG, "new st7102 panel @%p", st7102);

    // // 调试用：读取并打印 LCD 控制器 ID，可根据需要改为宏控制
    // st7102_read_and_log_id(st7102);

    return ESP_OK;

err:
    if (st7102) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st7102);
    }
    return ret;
}

static const st7102_lcd_init_cmd_t vendor_specific_init_default[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xB9, (uint8_t []){0xF1, 0x12, 0x83}, 3, 0},
    {0xB1, (uint8_t []){0x00, 0x00, 0x00, 0xDA, 0x80}, 5, 0},
    {0xB2, (uint8_t []){0xC8, 0x02, 0xF0}, 3, 0},
    {0xB3, (uint8_t []){0x10, 0x10, 0x28, 0x28, 0x03, 0xFF, 0x00, 0x00, 0x00, 0x00}, 10, 0},
    {0xB4, (uint8_t []){0x80}, 1, 0},
    {0xB5, (uint8_t []){0x0A, 0x0A}, 2, 0},
    {0xB6, (uint8_t []){0x8B, 0x8B}, 2, 0},
    {0xB8, (uint8_t []){0x26, 0x22, 0xF0, 0x13}, 4, 0},
    {0xBA, (uint8_t []){0x31, 0x81, 0x05, 0xF9, 0x0E, 0x0E, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x25, 0x00, 0x90, 0x0A, 0x00, 0x00, 0x01, 0x4F, 0x01, 0x00, 0x00, 0x37}, 27, 0},
    {0xBC, (uint8_t []){0x47}, 1, 0},
    {0xBF, (uint8_t []){0x02, 0x11, 0x00}, 3, 0},
    {0xC0, (uint8_t []){0x73, 0x73, 0x50, 0x50, 0x00, 0x00, 0x12, 0x70, 0x00}, 9, 0},
    {0xC1, (uint8_t []){0x54, 0xC0, 0x32, 0x32, 0x77, 0xF1, 0xFF, 0xFF, 0xCC, 0xCC, 0x77, 0x77}, 12, 0},
    {0xC6, (uint8_t []){0x82, 0x00, 0xBF, 0xFF, 0x00, 0xFF}, 6, 0},
    {0xC7, (uint8_t []){0xB8, 0x00, 0x0A, 0x00, 0x00, 0x02}, 6, 0},
    {0xC8, (uint8_t []){0x10, 0x40, 0x1E, 0x02}, 4, 0},
    {0xCC, (uint8_t []){0x0B}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x0C, 0x0F, 0x3F, 0x3F, 0x3F, 0x3F, 0x42, 0x05, 0x0B, 0x0B, 0x0D, 0x0E, 0x0C, 0x0F, 0x17, 0x1F, 0x00, 0x0C, 0x0F, 0x3F, 0x3F, 0x3F, 0x3F, 0x42, 0x05, 0x0B, 0x0B, 0x0D, 0x0E, 0x0C, 0x0F, 0x17, 0x1F}, 34, 0},
    {0xE3, (uint8_t []){0x07, 0x07, 0x0B, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xC0, 0x10}, 14, 0},
    {0xE9, (uint8_t []){0xC8, 0x10, 0x03, 0x05, 0x01, 0x80, 0x81, 0x12, 0x31, 0x23, 0x37, 0x81, 0x80, 0x81, 0x37, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00, 0x00, 0xF8, 0xBA, 0x02, 0x46, 0x08, 0x88, 0x88, 0x84, 0x88, 0x88, 0x88, 0xF8, 0xBA, 0x13, 0x57, 0x18, 0x88, 0x88, 0x85, 0x88, 0x88, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 63, 0},
    {0xEA, (uint8_t []){0x96, 0x12, 0x01, 0x01, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8F, 0xAB, 0x75, 0x31, 0x58, 0x88, 0x88, 0x81, 0x88, 0x88, 0x88, 0x8F, 0xAB, 0x64, 0x20, 0x48, 0x88, 0x88, 0x80, 0x88, 0x88, 0x88, 0x23, 0x00, 0x00, 0x00, 0x6B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x80, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 63, 0},
    {0xEF, (uint8_t []){0xFF, 0xFF, 0x01}, 3, 0},
    {0x11, (uint8_t []){0x00}, 1, 250},
    {0x29, (uint8_t []){0x00}, 1, 50},
};

static esp_err_t panel_st7102_del(esp_lcd_panel_t *panel)
{
    st7102_panel_t *st7102 = (st7102_panel_t *)panel->user_data;

    if (st7102->reset_gpio_num >= 0) {
        gpio_reset_pin(st7102->reset_gpio_num);
    }
    // Delete MIPI DPI panel
    st7102->del(panel);
    ESP_LOGD(TAG, "del st7102 panel @%p", st7102);
    free(st7102);

    return ESP_OK;
}

static esp_err_t panel_st7102_init(esp_lcd_panel_t *panel)
{
    st7102_panel_t *st7102 = (st7102_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7102->io;
    const st7102_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_cmd_overwritten = false;

    if (!st7102->init_in_command_mode) {
        ESP_RETURN_ON_ERROR(st7102->init(panel), TAG, "init MIPI DPI panel failed");
    }

    // st7102_read_and_log_id(st7102);

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7102->madctl_val,
    }, 1), TAG, "send command failed");
    
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
        st7102->colmod_val,
    }, 1), TAG, "send command failed");

    // vendor specific initialization, it can be different between manufacturers
    // should consult the LCD supplier for initialization sequence code
    if (st7102->init_cmds) {
        init_cmds = st7102->init_cmds;
        init_cmds_size = st7102->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(st7102_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        // TODO:
        // Check if the command has been used or conflicts with the internal
        // if (init_cmds[i].data_bytes > 0) {
        //     switch (init_cmds[i].cmd) {
        //     case LCD_CMD_MADCTL:
        //         is_cmd_overwritten = true;
        //         st7102->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
        //         break;
        //     case LCD_CMD_COLMOD:
        //         is_cmd_overwritten = true;
        //         st7102->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
        //         break;
        //     default:
        //         is_cmd_overwritten = false;
        //         break;
        //     }

        //     if (is_cmd_overwritten) {
        //         is_cmd_overwritten = false;
        //         ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence",
        //                  init_cmds[i].cmd);
        //     }
        // }

        // Send command
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }
    ESP_LOGD(TAG, "send init commands success");

    if (st7102->init_in_command_mode) {
        ESP_RETURN_ON_ERROR(st7102->init(panel), TAG, "init MIPI DPI panel failed");
    }

    return ESP_OK;
}

static esp_err_t panel_st7102_reset(esp_lcd_panel_t *panel)
{
    st7102_panel_t *st7102 = (st7102_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7102->io;

    // Perform hardware reset
    if (st7102->reset_gpio_num >= 0) {
        gpio_set_level(st7102->reset_gpio_num, !st7102->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(st7102->reset_gpio_num, st7102->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st7102->reset_gpio_num, !st7102->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else if (io) { // Perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_st7102_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7102_panel_t *st7102 = (st7102_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7102->io;
    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    uint8_t command = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;
    return esp_lcd_panel_io_tx_param(io, command, NULL, 0);
}

static esp_err_t panel_st7102_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    ESP_LOGW(TAG, "mirror is not supported by this ST7102 panel");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t panel_st7102_swap_xy(esp_lcd_panel_t *panel, bool swap_xy)
{
    ESP_LOGW(TAG, "swap_xy is not supported by this ST7102 panel");
    return ESP_ERR_NOT_SUPPORTED;
}


static esp_err_t panel_st7102_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7102_panel_t *st7102 = (st7102_panel_t *)panel->user_data;
    ESP_RETURN_ON_FALSE(st7102, ESP_ERR_INVALID_ARG, TAG, "panel user_data null");
    esp_lcd_panel_io_handle_t io = st7102->io;
    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    uint8_t command = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF; // 0x29 / 0x28
    // Per datasheet: DISPOFF / DISPON don't change GRAM contents; just panel enable status.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");

    // A short delay helps some panels stabilize after DISPON before memory writes
    if (on_off) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

static void st7102_read_and_log_id(st7102_panel_t *st7102)
{
    if (!st7102 || !st7102->io) {
        return;
    }
    // 读取并打印 LCD 控制器 ID (RDDID 0x04)
    // 返回 3 个参数:
    //   ID1: LCD module's manufacturer ID (若未编程为 0xFF)
    //   ID2: LCD module/driver version ID (若未编程为 0xFF)
    //   ID3: LCD module/driver ID (若未编程为 0xFF)
    // 注意：部分面板可能未编程这些值，全部读到 0xFF 属正常情况。
    uint8_t id[3] = {0};
    esp_err_t ret = esp_lcd_panel_io_rx_param(st7102->io, 0x04, id, sizeof(id));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "RDDID -> Manufacturer(ID1)=0x%02X, Version(ID2)=0x%02X, Driver(ID3)=0x%02X", id[0], id[1], id[2]);
    } else {
        ESP_LOGW(TAG, "read LCD ID (RDDID 0x04) failed: %s", esp_err_to_name(ret));
    }
}
#endif
