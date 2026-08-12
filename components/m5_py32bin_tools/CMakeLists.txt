idf_component_register(
    SRCS
        "src/m5_py32bin_bus.cpp"
        "src/devices/pm1/py32_pm1_dev.cpp"
        "src/devices/ioe1/py32_ioe1_dev.cpp"
    INCLUDE_DIRS "src"
    REQUIRES
        driver
        esp_common
        esp_timer
        esp_system
        esp_driver_spi
        esp_driver_uart
        mbedtls
        espressif__i2c_bus
)

set(COMPONENT_VERSION "0.1.0")

target_compile_definitions(${COMPONENT_LIB} PUBLIC M5_PY32BIN_COMPONENT_VERSION="${COMPONENT_VERSION}")
