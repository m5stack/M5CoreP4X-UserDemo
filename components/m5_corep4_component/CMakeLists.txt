idf_component_register(SRCS "m5_corep4_component.cpp" "m5_corep4_usb.cpp" "m5_corep4_ui_sync.cpp" "m5_corep4_status.cpp"
                       "corep4_lvgl_ui_sync/ui_sync_common.cpp"
                       "corep4_lvgl_ui_sync/ui_usb_phy_guard.cpp"
                       "corep4_lvgl_ui_sync/ui_mian_sync.cpp"
                       "corep4_lvgl_ui_sync/ui_touchtest_sync.cpp"
                       "corep4_lvgl_ui_sync/ui_function_sync.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_sleep_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_off_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_power_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_touch_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_sdcard_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_i2cscan_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_imu_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_imu_attitude.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_imu_renderer.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_rec_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_ping_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_timer_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_wifi_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_usb_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_irtxrx_window.cpp"
                       "corep4_lvgl_ui_sync/ui_function_windows/ui_function_music_window.cpp"
                       "corep4_lvgl_ui_sync/ui_power_sync.cpp"
                       "corep4_lvgl_ui_sync/ui_sleep_sync.cpp"
                       "corep4_lvgl_ui_sync/ui_sd_sync.cpp"
                       "corep4_lvgl_ui_sync/ui_eth_sync.cpp"
                       REQUIRES M5PM1 otool_esp_component M5IOE1
                       REQUIRES esp_lcd_touch esp_lcd_touch_cst3xx esp_lcd_st7102 esp_lvgl_port esp_netif esp_wifi esp_wifi_remote esp-hosted-mcu nvs_flash lwip spiffs efuse esp_timer
                       REQUIRES corep4_lvgl_ui_out
                       REQUIRES usb usb_host_hid  usb_host_msc esp_driver_jpeg
                       INCLUDE_DIRS "."
                       EMBED_FILES
                       "corep4_lvgl_ui_sync/ui_function_windows/assets/corep4x_front.rgb565"
                       "corep4_lvgl_ui_sync/ui_function_windows/assets/corep4x_back.rgb565"
                       "corep4_lvgl_ui_sync/ui_function_windows/assets/corep4x_top.rgb565"
                       "corep4_lvgl_ui_sync/ui_function_windows/assets/corep4x_bottom.rgb565"
                       "corep4_lvgl_ui_sync/ui_function_windows/assets/corep4x_left.rgb565"
                       "corep4_lvgl_ui_sync/ui_function_windows/assets/corep4x_right.rgb565")

# SquareLine 生成的 C 文件通过符号名回调到本组件，静态库链接时需要保留这些回调对象。
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
