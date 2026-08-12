#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CST3530_UPDATE_MATCH_CHIP_ID    0xCACA220EU
#define CST3530_UPDATE_MATCH_FW_VERSION 0x01010000U

typedef enum {
    CST3530_UPDATE_EVENT_INFO,
    CST3530_UPDATE_EVENT_ENTER_BOOT,
    CST3530_UPDATE_EVENT_ERASE,
    CST3530_UPDATE_EVENT_WRITE,
    CST3530_UPDATE_EVENT_VERIFY,
    CST3530_UPDATE_EVENT_DONE,
    CST3530_UPDATE_EVENT_ERROR,
} cst3530_update_event_t;

typedef struct {
    cst3530_update_event_t event;
    int progress;              // 0..100
    esp_err_t err;
    int attempt;
    int packet_index;
    int packet_count;
    uint32_t fw_checksum;
    uint32_t ic_checksum;
    char message[192];
} cst3530_update_status_t;

typedef void (*cst3530_update_status_cb_t)(const cst3530_update_status_t *status, void *user_ctx);
typedef esp_err_t (*cst3530_update_reset_cb_t)(void *user_ctx);

typedef struct {
    i2c_master_bus_handle_t bus;
    uint32_t i2c_speed_hz;
    cst3530_update_reset_cb_t reset_cb;
    void *reset_user_ctx;
    cst3530_update_status_cb_t status_cb;
    void *status_user_ctx;
} cst3530_update_config_t;

esp_err_t esp_lcd_touch_cst3530_update_default(const cst3530_update_config_t *config);
esp_err_t esp_lcd_touch_cst3530_update_firmware(const cst3530_update_config_t *config,
                                                const uint8_t *firmware,
                                                uint32_t firmware_len);
esp_err_t esp_lcd_touch_cst3530_read_ic_checksum(const cst3530_update_config_t *config, uint32_t *out_checksum);

#ifdef __cplusplus
}
#endif
