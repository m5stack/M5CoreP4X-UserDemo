/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2025 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst3xx.h"

static const char *TAG = "CST3XX";

#define CST3XX_MAX_TOUCH_POINTS 5
#define CST3XX_TOUCH_POINT_LIMIT \
    ((CST3XX_MAX_TOUCH_POINTS < CONFIG_ESP_LCD_TOUCH_MAX_POINTS) ? CST3XX_MAX_TOUCH_POINTS : CONFIG_ESP_LCD_TOUCH_MAX_POINTS)

/*******************************************************************************
 * Function definitions
 *******************************************************************************/
static esp_err_t esp_lcd_touch_cst3xx_read_data(esp_lcd_touch_handle_t tp);
static bool esp_lcd_touch_cst3xx_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                                        uint8_t *point_num, uint8_t max_point_num);
static esp_err_t esp_lcd_touch_cst3xx_del(esp_lcd_touch_handle_t tp);
static esp_err_t cst3xx_reset(esp_lcd_touch_handle_t tp);
static esp_err_t cst3xx_write_reg_nd(esp_lcd_touch_handle_t tp, uint32_t reg, size_t reg_len);
static esp_err_t cst3xx_read_reg(esp_lcd_touch_handle_t tp, uint32_t reg, size_t reg_len, uint8_t *read_buf,
                                 size_t read_len);
static esp_err_t cst3xx_read_config(esp_lcd_touch_handle_t tp);

esp_err_t esp_lcd_touch_new_i2c_cst3xx(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config,
                                       esp_lcd_touch_handle_t *out_touch)
{
    esp_err_t ret                 = ESP_OK;
    esp_lcd_touch_handle_t cst3xx = NULL;

    ESP_RETURN_ON_FALSE(io && config && out_touch, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");

    /* Allocate memory for controller */
    cst3xx = heap_caps_calloc(1, sizeof(esp_lcd_touch_t), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(cst3xx, ESP_ERR_NO_MEM, TAG, "No memory for CST3XX");

    /* Communication interface */
    cst3xx->io = io;

    /* Set callbacks */
    cst3xx->read_data = esp_lcd_touch_cst3xx_read_data;
    cst3xx->get_xy    = esp_lcd_touch_cst3xx_get_xy;
    cst3xx->del       = esp_lcd_touch_cst3xx_del;

    /* Mutex init */
    cst3xx->data.lock.owner = portMUX_FREE_VAL;

    /* Save config */
    memcpy(&cst3xx->config, config, sizeof(esp_lcd_touch_config_t));

    /* Initialize reset GPIO */
    if (cst3xx->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_config_t rst_gpio_cfg = {
            .pin_bit_mask = BIT64(cst3xx->config.rst_gpio_num),
            .mode         = GPIO_MODE_OUTPUT,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&rst_gpio_cfg), err, TAG, "GPIO config failed");
    }

    /* Hardware reset */
    ESP_GOTO_ON_ERROR(cst3xx_reset(cst3xx), err, TAG, "Reset failed");

    /* Read chip configuration */
    ESP_GOTO_ON_ERROR(cst3xx_read_config(cst3xx), err, TAG, "Read config failed");

    /* Prepare pin for touch interrupt */
    if (cst3xx->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_gpio_config = {
            .mode = GPIO_MODE_INPUT,
            .intr_type = (cst3xx->config.levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE),
            .pin_bit_mask = BIT64(cst3xx->config.int_gpio_num)
        };
        ret = gpio_config(&int_gpio_config);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "GPIO config failed");

        /* Register interrupt callback */
        if (cst3xx->config.interrupt_callback) {
            esp_lcd_touch_register_interrupt_callback(cst3xx, cst3xx->config.interrupt_callback);
        }
    }

    *out_touch = cst3xx;
    return ESP_OK;

err:
    if (cst3xx) {
        esp_lcd_touch_cst3xx_del(cst3xx);
    }
    return ret;
}

esp_err_t esp_lcd_touch_cst3xx_read_info(esp_lcd_touch_handle_t tp, cst3xx_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(tp && out_info, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");

    uint8_t buf[50] = {0};
    esp_err_t ret = cst3xx_read_reg(tp, 0xD0030000, 4, buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

#define U8TO16(low, high)  (((uint16_t)(high) << 8) | (low))
#define U8TO32(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))

    memset(out_info, 0, sizeof(*out_info));
    out_info->tx_channels = buf[48];
    out_info->rx_channels = buf[49];
    out_info->key_count = buf[27];
    out_info->resolution_x = U8TO16(buf[28], buf[29]);
    out_info->resolution_y = U8TO16(buf[30], buf[31]);
    out_info->project_id = U8TO32(buf[39], buf[38], buf[37], buf[36]);
    out_info->chip_id = U8TO32(buf[3], buf[2], buf[1], buf[0]);
    out_info->fw_version = U8TO32(buf[35], buf[34], buf[33], buf[32]);
    // This is the normal-mode information block field at 0xD003[44:45],
    // not the bootloader-computed flash checksum used by the updater.
    out_info->checksum = U8TO16(buf[44], buf[45]);

#undef U8TO16
#undef U8TO32

    return ESP_OK;
}

static esp_err_t esp_lcd_touch_cst3xx_read_data(esp_lcd_touch_handle_t tp)
{
    uint8_t data[50] = {0};
    esp_err_t ret    = ESP_OK;
    uint8_t finger_num, key_num;
    // static uint32_t debug_counter = 0;

    // read tp info
    ret = cst3xx_read_reg(tp, 0xD0070000, 4, data, 9); // 每个触摸点 9 bytes 数据
    if (ret != ESP_OK) {
        goto err;
    }

    finger_num = data[3] & 0x0F;
    key_num    = (data[3] & 0xF0) >> 4;
    uint8_t report_items = key_num + finger_num;
    report_items = (report_items > CST3XX_TOUCH_POINT_LIMIT) ? CST3XX_TOUCH_POINT_LIMIT : report_items;
    if (report_items > 1) {  // 若大于1个触摸点，则继续读取数据
        esp_lcd_panel_io_rx_param(tp->io, 0x00, &data[9], (report_items - 1) * 5);
    }

    // End of report
    ret = cst3xx_write_reg_nd(tp, 0xD00002AB, 4);
    if (ret != ESP_OK) {
        goto err;
    }
    // 注意：不要在触摸中断上下文中使用 printf/ESP_LOG，可能导致死锁
    // if (ret != ESP_OK) {
    //     printf(" End of report error\n");
    // }

    uint8_t points = finger_num;
    points         = (points > CST3XX_TOUCH_POINT_LIMIT) ? CST3XX_TOUCH_POINT_LIMIT : points;
    if (key_num >= report_items) {
        points = 0;
    } else if (key_num + points > report_items) {
        points = report_items - key_num;
    }

    uint8_t key_state = data[8] >> 4;
    
    // 调试输出：每100次循环输出一次触摸状态
    // debug_counter++;
    // if (debug_counter % 100 == 0 && (finger_num > 0 || key_state != 0)) {
    //     ESP_LOGI(TAG, "Touch status - finger_num=%d, key_state=0x%02X, data[8]=0x%02X", 
    //              finger_num, key_state, data[8]);
    // }
    
    portENTER_CRITICAL(&tp->data.lock);

    tp->data.points = 0;
    // key_state 用于判断当前是否真正有触摸（防止误触发）
    // key_state != 0 表示有真正的触摸事件（0x01, 0x02等）
    // key_state == 0 通常表示悬空或误触发
    if (finger_num && key_state != 0) {
        uint16_t index = 0;
        int pos_x, pos_y;
        for (uint8_t i = 0; i < points; i++) {
            index                = (key_num + i) * 5;
            pos_x                = data[index + 4] + ((uint16_t)(data[index + 7] & 0x0F) << 8);
            pos_y                = data[index + 5] + ((uint16_t)(data[index + 7] & 0xF0) << 4);
            tp->data.coords[i].x = pos_x;
            tp->data.coords[i].y = pos_y;
            // ESP_LOGV(TAG, "Point %d: X=%d, Y=%d", i, tp->data.coords[i].x, tp->data.coords[i].y);
        }
        // 设置实际的触摸点数量（支持多点触摸）
        tp->data.points = points;
        // 注意：不要在这里使用 ESP_LOGI，因为这个函数在临界区中被调用，
        // printf 可能导致死锁！如需调试，使用 ESP_EARLY_LOGV 或在临界区外打印
    } else {
        // 没有有效触摸
        tp->data.points = 0;
    }
    portEXIT_CRITICAL(&tp->data.lock);

    // ESP_LOGI(TAG, "Point %d: X=%d, Y=%d", 0, tp->data.coords[0].x, tp->data.coords[0].y);
    return ret;

err:
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);

    static TickType_t last_warn_tick = 0;
    const TickType_t now_tick = xTaskGetTickCount();
    if ((now_tick - last_warn_tick) > pdMS_TO_TICKS(1000)) {
        ESP_LOGW(TAG, "CST3XX read_data transient error: %s", esp_err_to_name(ret));
        last_warn_tick = now_tick;
    }
    return ESP_OK;
}

static bool esp_lcd_touch_cst3xx_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                                        uint8_t *point_num, uint8_t max_point_num)
{
    assert(tp && x && y && point_num);

    portENTER_CRITICAL(&tp->data.lock);
    *point_num = (tp->data.points > max_point_num) ? max_point_num : tp->data.points;

    for (size_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) strength[i] = 1; /* Strength not supported */
    }

    portEXIT_CRITICAL(&tp->data.lock);
    return (*point_num > 0);
}

static esp_err_t esp_lcd_touch_cst3xx_del(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
    }
    free(tp);
    return ESP_OK;
}

static esp_err_t cst3xx_reset(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, 0), TAG, "Reset low failed");
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, 1), TAG, "Reset high failed");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_OK;
}

static esp_err_t cst3xx_read_config(esp_lcd_touch_handle_t tp)
{
    esp_err_t ret = ESP_OK;

    // disable lp scan
    cst3xx_write_reg_nd(tp, 0xD0000400, 4);
    vTaskDelay(pdMS_TO_TICKS(20));
    cst3xx_write_reg_nd(tp, 0xD0000400, 4);
    vTaskDelay(pdMS_TO_TICKS(20));

    // normal mode, normal report, normal report
    uint32_t regs[] = {0xD0000000, 0xD0000C00, 0xD0000100};
    for (int i = 0; i < 3; i++) {
        cst3xx_write_reg_nd(tp, regs[i], 4);
    }

    cst3xx_info_t info = {0};
    ret = esp_lcd_touch_cst3xx_read_info(tp, &info);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Touch driver configuration:");
    ESP_LOGI(TAG, "  Channels  -> TX: %d, RX: %d, Keys: %d", info.tx_channels, info.rx_channels, info.key_count);
    ESP_LOGI(TAG, "  Resolution-> X: %d, Y: %d", info.resolution_x, info.resolution_y);
    ESP_LOGI(TAG, "  Project ID-> 0x%08lX", (unsigned long)info.project_id);
    ESP_LOGI(TAG, "  Chip ID   -> 0x%08lX", (unsigned long)info.chip_id);
    ESP_LOGI(TAG, "  FW Version-> 0x%08lX", (unsigned long)info.fw_version);
    ESP_LOGI(TAG, "  Info Sum  -> 0x%04X (0xD003[44:45], not boot flash checksum)", info.checksum);

    return ret;
}

/**
 * 无数据
 */
static esp_err_t cst3xx_write_reg_nd(esp_lcd_touch_handle_t tp, uint32_t reg, size_t reg_len)
{
    if (reg_len == 0 || reg_len > 4) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t addr_buf[4];
    for (int i = 0; i < reg_len; i++) {
        addr_buf[reg_len - 1 - i] = (reg >> (i * 8)) & 0xFF;
    }

    return esp_lcd_panel_io_tx_param(tp->io, addr_buf[0], &addr_buf[1], reg_len - 1);
}

static esp_err_t cst3xx_read_reg(esp_lcd_touch_handle_t tp, uint32_t reg, size_t reg_len, uint8_t *read_buf,
                                 size_t read_len)
{
    if (reg_len == 0 || reg_len > 4 || read_len == 0 || read_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t addr_buf[4];
    for (int i = 0; i < reg_len; i++) {
        addr_buf[reg_len - 1 - i] = (reg >> (i * 8)) & 0xFF;
    }

    esp_err_t err = esp_lcd_panel_io_tx_param(tp->io, addr_buf[0], &addr_buf[1], reg_len - 1);
    if (err != ESP_OK) return err;
    return esp_lcd_panel_io_rx_param(tp->io, 0x00, read_buf, read_len);
}

/**
 * @brief 设置 CST3XX 工作模式
 * 参考 hyn_driver_for_esp32 中的 cst66xx_set_workmode 函数实现
 * 
 * @param tp 触摸屏句柄
 * @param mode 工作模式
 * @return esp_err_t 
 */
esp_err_t esp_lcd_touch_cst3xx_set_workmode(esp_lcd_touch_handle_t tp, cst3xx_work_mode_t mode)
{
    esp_err_t ret = ESP_OK;

    // 先禁用低功耗I2C (参考 cst66xx: disable lp i2c plu)
    ret = cst3xx_write_reg_nd(tp, 0xD0000400, 4);
    vTaskDelay(pdMS_TO_TICKS(1));
    ret |= cst3xx_write_reg_nd(tp, 0xD0000400, 4);

    switch (mode) {
        case CST3XX_MODE_NORMAL:
            // 正常模式 (参考 cst66xx)
            ret |= cst3xx_write_reg_nd(tp, 0xD0000000, 4);
            ret |= cst3xx_write_reg_nd(tp, 0xD0000C00, 4);
            ret |= cst3xx_write_reg_nd(tp, 0xD0000100, 4);
            ESP_LOGI(TAG, "CST3XX set to NORMAL mode");
            break;
        case CST3XX_MODE_GESTURE:
            // 手势模式
            ret |= cst3xx_write_reg_nd(tp, 0xD0000C01, 4);
            ESP_LOGI(TAG, "CST3XX set to GESTURE mode");
            break;
        case CST3XX_MODE_LP:
            // 低功耗模式
            ret |= cst3xx_write_reg_nd(tp, 0xD00004AB, 4);
            ESP_LOGI(TAG, "CST3XX set to LP mode");
            break;
        case CST3XX_MODE_DIFF:
        case CST3XX_MODE_RAWDATA:
            // 差分/原始数据模式 - 进入调试模式
            ret |= cst3xx_write_reg_nd(tp, 0xD00002AB, 4);
            ret |= cst3xx_write_reg_nd(tp, 0xD00001AB, 4);
            ESP_LOGI(TAG, "CST3XX set to DEBUG mode (DIFF/RAWDATA)");
            break;
        case CST3XX_MODE_FAC_TEST:
            // 工厂测试模式
            ret |= cst3xx_write_reg_nd(tp, 0xD00002AB, 4);
            ret |= cst3xx_write_reg_nd(tp, 0xD00000AB, 4);
            ESP_LOGI(TAG, "CST3XX set to FAC_TEST mode");
            break;
        case CST3XX_MODE_DEEPSLEEP:
            // 深度休眠模式
            ret |= cst3xx_write_reg_nd(tp, 0xD00022AB, 4);
            ESP_LOGI(TAG, "CST3XX set to DEEPSLEEP mode");
            break;
        default:
            ESP_LOGW(TAG, "CST3XX unknown mode: %d, fallback to NORMAL", mode);
            ret |= cst3xx_write_reg_nd(tp, 0xD0000000, 4);
            ret |= cst3xx_write_reg_nd(tp, 0xD0000C00, 4);
            ret |= cst3xx_write_reg_nd(tp, 0xD0000100, 4);
            break;
    }

    return ret;
}


/**
 * @brief CST3XX 进入深度休眠
 * 
 * @param tp 触摸屏句柄
 * @return esp_err_t 
 */
esp_err_t esp_lcd_touch_cst3xx_enter_sleep(esp_lcd_touch_handle_t tp)
{
    return esp_lcd_touch_cst3xx_set_workmode(tp, CST3XX_MODE_DEEPSLEEP);
}

/**
 * @brief CST3XX 从休眠中恢复 (需要硬件复位)
 * 
 * @param tp 触摸屏句柄
 * @return esp_err_t 
 */
esp_err_t esp_lcd_touch_cst3xx_resume(esp_lcd_touch_handle_t tp)
{
    esp_err_t ret = cst3xx_reset(tp);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(50));
        ret = esp_lcd_touch_cst3xx_set_workmode(tp, CST3XX_MODE_NORMAL);
    }
    return ret;
}
