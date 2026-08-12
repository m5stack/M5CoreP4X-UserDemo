idf_component_register(SRCS "p4_ethernet_init.c"
                            "esp_eth_phy_ip101_compat.c"
                       REQUIRES esp_driver_gpio esp_eth esp_netif
                       INCLUDE_DIRS ".")
