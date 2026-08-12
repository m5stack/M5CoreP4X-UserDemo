#include "esp_lcd_touch_cst3530_update.h"

#include "CST3530_fw_v1.1.1.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "CST3530_UPDATE";

#define CST66XX_BIN_SIZE  (40U * 1024U)
#define CST66XX_PKG_SIZE  1024U
#define CST3530_BOOT_ADDR 0x5AU
#define CST3530_MAIN_ADDR 0x58U
#define CST3530_I2C_TIMEOUT_MS 1000

#define U8TO16(x1, x2) ((((uint16_t)(x1) & 0xFFU) << 8) | ((uint16_t)(x2) & 0xFFU))
#define U8TO32(x1, x2, x3, x4) \
    ((((uint32_t)(x1) & 0xFFU) << 24) | (((uint32_t)(x2) & 0xFFU) << 16) | \
     (((uint32_t)(x3) & 0xFFU) << 8) | ((uint32_t)(x4) & 0xFFU))
#define U16REV(x) ((((uint16_t)(x) << 8) & 0xFF00U) | (((uint16_t)(x) >> 8) & 0x00FFU))

typedef struct {
    cst3530_update_config_t config;
    i2c_master_dev_handle_t main_dev;
    i2c_master_dev_handle_t boot_dev;
    i2c_master_dev_handle_t current_dev;
    bool boot_pass;
} cst3530_update_ctx_t;

static void report_status(cst3530_update_ctx_t *ctx,
                          cst3530_update_event_t event,
                          int progress,
                          esp_err_t err,
                          int attempt,
                          int packet_index,
                          int packet_count,
                          uint32_t fw_checksum,
                          uint32_t ic_checksum,
                          const char *fmt,
                          ...)
{
    cst3530_update_status_t status = {
        .event = event,
        .progress = progress,
        .err = err,
        .attempt = attempt,
        .packet_index = packet_index,
        .packet_count = packet_count,
        .fw_checksum = fw_checksum,
        .ic_checksum = ic_checksum,
    };

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status.message, sizeof(status.message), fmt ? fmt : "", ap);
    va_end(ap);

    if (event == CST3530_UPDATE_EVENT_ERROR || err != ESP_OK) {
        ESP_LOGE(TAG, "%s", status.message);
    } else {
        ESP_LOGI(TAG, "%s", status.message);
    }

    if (ctx && ctx->config.status_cb) {
        ctx->config.status_cb(&status, ctx->config.status_user_ctx);
    }
}

static void set_i2c_addr(cst3530_update_ctx_t *ctx, uint8_t addr)
{
    ctx->current_dev = (addr == CST3530_BOOT_ADDR) ? ctx->boot_dev : ctx->main_dev;
    ESP_LOGI(TAG, "I2C target -> 0x%02X (%s)", addr, addr == CST3530_BOOT_ADDR ? "boot" : "main");
}

static uint8_t current_i2c_addr(const cst3530_update_ctx_t *ctx)
{
    return ctx && ctx->current_dev == ctx->boot_dev ? CST3530_BOOT_ADDR : CST3530_MAIN_ADDR;
}

static esp_err_t add_i2c_device(i2c_master_bus_handle_t bus,
                                uint8_t addr,
                                uint32_t speed_hz,
                                i2c_master_dev_handle_t *out_dev)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = speed_hz,
    };
    return i2c_master_bus_add_device(bus, &dev_config, out_dev);
}

static esp_err_t i2c_write_raw(cst3530_update_ctx_t *ctx, const uint8_t *buf, size_t len)
{
    if (!ctx || !ctx->current_dev || !buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit(ctx->current_dev, buf, len, CST3530_I2C_TIMEOUT_MS);
}

static esp_err_t wr_reg(cst3530_update_ctx_t *ctx, uint32_t reg_addr, uint8_t reg_len, uint8_t *rbuf, size_t rlen)
{
    uint8_t wbuf[4] = {0};
    reg_len &= 0x0F;
    if (reg_len == 0 || reg_len > sizeof(wbuf)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = reg_len - 1; i >= 0; --i) {
        wbuf[i] = (uint8_t)(reg_addr & 0xFFU);
        reg_addr >>= 8;
    }

    if (rlen == 0) {
        return i2c_write_raw(ctx, wbuf, reg_len);
    }
    if (!rbuf) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(ctx->current_dev, wbuf, reg_len, rbuf, rlen, CST3530_I2C_TIMEOUT_MS);
}

static esp_err_t wait_ready(cst3530_update_ctx_t *ctx,
                            uint16_t times,
                            uint8_t ms,
                            uint16_t reg,
                            uint16_t check_value,
                            uint16_t *last_value,
                            uint16_t *poll_count)
{
    uint8_t buf[4] = {0};
    uint16_t polls = 0;
    uint16_t last = 0xFFFF;

    while (times--) {
        ++polls;
        if (wr_reg(ctx, reg, 2, buf, 2) == ESP_OK) {
            last = U8TO16(buf[0], buf[1]);
            if (last == check_value) {
                if (last_value) {
                    *last_value = last;
                }
                if (poll_count) {
                    *poll_count = polls;
                }
                ESP_LOGI(TAG, "Ready 0x%04X matched 0x%04X after %u polls", reg, check_value, polls);
                return ESP_OK;
            }
        } else {
            ESP_LOGW(TAG, "Ready poll failed: addr=0x%02X reg=0x%04X poll=%u", current_i2c_addr(ctx), reg, polls);
        }
        vTaskDelay(pdMS_TO_TICKS(ms));
    }

    if (last_value) {
        *last_value = last;
    }
    if (poll_count) {
        *poll_count = polls;
    }
    ESP_LOGW(TAG,
             "Ready timeout: addr=0x%02X reg=0x%04X expect=0x%04X last=0x%04X polls=%u",
             current_i2c_addr(ctx),
             reg,
             check_value,
             last,
             polls);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t write_reg_checked(cst3530_update_ctx_t *ctx,
                                   uint32_t reg_addr,
                                   uint8_t reg_len,
                                   const char *label)
{
    esp_err_t ret = wr_reg(ctx, reg_addr, reg_len, NULL, 0);
    if (ret != ESP_OK) {
        report_status(ctx, CST3530_UPDATE_EVENT_INFO, 0, ret, 0, 0, 0, 0, 0,
                      "%s failed: addr=0x%02X reg=0x%08lx len=%u err=%s",
                      label ? label : "write_reg",
                      current_i2c_addr(ctx),
                      (unsigned long)reg_addr,
                      (unsigned)reg_len,
                      esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t touch_reset(cst3530_update_ctx_t *ctx)
{
    if (!ctx->config.reset_cb) {
        return ESP_ERR_INVALID_STATE;
    }
    return ctx->config.reset_cb(ctx->config.reset_user_ctx);
}

static esp_err_t enter_boot_with_retries(cst3530_update_ctx_t *ctx, int max_retries)
{
    if (max_retries <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    report_status(ctx, CST3530_UPDATE_EVENT_ENTER_BOOT, 3, ESP_OK, 0, 0, 0, 0, 0,
                  "Enter boot: switch I2C to 0x%02X, reset then send 0xA001A8", CST3530_BOOT_ADDR);
    set_i2c_addr(ctx, CST3530_BOOT_ADDR);
    for (int retry = 1; retry <= max_retries; ++retry) {
        report_status(ctx, CST3530_UPDATE_EVENT_ENTER_BOOT, 4, ESP_OK, retry, 0, 0, 0, 0,
                      "Enter boot retry %d/%d: delay 1000ms, reset P8, post-delay %dms", retry, max_retries, retry);
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_RETURN_ON_ERROR(touch_reset(ctx), TAG, "Touch reset failed");
        vTaskDelay(pdMS_TO_TICKS(retry));

        esp_err_t ret = write_reg_checked(ctx, 0xA001A8, 3, "Boot command 0xA001A8");
        if (ret != ESP_OK) {
            report_status(ctx, CST3530_UPDATE_EVENT_ENTER_BOOT, 4, ret, retry, 0, 0, 0, 0,
                          "Boot command NACK on retry %d/%d", retry, max_retries);
            continue;
        }

        uint16_t ready = 0;
        uint16_t polls = 0;
        ret = wait_ready(ctx, 10, 2, 0xA002, 0x22DD, &ready, &polls);
        if (ret == ESP_OK) {
            report_status(ctx, CST3530_UPDATE_EVENT_ENTER_BOOT, 8, ESP_OK, retry, 0, 0, 0, 0,
                          "Boot mode ready retry=%d ready=0x%04X polls=%u", retry, ready, polls);
            return ESP_OK;
        }

        report_status(ctx, CST3530_UPDATE_EVENT_ENTER_BOOT, 4, ret, retry, 0, 0, 0, 0,
                      "Boot ready timeout retry=%d/%d last=0x%04X polls=%u", retry, max_retries, ready, polls);
    }

    report_status(ctx, CST3530_UPDATE_EVENT_ERROR, 4, ESP_ERR_TIMEOUT, 0, 0, 0, 0, 0,
                  "Failed to enter boot mode");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t enter_boot(cst3530_update_ctx_t *ctx)
{
    return enter_boot_with_retries(ctx, 3);
}

static esp_err_t erase_flash(cst3530_update_ctx_t *ctx, uint16_t start_addr, uint16_t len, uint16_t type)
{
    const uint32_t start_cmd = 0xA0060000U + U16REV(start_addr);
    const uint32_t len_cmd = 0xA0080000U + U16REV(len);
    const uint32_t type_cmd = 0xA00A0000U + U16REV(type);

    report_status(ctx, CST3530_UPDATE_EVENT_ERASE, 10, ESP_OK, 0, 0, 0, 0, 0,
                  "Erase setup start=0x%04X len=0x%04X type=0x%04X cmds=%08lX/%08lX/%08lX",
                  start_addr,
                  len,
                  type,
                  (unsigned long)start_cmd,
                  (unsigned long)len_cmd,
                  (unsigned long)type_cmd);

    esp_err_t ret = write_reg_checked(ctx, start_cmd, 4, "Erase start address");
    if (ret != ESP_OK) return ret;
    ret = write_reg_checked(ctx, len_cmd, 4, "Erase length");
    if (ret != ESP_OK) return ret;
    ret = write_reg_checked(ctx, type_cmd, 4, "Erase type");
    if (ret != ESP_OK) return ret;
    ret = write_reg_checked(ctx, 0xA018CACA, 4, "Erase unlock key");
    if (ret != ESP_OK) return ret;
    ret = write_reg_checked(ctx, 0xA004E0, 3, "Erase execute");
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(20));

    uint16_t ready = 0;
    uint16_t polls = 0;
    ret = wait_ready(ctx, 100, 1, 0xA020, 0xE088, &ready, &polls);
    report_status(ctx,
                  CST3530_UPDATE_EVENT_ERASE,
                  ret == ESP_OK ? 18 : 10,
                  ret,
                  0,
                  0,
                  0,
                  0,
                  0,
                  "Erase ready start=0x%04X len=0x%04X ret=%s last=0x%04X polls=%u",
                  start_addr,
                  len,
                  esp_err_to_name(ret),
                  ready,
                  polls);
    return ret;
}

static esp_err_t read_checksum_block(cst3530_update_ctx_t *ctx,
                                     uint16_t start_val,
                                     uint16_t start_addr,
                                     uint16_t len,
                                     uint32_t *check_sum)
{
    uint8_t buf[8] = {0};
    for (int retry = 0; retry < 3; ++retry) {
        const uint32_t start_cmd = U8TO32(0xA0, 0x06, start_addr & 0xFF, start_addr >> 8);
        const uint32_t len_cmd = U8TO32(0xA0, 0x08, len & 0xFF, len >> 8);
        const uint32_t seed_cmd = U8TO32(0xA0, 0x0A, start_val & 0xFF, start_val >> 8);

        report_status(ctx, CST3530_UPDATE_EVENT_VERIFY, 94, ESP_OK, retry + 1, 0, 0, 0, 0,
                      "Checksum block retry=%d/3 start_val=%u start=0x%04X len=0x%04X",
                      retry + 1,
                      start_val,
                      start_addr,
                      len);

        esp_err_t ret = write_reg_checked(ctx, start_cmd, 4, "Checksum start address");
        if (ret == ESP_OK) ret = write_reg_checked(ctx, len_cmd, 4, "Checksum length");
        if (ret == ESP_OK) ret = write_reg_checked(ctx, seed_cmd, 4, "Checksum seed/type");
        if (ret == ESP_OK) ret = write_reg_checked(ctx, 0xA004D6, 3, "Checksum execute");
        if (ret != ESP_OK) {
            report_status(ctx, CST3530_UPDATE_EVENT_VERIFY, 94, ret, retry + 1, 0, 0, 0, 0,
                          "Checksum command setup failed retry=%d/3 ret=%s", retry + 1, esp_err_to_name(ret));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(len / 0xC00 + 1));
        uint16_t ready = 0;
        uint16_t polls = 0;
        ret = wait_ready(ctx, 20, 2, 0xA020, 0xD688, &ready, &polls);
        if (ret != ESP_OK) {
            report_status(ctx, CST3530_UPDATE_EVENT_VERIFY, 94, ret, retry + 1, 0, 0, 0, 0,
                          "Checksum ready timeout retry=%d/3 last=0x%04X polls=%u",
                          retry + 1,
                          ready,
                          polls);
            continue;
        }

        ret = wr_reg(ctx, 0xA040, 2, buf, 5);
        if (ret == ESP_OK && buf[0] == 0xCA) {
            *check_sum = U8TO32(buf[4], buf[3], buf[2], buf[1]);
            report_status(ctx, CST3530_UPDATE_EVENT_VERIFY, 95, ESP_OK, retry + 1, 0, 0, 0, *check_sum,
                          "Checksum block OK start_val=%u start=0x%04X len=0x%04X raw=%02X %02X %02X %02X %02X sum=%08lX",
                          start_val,
                          start_addr,
                          len,
                          buf[0],
                          buf[1],
                          buf[2],
                          buf[3],
                          buf[4],
                          (unsigned long)*check_sum);
            return ESP_OK;
        }

        esp_err_t read_err = ret == ESP_OK ? ESP_FAIL : ret;
        report_status(ctx, CST3530_UPDATE_EVENT_VERIFY, 94, read_err, retry + 1, 0, 0, 0, 0,
                      "Checksum read failed retry=%d/3 ret=%s header=0x%02X",
                      retry + 1,
                      esp_err_to_name(read_err),
                      buf[0]);
    }
    return ESP_FAIL;
}

static uint32_t read_ic_checksum(cst3530_update_ctx_t *ctx)
{
    uint32_t chip_sum1 = 0;
    uint32_t chip_sum2 = 0;
    uint32_t total_sum = 0;
    ctx->boot_pass = false;

    report_status(ctx, CST3530_UPDATE_EVENT_VERIFY, 93, ESP_OK, 0, 0, 0, 0, 0,
                  "Read IC checksum: block1 0x0000+0x9000, block2 0xB000+0x0FFC");

    esp_err_t sum1_ret = read_checksum_block(ctx, 0, 0, 0x9000, &chip_sum1);
    esp_err_t sum2_ret = sum1_ret == ESP_OK ? read_checksum_block(ctx, 1, 0xB000, 0x0FFC, &chip_sum2) : sum1_ret;
    if (sum1_ret == ESP_OK && sum2_ret == ESP_OK) {
        total_sum = chip_sum1 + chip_sum2 * 2U - 0x55U;
        ctx->boot_pass = true;
    } else {
        report_status(ctx, CST3530_UPDATE_EVENT_VERIFY, 96, sum2_ret, 0, 0, 0, 0, 0,
                      "IC checksum read failed sum1=%s sum2=%s",
                      esp_err_to_name(sum1_ret),
                      esp_err_to_name(sum2_ret));
    }

    report_status(ctx, CST3530_UPDATE_EVENT_VERIFY, 96, ESP_OK, 0, 0, 0, 0, total_sum,
                  "IC checksum sum1=%08lx sum2=%08lx total=%08lx pass=%d",
                  (unsigned long)chip_sum1,
                  (unsigned long)chip_sum2,
                  (unsigned long)total_sum,
                  ctx->boot_pass ? 1 : 0);
    return total_sum;
}

static esp_err_t read_tp_info(cst3530_update_ctx_t *ctx, uint32_t *fw_ver, uint32_t *chip_type, uint32_t *project_id)
{
    uint8_t buf[60] = {0};
    esp_err_t ret = ESP_FAIL;

    report_status(ctx, CST3530_UPDATE_EVENT_INFO, 98, ESP_OK, 0, 0, 0, 0, 0,
                  "Read TP info: switch to main 0x%02X, reset, read 0xD0030000", CST3530_MAIN_ADDR);
    set_i2c_addr(ctx, CST3530_MAIN_ADDR);
    ESP_RETURN_ON_ERROR(touch_reset(ctx), TAG, "Touch reset failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    for (int retry = 0; retry < 5; ++retry) {
        report_status(ctx, CST3530_UPDATE_EVENT_INFO, 98, ESP_OK, retry + 1, 0, 0, 0, 0,
                      "Read TP info retry=%d/5: wake 0xD0000000 then read 50 bytes", retry + 1);
        ret = wr_reg(ctx, 0xD0000000, 4, NULL, 0);
        ret |= wr_reg(ctx, 0xD0030000, 0x80 | 4, buf, 50);
        if (ret == ESP_OK && buf[3] == 0xCA && buf[2] == 0xCA) {
            report_status(ctx, CST3530_UPDATE_EVENT_INFO, 98, ESP_OK, retry + 1, 0, 0, 0, 0,
                          "Read TP info header OK retry=%d/5 buf[3:2]=%02X %02X", retry + 1, buf[3], buf[2]);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        (void)wr_reg(ctx, 0xD0000400, 4, NULL, 0);
        esp_err_t info_err = ret == ESP_OK ? ESP_FAIL : ret;
        report_status(ctx, CST3530_UPDATE_EVENT_INFO, 98, info_err, retry + 1, 0, 0, 0, 0,
                      "Read TP info retry=%d/5 failed ret=%s header=%02X %02X, sent 0xD0000400",
                      retry + 1,
                      esp_err_to_name(info_err),
                      buf[3],
                      buf[2]);
        ret = ESP_FAIL;
    }

    if (ret != ESP_OK) {
        report_status(ctx, CST3530_UPDATE_EVENT_ERROR, 99, ret, 0, 0, 0, 0, 0, "Read TP info failed");
        return ret;
    }

    if (project_id) {
        *project_id = U8TO32(buf[39], buf[38], buf[37], buf[36]);
    }
    if (chip_type) {
        *chip_type = U8TO32(buf[3], buf[2], buf[1], buf[0]);
    }
    if (fw_ver) {
        *fw_ver = U8TO32(buf[35], buf[34], buf[33], buf[32]);
    }
    report_status(ctx, CST3530_UPDATE_EVENT_INFO, 99, ESP_OK, 0, 0, 0, 0, 0,
                  "TP info project=%08lx chip=%08lx fw=%08lx",
                  (unsigned long)(project_id ? *project_id : 0),
                  (unsigned long)(chip_type ? *chip_type : 0),
                  (unsigned long)(fw_ver ? *fw_ver : 0));
    return ESP_OK;
}

static esp_err_t upgrade_firmware(cst3530_update_ctx_t *ctx, const uint8_t *bin_addr, uint32_t len)
{
    const uint32_t fw_len = CST66XX_BIN_SIZE;
    const int pak_num = (int)(fw_len / CST66XX_PKG_SIZE);
    uint8_t i2c_buf[CST66XX_PKG_SIZE + 10];
    esp_err_t ret = ESP_FAIL;

    if (!bin_addr || len < fw_len + 4U) {
        report_status(ctx, CST3530_UPDATE_EVENT_ERROR, 0, ESP_ERR_INVALID_SIZE, 0, 0, 0, 0, 0,
                      "Firmware length invalid: %lu", (unsigned long)len);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t fw_checksum = U8TO32(bin_addr[fw_len + 3],
                                        bin_addr[fw_len + 2],
                                        bin_addr[fw_len + 1],
                                        bin_addr[fw_len + 0]);
    report_status(ctx, CST3530_UPDATE_EVENT_INFO, 1, ESP_OK, 0, 0, pak_num, fw_checksum, 0,
                  "Target firmware checksum %08lx", (unsigned long)fw_checksum);
    report_status(ctx, CST3530_UPDATE_EVENT_INFO, 1, ESP_OK, 0, 0, pak_num, fw_checksum, 0,
                  "Firmware image payload=%lu trailer=4 total=%lu packet_size=%u packets=%d; trailer is verify target",
                  (unsigned long)fw_len,
                  (unsigned long)len,
                  (unsigned)CST66XX_PKG_SIZE,
                  pak_num);

    const int max_update_attempts = 1;
    for (int retry_fw = 0; retry_fw < max_update_attempts; ++retry_fw) {
        report_status(ctx, CST3530_UPDATE_EVENT_INFO, 2, ESP_OK, retry_fw + 1, 0, pak_num, fw_checksum, 0,
                      "Upgrade attempt %d/%d", retry_fw + 1, max_update_attempts);

        ret = enter_boot(ctx);
        if (ret != ESP_OK) {
            report_status(ctx, CST3530_UPDATE_EVENT_ERROR, 4, ret, retry_fw + 1, 0, pak_num,
                          fw_checksum, 0, "Attempt %d/%d cannot enter boot, stopping",
                          retry_fw + 1, max_update_attempts);
            break;
        }

        report_status(ctx, CST3530_UPDATE_EVENT_ERASE, 10, ESP_OK, retry_fw + 1, 0, pak_num, fw_checksum, 0,
                      "Erase flash 0x0000-0x8000");
        ret = erase_flash(ctx, 0x0000, 0x8000, 0x02);
        if (ret != ESP_OK) {
            report_status(ctx, CST3530_UPDATE_EVENT_ERASE, 10, ret, retry_fw + 1, 0, pak_num,
                          fw_checksum, 0, "Erase 0x0000-0x8000 failed, stopping");
            break;
        }
        report_status(ctx, CST3530_UPDATE_EVENT_ERASE, 14, ESP_OK, retry_fw + 1, 0, pak_num, fw_checksum, 0,
                      "Erase flash 0x8000-0x9000");
        ret = erase_flash(ctx, 0x8000, 0x1000, 0x01);
        if (ret != ESP_OK) {
            report_status(ctx, CST3530_UPDATE_EVENT_ERASE, 14, ret, retry_fw + 1, 0, pak_num,
                          fw_checksum, 0, "Erase 0x8000-0x9000 failed, stopping");
            break;
        }
        report_status(ctx, CST3530_UPDATE_EVENT_ERASE, 18, ESP_OK, retry_fw + 1, 0, pak_num, fw_checksum, 0,
                      "Erase flash 0xB000-0xE000");
        ret = erase_flash(ctx, 0xB000, 0x3000, 0x04);
        if (ret != ESP_OK) {
            report_status(ctx, CST3530_UPDATE_EVENT_ERASE, 18, ret, retry_fw + 1, 0, pak_num,
                          fw_checksum, 0, "Erase 0xB000-0xE000 failed, stopping");
            break;
        }

        uint16_t eep_addr = 0;
        uint16_t eep_len = 0;
        bool pkt_ok = true;

        for (int i = 0; i < pak_num; ++i) {
            const uint16_t write_addr = eep_addr;
            const uint16_t write_offset = eep_len;
            const uint32_t write_type_cmd = i >= 36 ? 0xA00A0300 : 0xA00A0000;
            const uint32_t write_addr_cmd = U8TO32(0xA0, 0x06, eep_addr, eep_addr >> 8);

            report_status(ctx, CST3530_UPDATE_EVENT_WRITE, 20 + (i * 70) / pak_num, ESP_OK,
                          retry_fw + 1, i + 1, pak_num, fw_checksum, 0,
                          "Packet %d/%d setup: payload_off=0x%04X eep_addr=0x%04X type_cmd=0x%08lX",
                          i + 1,
                          pak_num,
                          write_offset,
                          write_addr,
                          (unsigned long)write_type_cmd);

            ret = write_reg_checked(ctx, write_addr_cmd, 4, "Packet EEPROM address");
            if (ret == ESP_OK) ret = write_reg_checked(ctx, 0xA0080004, 4, "Packet write length");
            if (ret == ESP_OK) ret = write_reg_checked(ctx, write_type_cmd, 4, "Packet area type");
            if (ret == ESP_OK) ret = write_reg_checked(ctx, 0xA018CACA, 4, "Packet unlock key");
            if (ret != ESP_OK) {
                report_status(ctx, CST3530_UPDATE_EVENT_WRITE, 20, ret, retry_fw + 1, i + 1, pak_num,
                              fw_checksum, 0, "Packet %d/%d setup failed: %s", i + 1, pak_num, esp_err_to_name(ret));
                pkt_ok = false;
                break;
            }

            i2c_buf[0] = 0xA0;
            i2c_buf[1] = 0x40;
            memcpy(i2c_buf + 2, bin_addr + eep_len, CST66XX_PKG_SIZE);

            ret = i2c_write_raw(ctx, i2c_buf, CST66XX_PKG_SIZE + 2);
            if (ret != ESP_OK) {
                report_status(ctx, CST3530_UPDATE_EVENT_WRITE, 20, ret, retry_fw + 1, i + 1, pak_num,
                              fw_checksum, 0, "Packet %d/%d raw write 0xA040 len=%u failed: %s",
                              i + 1,
                              pak_num,
                              (unsigned)(CST66XX_PKG_SIZE + 2),
                              esp_err_to_name(ret));
                pkt_ok = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            ret = write_reg_checked(ctx, 0xA004E1, 3, "Packet program execute");
            if (ret != ESP_OK) {
                report_status(ctx, CST3530_UPDATE_EVENT_WRITE, 20, ret, retry_fw + 1, i + 1, pak_num,
                              fw_checksum, 0, "Packet %d/%d execute failed: %s",
                              i + 1,
                              pak_num,
                              esp_err_to_name(ret));
                pkt_ok = false;
                break;
            }

            eep_len += CST66XX_PKG_SIZE;
            eep_addr += CST66XX_PKG_SIZE;
            if (eep_addr == 0x9000) {
                eep_addr = 0xB000;
            }

            vTaskDelay(pdMS_TO_TICKS(20));
            uint16_t ready = 0;
            uint16_t polls = 0;
            esp_err_t ready_ret = wait_ready(ctx, 100, 1, 0xA020, 0xE188, &ready, &polls);
            if (ready_ret != ESP_OK) {
                report_status(ctx, CST3530_UPDATE_EVENT_WRITE, 20, ready_ret, retry_fw + 1, i + 1, pak_num,
                              fw_checksum, 0,
                              "Packet %d/%d ready timeout ignored like reference: last=0x%04X polls=%u",
                              i + 1,
                              pak_num,
                              ready,
                              polls);
            }

            const int progress = 20 + ((i + 1) * 70) / pak_num;
            report_status(ctx, CST3530_UPDATE_EVENT_WRITE, progress, ESP_OK, retry_fw + 1, i + 1, pak_num,
                          fw_checksum, 0,
                          "Write packet %d/%d OK: payload_off=0x%04X wrote=1024 next_eep=0x%04X ready=0x%04X polls=%u",
                          i + 1,
                          pak_num,
                          write_offset,
                          eep_addr,
                          ready,
                          polls);
        }

        if (!pkt_ok) {
            report_status(ctx, CST3530_UPDATE_EVENT_ERROR, 20, ESP_FAIL, retry_fw + 1, 0, pak_num,
                          fw_checksum, 0, "Packet write failed, stopping");
            ret = ESP_FAIL;
            break;
        }

        uint32_t ic_checksum = read_ic_checksum(ctx);
        if (fw_checksum == ic_checksum && ctx->boot_pass) {
            report_status(ctx, CST3530_UPDATE_EVENT_VERIFY, 98, ESP_OK, retry_fw + 1, 0, pak_num,
                          fw_checksum, ic_checksum, "Checksum OK fw=%08lx ic=%08lx",
                          (unsigned long)fw_checksum, (unsigned long)ic_checksum);
            ret = ESP_OK;
            break;
        }

        report_status(ctx, CST3530_UPDATE_EVENT_ERROR, 96, ESP_ERR_INVALID_CRC, retry_fw + 1, 0, pak_num,
                      fw_checksum, ic_checksum, "Checksum failed fw=%08lx ic=%08lx",
                      (unsigned long)fw_checksum, (unsigned long)ic_checksum);
        ret = ESP_ERR_INVALID_CRC;
        break;
    }

    report_status(ctx, CST3530_UPDATE_EVENT_INFO, 98, ESP_OK, 0, 0, pak_num, fw_checksum, 0,
                  "Switch back to main addr 0x%02X, reset and settle 50ms", CST3530_MAIN_ADDR);
    set_i2c_addr(ctx, CST3530_MAIN_ADDR);
    (void)touch_reset(ctx);
    vTaskDelay(pdMS_TO_TICKS(50));

    if (ret == ESP_OK) {
        uint32_t ver = 0;
        uint32_t chip = 0;
        uint32_t proj = 0;
        (void)read_tp_info(ctx, &ver, &chip, &proj);
        report_status(ctx, CST3530_UPDATE_EVENT_DONE, 100, ESP_OK, 0, pak_num, pak_num, fw_checksum, 0,
                      "Firmware update complete: project=%08lx chip=%08lx fw=%08lx checksum=%08lx",
                      (unsigned long)proj,
                      (unsigned long)chip,
                      (unsigned long)ver,
                      (unsigned long)fw_checksum);
    } else {
        report_status(ctx, CST3530_UPDATE_EVENT_ERROR, 100, ret, 0, 0, pak_num, fw_checksum, 0,
                      "Firmware update failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t esp_lcd_touch_cst3530_update_firmware(const cst3530_update_config_t *config,
                                                const uint8_t *firmware,
                                                uint32_t firmware_len)
{
    ESP_RETURN_ON_FALSE(config && config->bus && config->reset_cb, ESP_ERR_INVALID_ARG, TAG, "Invalid config");
    ESP_RETURN_ON_FALSE(firmware && firmware_len > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid firmware");

    cst3530_update_ctx_t ctx = {
        .config = *config,
    };
    if (ctx.config.i2c_speed_hz == 0) {
        ctx.config.i2c_speed_hz = 200000;
    }

    report_status(&ctx, CST3530_UPDATE_EVENT_INFO, 0, ESP_OK, 0, 0, 0, 0, 0,
                  "Updater start: bus=%p speed=%lu main=0x%02X boot=0x%02X",
                  (void *)ctx.config.bus,
                  (unsigned long)ctx.config.i2c_speed_hz,
                  CST3530_MAIN_ADDR,
                  CST3530_BOOT_ADDR);

    esp_err_t ret = add_i2c_device(ctx.config.bus, CST3530_MAIN_ADDR, ctx.config.i2c_speed_hz, &ctx.main_dev);
    if (ret != ESP_OK) {
        report_status(&ctx, CST3530_UPDATE_EVENT_ERROR, 0, ret, 0, 0, 0, 0, 0,
                      "Add main I2C device failed: %s", esp_err_to_name(ret));
        goto done;
    }
    report_status(&ctx, CST3530_UPDATE_EVENT_INFO, 0, ESP_OK, 0, 0, 0, 0, 0,
                  "Main I2C device added: addr=0x%02X speed=%lu", CST3530_MAIN_ADDR,
                  (unsigned long)ctx.config.i2c_speed_hz);

    ret = add_i2c_device(ctx.config.bus, CST3530_BOOT_ADDR, ctx.config.i2c_speed_hz, &ctx.boot_dev);
    if (ret != ESP_OK) {
        report_status(&ctx, CST3530_UPDATE_EVENT_ERROR, 0, ret, 0, 0, 0, 0, 0,
                      "Add boot I2C device failed: %s", esp_err_to_name(ret));
        goto done;
    }
    report_status(&ctx, CST3530_UPDATE_EVENT_INFO, 0, ESP_OK, 0, 0, 0, 0, 0,
                  "Boot I2C device added: addr=0x%02X speed=%lu", CST3530_BOOT_ADDR,
                  (unsigned long)ctx.config.i2c_speed_hz);

    set_i2c_addr(&ctx, CST3530_MAIN_ADDR);
    ret = upgrade_firmware(&ctx, firmware, firmware_len);

done:
    if (ctx.boot_dev) {
        (void)i2c_master_bus_rm_device(ctx.boot_dev);
    }
    if (ctx.main_dev) {
        (void)i2c_master_bus_rm_device(ctx.main_dev);
    }
    return ret;
}

esp_err_t esp_lcd_touch_cst3530_update_default(const cst3530_update_config_t *config)
{
    return esp_lcd_touch_cst3530_update_firmware(config, cst3530_fw_bin, sizeof(cst3530_fw_bin));
}

esp_err_t esp_lcd_touch_cst3530_read_ic_checksum(const cst3530_update_config_t *config, uint32_t *out_checksum)
{
    ESP_RETURN_ON_FALSE(config && config->bus && config->reset_cb && out_checksum,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid checksum config");

    cst3530_update_ctx_t ctx = {
        .config = *config,
    };
    if (ctx.config.i2c_speed_hz == 0) {
        ctx.config.i2c_speed_hz = 200000;
    }

    *out_checksum = 0;
    report_status(&ctx, CST3530_UPDATE_EVENT_VERIFY, 0, ESP_OK, 0, 0, 0, 0, 0,
                  "IC checksum read start: bus=%p speed=%lu main=0x%02X boot=0x%02X",
                  (void *)ctx.config.bus,
                  (unsigned long)ctx.config.i2c_speed_hz,
                  CST3530_MAIN_ADDR,
                  CST3530_BOOT_ADDR);

    esp_err_t ret = add_i2c_device(ctx.config.bus, CST3530_MAIN_ADDR, ctx.config.i2c_speed_hz, &ctx.main_dev);
    if (ret != ESP_OK) {
        report_status(&ctx, CST3530_UPDATE_EVENT_ERROR, 0, ret, 0, 0, 0, 0, 0,
                      "Add main I2C device failed: %s", esp_err_to_name(ret));
        goto done;
    }

    ret = add_i2c_device(ctx.config.bus, CST3530_BOOT_ADDR, ctx.config.i2c_speed_hz, &ctx.boot_dev);
    if (ret != ESP_OK) {
        report_status(&ctx, CST3530_UPDATE_EVENT_ERROR, 0, ret, 0, 0, 0, 0, 0,
                      "Add boot I2C device failed: %s", esp_err_to_name(ret));
        goto done;
    }

    ret = enter_boot_with_retries(&ctx, 3);
    if (ret != ESP_OK) {
        goto done;
    }

    uint32_t ic_checksum = read_ic_checksum(&ctx);
    if (!ctx.boot_pass) {
        ret = ESP_FAIL;
        report_status(&ctx, CST3530_UPDATE_EVENT_ERROR, 96, ret, 0, 0, 0, 0, ic_checksum,
                      "IC checksum read did not pass");
        goto done;
    }

    *out_checksum = ic_checksum;
    report_status(&ctx, CST3530_UPDATE_EVENT_VERIFY, 100, ESP_OK, 0, 0, 0, 0, ic_checksum,
                  "IC checksum read complete: 0x%08lx", (unsigned long)ic_checksum);

done:
    set_i2c_addr(&ctx, CST3530_MAIN_ADDR);
    if (ctx.config.reset_cb) {
        (void)touch_reset(&ctx);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (ctx.boot_dev) {
        (void)i2c_master_bus_rm_device(ctx.boot_dev);
    }
    if (ctx.main_dev) {
        (void)i2c_master_bus_rm_device(ctx.main_dev);
    }
    return ret;
}
