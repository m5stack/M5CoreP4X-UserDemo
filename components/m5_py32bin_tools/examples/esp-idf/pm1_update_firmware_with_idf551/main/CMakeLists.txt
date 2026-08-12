idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES m5_py32bin_tools esp_driver_usb_serial_jtag i2c_bus
                    )