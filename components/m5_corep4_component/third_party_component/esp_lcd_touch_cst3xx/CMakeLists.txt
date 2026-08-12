idf_component_register(SRCS "esp_lcd_touch_cst3xx.c"
                            "cst3530_update/esp_lcd_touch_cst3530_update.c"
                       INCLUDE_DIRS "include" "cst3530_update/include"
                       PRIV_INCLUDE_DIRS "cst3530_update"
                       REQUIRES "esp_lcd" "esp_driver_i2c")
