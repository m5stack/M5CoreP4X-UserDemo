/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "p4_ethernet_init.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_phy_ip101.h"
#include "driver/gpio.h"

#define INTERNAL_ETHERNETS_NUM      1

static const char *TAG = "p4_eth_init";


/**
 * @brief Internal ESP32 Ethernet initialization
 *
 * @param[out] mac_out optionally returns Ethernet MAC object
 * @param[out] phy_out optionally returns Ethernet PHY object
 * @return
 *          - esp_eth_handle_t if init succeeded
 *          - NULL if init failed
 */
static esp_eth_handle_t eth_init_internal(esp_eth_mac_t **mac_out, esp_eth_phy_t **phy_out)
{
    esp_eth_handle_t ret = NULL;

    // Init common MAC and PHY configs to default
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    // Update PHY config based on board specific configuration
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = 52;  // GPIO_NUM_52
    // Init vendor specific MAC config to default
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    // Update vendor specific MAC config based on board configuration
    esp32_emac_config.smi_gpio.mdc_num = 31;   // GPIO_NUM_31
    esp32_emac_config.smi_gpio.mdio_num = 36;  // GPIO_NUM_36

    // Set RMII PIN
    esp32_emac_config.clock_config.rmii.clock_gpio = 32;        // GPIO_NUM_32
    esp32_emac_config.emac_dataif_gpio.rmii.tx_en_num = 33;     // GPIO_NUM_33
    esp32_emac_config.emac_dataif_gpio.rmii.txd0_num = 41;      // GPIO_NUM_41
    esp32_emac_config.emac_dataif_gpio.rmii.txd1_num = 35;      // GPIO_NUM_35
    esp32_emac_config.emac_dataif_gpio.rmii.crs_dv_num = 28;    // GPIO_NUM_28
    esp32_emac_config.emac_dataif_gpio.rmii.rxd0_num = 29;      // GPIO_NUM_29
    esp32_emac_config.emac_dataif_gpio.rmii.rxd1_num = 30;      // GPIO_NUM_30

    // Create new ESP32 Ethernet MAC instance
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    // Create new IP101 PHY instance
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    // Init Ethernet driver to default and install it
    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_GOTO_ON_FALSE(esp_eth_driver_install(&config, &eth_handle) == ESP_OK, NULL,
                        err, TAG, "Ethernet driver install failed");

    if (mac_out != NULL) {
        *mac_out = mac;
    }
    if (phy_out != NULL) {
        *phy_out = phy;
    }
    return eth_handle;
err:
    if (eth_handle != NULL) {
        esp_eth_driver_uninstall(eth_handle);
    }
    if (mac != NULL) {
        mac->del(mac);
    }
    if (phy != NULL) {
        phy->del(phy);
    }
    return ret;
}


esp_err_t p4_eth_init(esp_eth_handle_t *eth_handles_out[], uint8_t *eth_cnt_out)
{
    esp_err_t ret = ESP_OK;
    esp_eth_handle_t *eth_handles = NULL;
    uint8_t eth_cnt = 0;

    ESP_GOTO_ON_FALSE(eth_handles_out != NULL && eth_cnt_out != NULL, ESP_ERR_INVALID_ARG,
                        err, TAG, "invalid arguments: initialized handles array or number of interfaces");
    eth_handles = calloc(INTERNAL_ETHERNETS_NUM, sizeof(esp_eth_handle_t));
    ESP_GOTO_ON_FALSE(eth_handles != NULL, ESP_ERR_NO_MEM, err, TAG, "no memory");

    eth_handles[eth_cnt] = eth_init_internal(NULL, NULL);
    ESP_GOTO_ON_FALSE(eth_handles[eth_cnt], ESP_FAIL, err, TAG, "internal Ethernet init failed");
    eth_cnt++;

    *eth_handles_out = eth_handles;
    *eth_cnt_out = eth_cnt;
    return ESP_OK;

err:
    if (eth_handles) {
        free(eth_handles);
    }
    return ret;
}

esp_err_t p4_eth_deinit(esp_eth_handle_t *eth_handles, uint8_t eth_cnt)
{
    ESP_RETURN_ON_FALSE(eth_handles != NULL, ESP_ERR_INVALID_ARG, TAG, "array of Ethernet handles cannot be NULL");
    for (int i = 0; i < eth_cnt; i++) {
        esp_eth_mac_t *mac = NULL;
        esp_eth_phy_t *phy = NULL;
        if (eth_handles[i] != NULL) {
            esp_eth_get_mac_instance(eth_handles[i], &mac);
            esp_eth_get_phy_instance(eth_handles[i], &phy);
            ESP_RETURN_ON_ERROR(esp_eth_driver_uninstall(eth_handles[i]), TAG, "Ethernet %p uninstall failed", eth_handles[i]);
        }
        if (mac != NULL) {
            mac->del(mac);
        }
        if (phy != NULL) {
            phy->del(phy);
        }
    }
    free(eth_handles);
    return ESP_OK;
}
