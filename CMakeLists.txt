cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0079)
    cmake_policy(SET CMP0079 NEW)
endif()

# The vendored Wi-Fi Remote component uses this value to select its Kconfig.
set(ENV{ESP_IDF_VERSION} "6.1")

# This project embeds every enabled audio asset in the factory application
# image. Force the list empty so an existing build cache cannot retain an older
# file-backed audio configuration.
set(OTOOL_AUDIO_EXTERNAL_FILES ""
    CACHE INTERNAL "No project-managed external audio files" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m5corep4x_user_demo)

if(NOT IDF_VERSION_MAJOR EQUAL 6 OR NOT IDF_VERSION_MINOR EQUAL 1)
    message(FATAL_ERROR
        "M5CoreP4X UserDemo requires ESP-IDF 6.1; found ${IDF_VERSION} at $ENV{IDF_PATH}")
endif()

# Required dependency fixes for the vendored components under ESP-IDF 6.1.
idf_component_get_property(esp_dsp_lib espressif__esp-dsp COMPONENT_LIB)
if(TARGET ${esp_dsp_lib})
    target_compile_options(${esp_dsp_lib} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-include>"
        "$<$<COMPILE_LANGUAGE:CXX>:cmath>")
endif()

idf_component_get_property(i2c_bus_lib espressif__i2c_bus COMPONENT_LIB)
if(TARGET ${i2c_bus_lib} AND TARGET idf::esp_driver_gpio)
    target_link_libraries(${i2c_bus_lib} PRIVATE idf::esp_driver_gpio)
endif()

idf_component_get_property(esp_hosted_mcu_lib esp-hosted-mcu COMPONENT_LIB)
if(TARGET ${esp_hosted_mcu_lib} AND TARGET idf::esp_driver_sdmmc AND TARGET idf::esp_driver_dma AND TARGET idf::esp_driver_gpio)
    target_link_libraries(${esp_hosted_mcu_lib} PRIVATE
        idf::esp_driver_sdmmc
        idf::esp_driver_dma
        idf::esp_driver_gpio)
endif()
