# M5CoreP4X UserDemo

M5CoreP4X UserDemo is the preinstalled ESP-IDF demonstration firmware for the M5Stack M5CoreP4X. It presents the complete device UI and demonstrates board-level capabilities including power control, display, touch, audio, RTC, IMU, microSD, Ethernet, ESP-Hosted Wi-Fi, USB Host, infrared, and Grove I2C.

## Hardware and Environment

- M5Stack M5CoreP4X with ESP32-P4, 16 MiB flash, and PSRAM
- M5Stack A172 Stamp-AddOn C6 For P4 module with compatible ESP-Hosted slave firmware (required for Wi-Fi)
- ESP-IDF v6.1
- Optional microSD card, Ethernet connection, USB devices, and Grove I2C peripherals

## Demonstrated Features

After startup, the device opens the LVGL Startup screen. Users can swipe between the main status, Info, and Function screens. The Function screen provides interactive pages for power control, touch, microSD, I2C scanning, IMU, recording, network ping, Wi-Fi, USB, infrared transmit and receive, music, sleep, and power-off operations.

The startup path initializes board power, RTC, audio, Ethernet, LCD, and touch. Ethernet connection waiting, the startup sound, and time-consuming page operations run in background tasks. The absence of an optional peripheral does not prevent the remaining demonstration pages from starting.

## Build

```bash
source esp-idf/export.sh
idf.py -B build set-target esp32p4
idf.py -B build build
```

The application image is generated at:

```text
build/m5corep4x_user_demo.bin
```

## Flash and Monitor

Connect the download/debug USB port and replace the serial port with the one assigned by the host:

```bash
idf.py -B build -p /dev/ttyACM0 flash monitor
```

## Repository Layout

- `main/`: application entry point, startup initialization, and firmware metadata
- `components/m5_corep4_component/`: M5CoreP4X board support, LVGL UI, and function pages
- `components/otool_esp_component/`: shared peripheral and audio utilities
- `sdkconfig.defaults`: ESP32-P4, flash, PSRAM, network, and component configuration baseline
- `partitions.csv`: application, NVS, PHY, and SPIFFS partition definitions
- `THIRD_PARTY_SOURCES.md`: sources and pinned versions for bundled third-party code
- `USER_DEMO_CODE_REVIEW_CN.md`: Chinese engineering review produced using the User Demo review standard

## Known Boundaries

- A successful build does not replace hardware validation. Touch, audio, wireless, networking, and repeated page-entry stability must still be tested on an M5CoreP4X before release.
- Wi-Fi requires the A172 Stamp-AddOn C6 For P4 module. It uses ESP-Hosted communication between the module's ESP32-C6 and the ESP32-P4; when Wi-Fi fails, first verify that the module is installed, then check its slave firmware, power rails, and SDIO connection.
- The microSD card uses SPI mode to avoid contention with the SDMMC resources used by ESP-Hosted.

## License

See [LICENSE](LICENSE) for the project license. Bundled third-party components remain subject to their respective licenses; see [THIRD_PARTY_SOURCES.md](THIRD_PARTY_SOURCES.md) for source details.
