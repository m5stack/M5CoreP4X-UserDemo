#include "devices/pm1/py32_pm1_dev.h"

 #include <algorithm>
 #include <array>
 #include <cstring>
 #include <string>
 #include <vector>
 
 #include "esp_log.h"
 #include "esp_timer.h"

#include "esp_rom_md5.h"

#if defined(ARDUINO)
#include <Arduino.h>
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace m5::py32bin::devices {

namespace {
constexpr char TAG[] = "py32-pm1";
constexpr uint32_t REG_UID_LOW = 0x00;
constexpr uint32_t REG_UID_HIGH = 0x01;
constexpr uint32_t REG_HW_REV = 0x02;
constexpr uint32_t REG_SW_REV = 0x03;
constexpr size_t IAP_HEADER_SIZE = 8;
constexpr uint8_t IAP_PADDING_BYTE = 0xFF;
constexpr size_t MD5_ASCII_LENGTH = 32;
constexpr uint8_t DOWNLOAD_REGISTER = 0xFD;
constexpr uint8_t DOWNLOAD_KEY = 0x1A;
constexpr uint8_t BOOTLOADER_ADDRESS = 0x54;
constexpr uint32_t FLASH_BASE_ADDRESS = 0x08001000;
constexpr uint32_t DOWNLOAD_TRIGGER_DELAY_MS = 200;
constexpr uint32_t BOOTLOADER_WAIT_TIMEOUT_MS = 30000;
constexpr uint32_t BOOTLOADER_POLL_INTERVAL_MS = 100;
constexpr uint32_t JUMP_RELEASE_TIMEOUT_MS = 5000;

void delay_ms(uint32_t ms) {
    if (ms == 0) {
        return;
    }
#if defined(ARDUINO)
    delay(ms);
#else
    vTaskDelay(pdMS_TO_TICKS(ms));
#endif
}

uint64_t runtime_millis() {
#if defined(ARDUINO)
    return static_cast<uint64_t>(millis());
#else
    return esp_timer_get_time() / 1000ULL;
#endif
}

bool is_hex_digit(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

uint8_t hex_value(uint8_t c) {
    if (c >= '0' && c <= '9') {
        return static_cast<uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<uint8_t>(10 + (c - 'a'));
    }
    return static_cast<uint8_t>(10 + (c - 'A'));
}

esp_err_t compute_md5(const uint8_t *data, size_t length, std::array<uint8_t, 16> &digest) {
    if (data == nullptr || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, data, static_cast<uint32_t>(length));
    esp_rom_md5_final(digest.data(), &ctx);
    return ESP_OK;
}

esp_err_t capture_serial_stream(BusAdapter &serial,
                                const Py32Pm1SerialInputOptions &options,
                                std::vector<uint8_t> &raw_buffer) {
    if (!serial.ready()) {
        ESP_LOGE(TAG, "input serial adapter not ready");
        return ESP_ERR_INVALID_STATE;
    }
    raw_buffer.clear();
    raw_buffer.reserve(std::min(options.max_input_size, std::max<size_t>(options.max_input_size / 4, 1024)));

    uint64_t last_rx = runtime_millis();
    bool received_any = false;

    ESP_LOGI(TAG, "[capture] Starting capture (max_size=%zu, per_byte_timeout=%u, idle_timeout=%u)",
             options.max_input_size,
             static_cast<unsigned>(options.per_byte_timeout_ms),
             static_cast<unsigned>(options.idle_timeout_ms));

    while (raw_buffer.size() < options.max_input_size) {
        // 逐字节读取可以避免 USB-JTAG 回调在部分读取时丢弃数据
        uint8_t byte = 0;
        esp_err_t err = serial.readRaw(&byte, 1, options.per_byte_timeout_ms);

        if (err == ESP_OK) {
            raw_buffer.push_back(byte);
            last_rx = runtime_millis();
            received_any = true;

            // 减少日志频率：每 4KB 打印一次，避免阻塞 USB-CDC
            if (raw_buffer.size() % 4096 == 0) {
                ESP_LOGI(TAG, "[capture] Received %zu bytes...", raw_buffer.size());
            }
            continue;
        }

        if (err == ESP_ERR_TIMEOUT) {
            if (!received_any) {
                // 还没开始接收数据，继续等待
                continue;
            }

            uint64_t idle_ms = runtime_millis() - last_rx;
            if (idle_ms >= options.idle_timeout_ms) {
                ESP_LOGI(TAG, "[capture] Data stream idle for %llu ms, capture complete", idle_ms);
                break;
            }
            continue;
        }

        ESP_LOGE(TAG, "serial read failed: %s", esp_err_to_name(err));
        return err;
    }

    if (raw_buffer.empty()) {
        ESP_LOGE(TAG, "no data captured from serial input");
        return ESP_ERR_INVALID_SIZE;
    }

    if (raw_buffer.size() >= options.max_input_size) {
        ESP_LOGW(TAG, "input truncated at max_input_size=%zu", options.max_input_size);
    }

    ESP_LOGI(TAG, "[capture] Capture complete: %zu bytes received", raw_buffer.size());
    delay_ms(options.post_receive_delay_ms);
    return ESP_OK;
}

esp_err_t parse_md5_header(const std::vector<uint8_t> &raw,
                           const Py32Pm1SerialInputOptions &options,
                           std::array<uint8_t, 16> &md5_out,
                           size_t &payload_offset) {
    size_t offset = 0;
    if (options.expect_md5_prefix) {
        if (options.md5_ascii) {
            size_t digits = 0;
            std::array<uint8_t, 16> tmp = {0};
            while (offset < raw.size() && digits < MD5_ASCII_LENGTH) {
                uint8_t c = raw[offset++];
                if (c == '\r' || c == '\n' || c == ' ') {
                    continue;
                }
                if (!is_hex_digit(c)) {
                    ESP_LOGE(TAG, "non-hex character detected while parsing MD5");
                    return ESP_ERR_INVALID_ARG;
                }
                uint8_t nibble = hex_value(c);
                size_t byte_index = digits / 2;
                if ((digits & 1U) == 0U) {
                    tmp[byte_index] = static_cast<uint8_t>(nibble << 4);
                } else {
                    tmp[byte_index] = static_cast<uint8_t>(tmp[byte_index] | nibble);
                }
                ++digits;
            }
            if (digits != MD5_ASCII_LENGTH) {
                ESP_LOGE(TAG, "expected %zu hex digits for MD5, got %zu", MD5_ASCII_LENGTH, digits);
                return ESP_ERR_INVALID_SIZE;
            }
            md5_out = tmp;
            while (offset < raw.size() && (raw[offset] == '\r' || raw[offset] == '\n')) {
                ++offset;
            }
        } else {
            if (raw.size() < 16) {
                ESP_LOGE(TAG, "payload shorter than binary MD5 header");
                return ESP_ERR_INVALID_SIZE;
            }
            std::copy(raw.begin(), raw.begin() + 16, md5_out.begin());
            offset = 16;
        }
    }
    payload_offset = offset;
    return ESP_OK;
}

esp_err_t parse_md5_and_firmware(const std::vector<uint8_t> &raw,
                                 const Py32Pm1SerialInputOptions &options,
                                 std::array<uint8_t, 16> &md5_out,
                                 std::vector<uint8_t> &firmware_out) {
    size_t offset = 0;
    esp_err_t err = parse_md5_header(raw, options, md5_out, offset);
    if (err != ESP_OK) {
        return err;
    }
    if (offset >= raw.size()) {
        ESP_LOGE(TAG, "no firmware data present after MD5 prefix");
        return ESP_ERR_INVALID_SIZE;
    }
    firmware_out.assign(raw.begin() + offset, raw.end());
    return ESP_OK;
}

std::string md5_to_hex(const std::array<uint8_t, 16> &digest) {
    static constexpr char HEX_CHARS[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (uint8_t b : digest) {
        out.push_back(HEX_CHARS[(b >> 4) & 0x0F]);
        out.push_back(HEX_CHARS[b & 0x0F]);
    }
    return out;
}

void send_uart_prompt(BusAdapter &serial) {
    static constexpr char kLine1[] = "[iap] PM1 bridge ready (Serial/Context adapter)\r\n";
    static constexpr char kLine2[] = "[iap] Send 32 hex MD5 + newline + firmware binary (no XMODEM).\r\n";
    static constexpr char kLine3[] = "[iap] Example: python send_pm1_fw.py --port COMx --md5 <digest> --file fw.bin\r\n";
    
    ESP_LOGI(TAG, "[iap] sending UART prompt (line by line with delays)...");
    serial.writeRaw(reinterpret_cast<const uint8_t *>(kLine1), sizeof(kLine1) - 1, 500);
    delay_ms(50);
    serial.writeRaw(reinterpret_cast<const uint8_t *>(kLine2), sizeof(kLine2) - 1, 500);
    delay_ms(50);
    serial.writeRaw(reinterpret_cast<const uint8_t *>(kLine3), sizeof(kLine3) - 1, 500);
    delay_ms(50);
    ESP_LOGI(TAG, "[iap] UART prompt sent");
}

#if !defined(ARDUINO)
esp_err_t trigger_download_mode(i2c_bus_device_handle_t pm1_dev) {
    if (pm1_dev == nullptr) {
        ESP_LOGE(TAG, "pm1 i2c device handle is null");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = i2c_bus_write_byte(pm1_dev, DOWNLOAD_REGISTER, DOWNLOAD_KEY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to request download mode: %s", esp_err_to_name(err));
        return err;
    }
    delay_ms(DOWNLOAD_TRIGGER_DELAY_MS);
    return ESP_OK;
}

esp_err_t ping_bootloader(i2c_bus_device_handle_t boot_dev) {
    if (boot_dev == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t dummy = 0;
    return i2c_bus_read_byte(boot_dev, NULL_I2C_MEM_ADDR, &dummy);
}

esp_err_t wait_for_bootloader_ready(i2c_bus_device_handle_t boot_dev, uint32_t timeout_ms) {
    const uint64_t deadline = runtime_millis() + timeout_ms;
    while (runtime_millis() < deadline) {
        esp_err_t err = ping_bootloader(boot_dev);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        delay_ms(BOOTLOADER_POLL_INTERVAL_MS);
    }
    ESP_LOGE(TAG, "bootloader 0x%02X did not respond", i2c_bus_device_get_address(boot_dev));
    return ESP_ERR_TIMEOUT;
}

esp_err_t stream_firmware_over_i2c(const uint8_t *data,
                                   size_t length,
                                   const Py32Pm1IapOptions &options,
                                   i2c_bus_device_handle_t boot_dev) {
    if (data == nullptr || length == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (boot_dev == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (options.chunk_size == 0 || options.chunk_size > 255) {
        ESP_LOGE(TAG, "invalid chunk size %u", static_cast<unsigned>(options.chunk_size));
        return ESP_ERR_INVALID_ARG;
    }

    std::vector<uint8_t> frame(IAP_HEADER_SIZE + options.chunk_size, 0x00);
    const size_t total_packets = (length + options.chunk_size - 1) / options.chunk_size;
    uint32_t address = FLASH_BASE_ADDRESS;
    size_t offset = 0;
    size_t packet_index = 0;

    bool notified_complete = false;
    while (offset < length) {
        const size_t chunk = std::min(static_cast<size_t>(options.chunk_size), length - offset);
        frame[0] = options.write_opcode;
        frame[1] = static_cast<uint8_t>((address >> 24) & 0xFF);
        frame[2] = static_cast<uint8_t>((address >> 16) & 0xFF);
        frame[3] = static_cast<uint8_t>((address >> 8) & 0xFF);
        frame[4] = static_cast<uint8_t>(address & 0xFF);
        frame[5] = 0x00;
        frame[6] = static_cast<uint8_t>(chunk & 0xFF);
        frame[7] = IAP_PADDING_BYTE;
        std::memcpy(frame.data() + IAP_HEADER_SIZE, data + offset, chunk);

        // 在发送前等待 Bootloader 就绪 (Flash 操作可能正在进行)
        uint32_t wait_count = 0;
        const uint32_t max_wait = 50; // 最多等待 50 * 100ms = 5秒
        while (ping_bootloader(boot_dev) != ESP_OK) {
            delay_ms(100);
            if (++wait_count >= max_wait) {
                ESP_LOGE(TAG, "Bootloader not responding before write at 0x%08lX", 
                         static_cast<unsigned long>(address));
                return ESP_ERR_TIMEOUT;
            }
        }

        esp_err_t err = i2c_bus_write_bytes(boot_dev, NULL_I2C_MEM_ADDR, IAP_HEADER_SIZE + chunk, frame.data());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c write failed at 0x%08lX: %s",
                     static_cast<unsigned long>(address), esp_err_to_name(err));
            return err;
        }

        offset += chunk;
        address += static_cast<uint32_t>(chunk);
        ++packet_index;

        float percent = 100.0f * static_cast<float>(packet_index) / static_cast<float>(total_packets);
        ESP_LOGI(TAG, "[iap] chunk %03zu/%03zu (%.2f%%)", packet_index, total_packets, percent);

        if (!notified_complete && percent >= 100.0f) {
            ESP_LOGI(TAG, "[iap] firmware refresh complete; waiting for device reboot...");
            notified_complete = true;
        }

        if (options.chunk_delay_ms > 0) {
            delay_ms(options.chunk_delay_ms);
        }
    }

    return ESP_OK;
}

esp_err_t jump_to_application(i2c_bus_device_handle_t boot_dev, uint8_t opcode) {
    if (boot_dev == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Sending jump command (0x%02X)...", opcode);
    esp_err_t err = i2c_bus_write_bytes(boot_dev, NULL_I2C_MEM_ADDR, 1, &opcode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to send jump opcode: %s", esp_err_to_name(err));
        return err;
    }

    const uint64_t deadline = runtime_millis() + JUMP_RELEASE_TIMEOUT_MS;
    while (runtime_millis() < deadline) {
        if (ping_bootloader(boot_dev) != ESP_OK) {
            ESP_LOGI(TAG, "bootloader released bus, application should be running");
            return ESP_OK;
        }
        
        // Retry sending jump command
        ESP_LOGD(TAG, "Bootloader still active, retrying jump command...");
        i2c_bus_write_bytes(boot_dev, NULL_I2C_MEM_ADDR, 1, &opcode);
        
        delay_ms(200);
    }

    ESP_LOGW(TAG, "bootloader still responding after jump command");
    return ESP_ERR_TIMEOUT;
}
#endif
}  // namespace

esp_err_t Py32Pm1Device::detect() {
    std::array<uint8_t, 4> id_bytes = {0};
    esp_err_t err = readRegister(REG_UID_LOW, id_bytes.data(), id_bytes.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read UID: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t hw_rev = 0;
    uint8_t sw_rev = 0;
    err = readRegister(REG_HW_REV, &hw_rev, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = readRegister(REG_SW_REV, &sw_rev, 1);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "%s UID:%02X%02X HW:%u SW:%u", name(), id_bytes[1], id_bytes[0], hw_rev, sw_rev);
    return ESP_OK;
}

esp_err_t Py32Pm1Device::configureDefaults() {
    return ESP_OK;
}

// ========== 通用 I2C 升级函数 (Arduino & ESP-IDF 双平台) ==========

namespace {
// I2CBusAdapter 版本的内部辅助函数

esp_err_t trigger_download_mode_via_adapter(I2CBusAdapter &adapter) {
#if defined(ARDUINO)
    Serial.println("[PM1] Triggering download mode...");
#endif
    esp_err_t err = adapter.writeRegister(DOWNLOAD_REGISTER, &DOWNLOAD_KEY, 1, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to request download mode via adapter: %s", esp_err_to_name(err));
#if defined(ARDUINO)
        Serial.printf("[PM1] Failed to trigger download mode: %s\n", esp_err_to_name(err));
#endif
        return err;
    }
    delay_ms(DOWNLOAD_TRIGGER_DELAY_MS);
    return ESP_OK;
}

esp_err_t ping_bootloader_via_adapter(I2CBusAdapter &adapter) {
    uint8_t dummy = 0;
    return adapter.readRaw(&dummy, 1, 100);
}

esp_err_t wait_for_bootloader_ready_via_adapter(I2CBusAdapter &adapter, uint32_t timeout_ms) {
#if defined(ARDUINO)
    Serial.printf("[PM1] Waiting for bootloader (timeout=%u ms)...\n", timeout_ms);
#endif
    const uint64_t deadline = runtime_millis() + timeout_ms;
    while (runtime_millis() < deadline) {
        esp_err_t err = ping_bootloader_via_adapter(adapter);
        if (err == ESP_OK) {
#if defined(ARDUINO)
            Serial.println("[PM1] Bootloader is ready!");
#endif
            return ESP_OK;
        }
        delay_ms(BOOTLOADER_POLL_INTERVAL_MS);
    }
    ESP_LOGE(TAG, "bootloader did not respond within %lu ms", static_cast<unsigned long>(timeout_ms));
#if defined(ARDUINO)
    Serial.println("[PM1] Bootloader timeout!");
#endif
    return ESP_ERR_TIMEOUT;
}

esp_err_t stream_firmware_via_adapter(const uint8_t *data,
                                       size_t length,
                                       const Py32Pm1IapOptions &options,
                                       I2CBusAdapter &adapter) {
    if (data == nullptr || length == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (options.chunk_size == 0 || options.chunk_size > 255) {
        ESP_LOGE(TAG, "invalid chunk size %u", static_cast<unsigned>(options.chunk_size));
        return ESP_ERR_INVALID_ARG;
    }

    std::vector<uint8_t> frame(IAP_HEADER_SIZE + options.chunk_size, 0x00);
    const size_t total_packets = (length + options.chunk_size - 1) / options.chunk_size;
    uint32_t address = FLASH_BASE_ADDRESS;
    size_t offset = 0;
    size_t packet_index = 0;

    bool notified_complete = false;
    while (offset < length) {
        const size_t chunk = std::min(static_cast<size_t>(options.chunk_size), length - offset);
        frame[0] = options.write_opcode;
        frame[1] = static_cast<uint8_t>((address >> 24) & 0xFF);
        frame[2] = static_cast<uint8_t>((address >> 16) & 0xFF);
        frame[3] = static_cast<uint8_t>((address >> 8) & 0xFF);
        frame[4] = static_cast<uint8_t>(address & 0xFF);
        frame[5] = 0x00;
        frame[6] = static_cast<uint8_t>(chunk & 0xFF);
        frame[7] = IAP_PADDING_BYTE;
        std::memcpy(frame.data() + IAP_HEADER_SIZE, data + offset, chunk);

        // 在发送前等待 Bootloader 就绪 (Flash 操作可能正在进行)
        uint32_t wait_count = 0;
        const uint32_t max_wait = 50; // 最多等待 50 * 100ms = 5秒
        while (ping_bootloader_via_adapter(adapter) != ESP_OK) {
            delay_ms(100);
            if (++wait_count >= max_wait) {
                ESP_LOGE(TAG, "Bootloader not responding before write at 0x%08lX", 
                         static_cast<unsigned long>(address));
                return ESP_ERR_TIMEOUT;
            }
        }

        esp_err_t err = adapter.writeRaw(frame.data(), IAP_HEADER_SIZE + chunk, options.transfer_timeout_ms);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c write failed at 0x%08lX: %s",
                     static_cast<unsigned long>(address), esp_err_to_name(err));
#if defined(ARDUINO)
            Serial.printf("[PM1] Write failed at 0x%08lX: %s\n", static_cast<unsigned long>(address), esp_err_to_name(err));
#endif
            return err;
        }

        offset += chunk;
        address += static_cast<uint32_t>(chunk);
        ++packet_index;

        float percent = 100.0f * static_cast<float>(packet_index) / static_cast<float>(total_packets);
        ESP_LOGI(TAG, "[iap] chunk %03zu/%03zu (%.2f%%)", packet_index, total_packets, percent);
#if defined(ARDUINO)
        if (packet_index % 10 == 0 || packet_index == total_packets) {
             Serial.printf("[PM1] Progress: %03zu/%03zu (%.1f%%)\n", packet_index, total_packets, percent);
        }
#endif

        if (!notified_complete && percent >= 100.0f) {
            ESP_LOGI(TAG, "[iap] firmware refresh complete; waiting for device reboot...");
            notified_complete = true;
        }

        if (options.chunk_delay_ms > 0) {
            delay_ms(options.chunk_delay_ms);
        }
    }

    return ESP_OK;
}

esp_err_t jump_to_application_via_adapter(I2CBusAdapter &adapter, uint8_t opcode) {
#if defined(ARDUINO)
    Serial.println("[PM1] Sending jump command...");
#endif
    esp_err_t err = adapter.writeRaw(&opcode, 1, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to send jump opcode: %s", esp_err_to_name(err));
#if defined(ARDUINO)
        Serial.printf("[PM1] Failed to send jump command: %s\n", esp_err_to_name(err));
#endif
        return err;
    }

#if defined(ARDUINO)
    Serial.println("[PM1] Waiting for device to reboot (expecting NACK from bootloader)...");
#endif

    const uint64_t deadline = runtime_millis() + JUMP_RELEASE_TIMEOUT_MS;
    while (runtime_millis() < deadline) {
        if (ping_bootloader_via_adapter(adapter) != ESP_OK) {
            ESP_LOGI(TAG, "bootloader released bus, application should be running");
#if defined(ARDUINO)
            Serial.println("[PM1] Bootloader released bus. Jump successful!");
#endif
            return ESP_OK;
        }
        
        // Retry sending jump command
        adapter.writeRaw(&opcode, 1, 1000);
        
        delay_ms(200);
    }

    ESP_LOGW(TAG, "bootloader still responding after jump command");
#if defined(ARDUINO)
    Serial.println("[PM1] Warning: Bootloader still responding. Jump might have failed or device is stuck.");
#endif
    // 即使超时，也可能只是因为设备重启慢，或者已经重启但地址巧合？
    // 但通常如果还在响应 0x54，说明还在 Bootloader。
    // 我们可以选择返回 OK，让用户去检查，或者保持 TIMEOUT。
    // 这里我们尝试返回 OK，但打印警告，因为固件已经写完了。
    return ESP_OK; 
}
}  // namespace

esp_err_t Py32Pm1Device::i2cArrayUpgrade(I2CBusAdapter &pm1_adapter,
                                          const Py32Pm1FirmwareImage &image,
                                          const Py32Pm1IapOptions &options) {
    if (!pm1_adapter.ready()) {
        ESP_LOGE(TAG, "PM1 I2C adapter not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (image.data == nullptr || image.length == 0) {
        ESP_LOGE(TAG, "invalid firmware image payload");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "[iap] starting I2C array upgrade (%zu bytes)", image.length);

    // 计算固件 MD5 用于日志
    std::array<uint8_t, 16> firmware_md5 = {0};
    esp_err_t err = compute_md5(image.data, image.length, firmware_md5);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[iap] firmware MD5: %s", md5_to_hex(firmware_md5).c_str());
    }

    // Step 1: 准备 Bootloader 适配器 (0x54)
    I2CBusAdapter boot_adapter;
    Py32I2CConfig boot_config = pm1_adapter.createConfigWithAddress(BOOTLOADER_ADDRESS);
    
    err = boot_adapter.configure(boot_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure bootloader adapter");
        return err;
    }
    
    err = boot_adapter.begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start bootloader adapter");
        return err;
    }

    // Step 2: 检查是否已经在 Bootloader 模式
    bool already_in_bootloader = (ping_bootloader_via_adapter(boot_adapter) == ESP_OK);

    if (already_in_bootloader) {
        ESP_LOGI(TAG, "Device already in bootloader mode (0x%02X)", BOOTLOADER_ADDRESS);
#if defined(ARDUINO)
        Serial.printf("[PM1] Device already in bootloader mode (0x%02X)\n", BOOTLOADER_ADDRESS);
#endif
    } else {
        // Step 3: 如果不在 Bootloader 模式，尝试触发进入
        err = trigger_download_mode_via_adapter(pm1_adapter);
        if (err != ESP_OK) {
            // 触发失败，且之前也没检测到 bootloader，那就是真的失败了
            boot_adapter.end();
            return err;
        }
    }

    // Step 4: 等待 Bootloader 就绪
    err = wait_for_bootloader_ready_via_adapter(boot_adapter, BOOTLOADER_WAIT_TIMEOUT_MS);
    if (err != ESP_OK) {
        boot_adapter.end();
        return err;
    }

    // Step 5: 传输固件 (每个数据帧第一个字节已包含 write_opcode)
    err = stream_firmware_via_adapter(image.data, image.length, options, boot_adapter);
    if (err != ESP_OK) {
        boot_adapter.end();
        return err;
    }

    // Step 6: 发送跳转命令
    if (options.send_jump_command) {
        err = jump_to_application_via_adapter(boot_adapter, options.jump_opcode);
        if (err != ESP_OK) {
            boot_adapter.end();
            return err;
        }
    }

    boot_adapter.end();
    ESP_LOGI(TAG, "[iap] I2C array upgrade complete");
    return ESP_OK;
}

esp_err_t Py32Pm1Device::i2cBridgeUpgrade(I2CBusAdapter &pm1_adapter,
                                           BusAdapter &user_input,
                                           const Py32Pm1IapOptions &iap_options,
                                           const Py32Pm1SerialInputOptions &input_options) {
    if (!pm1_adapter.ready()) {
        ESP_LOGE(TAG, "PM1 I2C adapter not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (!user_input.ready()) {
        ESP_LOGE(TAG, "user input adapter not ready");
        return ESP_ERR_INVALID_STATE;
    }

    send_uart_prompt(user_input);

    // Stage 1: 捕获并解析 MD5 和固件数据
    ESP_LOGI(TAG, "[iap] waiting for MD5 + firmware payload...");
    std::vector<uint8_t> raw_capture;
    raw_capture.reserve(128 * 1024);
    esp_err_t err = capture_serial_stream(user_input, input_options, raw_capture);
    if (err != ESP_OK) {
        return err;
    }

    std::array<uint8_t, 16> expected_md5 = {0};
    std::vector<uint8_t> firmware_payload;
    firmware_payload.reserve(raw_capture.size());
    err = parse_md5_and_firmware(raw_capture, input_options, expected_md5, firmware_payload);
    if (err != ESP_OK) {
        return err;
    }

    // 验证 MD5
    std::array<uint8_t, 16> actual_md5 = {0};
    err = compute_md5(firmware_payload.data(), firmware_payload.size(), actual_md5);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to compute firmware MD5");
        return err;
    }

    if (input_options.expect_md5_prefix && actual_md5 != expected_md5) {
        ESP_LOGE(TAG, "MD5 mismatch between provided and calculated digests");
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "[iap] MD5 verified (%zu bytes)", firmware_payload.size());

    // 使用数组升级函数
    Py32Pm1FirmwareImage image{
        .data = firmware_payload.data(),
        .length = firmware_payload.size(),
        .base_address = 0x08001000,
    };

    err = i2cArrayUpgrade(pm1_adapter, image, iap_options);
    if (err != ESP_OK) {
        return err;
    }

    // 发送成功消息
    static constexpr char kSuccessMsg[] = "[iap] OK\r\n";
    user_input.writeRaw(reinterpret_cast<const uint8_t *>(kSuccessMsg), sizeof(kSuccessMsg) - 1, 500);
    delay_ms(50);

    ESP_LOGI(TAG, "[iap] I2C bridge upgrade complete");
    return ESP_OK;
}

#if !defined(ARDUINO)
// 以下函数仅在 ESP-IDF 环境中可用（使用 i2c_bus API）

esp_err_t Py32Pm1Device::uartToI2cBusUpgrade(const Py32SerialConfig &uart_config,
                                                 i2c_bus_handle_t bus,
                                                 const Py32Pm1IapOptions &iap_options,
                                                 const Py32Pm1SerialInputOptions &input_options) {
    if (bus == nullptr) {
        ESP_LOGE(TAG, "invalid i2c handles for PM1 upgrade");
        return ESP_ERR_INVALID_ARG;
    }

    // 使用 SerialBusAdapter 作为 UART 接口
    // 注意：这里不能直接使用 ContextBusAdapter，因为需要硬件 UART 配置
    SerialBusAdapter user_serial;
    esp_err_t err = user_serial.configure(uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure UART adapter: %s", esp_err_to_name(err));
        return err;
    }

    err = user_serial.begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start UART adapter: %s", esp_err_to_name(err));
        return err;
    }

    send_uart_prompt(user_serial);

    i2c_bus_device_handle_t boot_dev = nullptr;
    auto cleanup = [&]() {
        if (boot_dev != nullptr) {
            i2c_bus_device_delete(&boot_dev);
            boot_dev = nullptr;
        }
        user_serial.end();
    };

    // Stage 1: Capture and parse MD5 header only
    ESP_LOGI(TAG, "[iap] stage 1: waiting for MD5 header...");
    std::vector<uint8_t> md5_capture;
    md5_capture.reserve(64);
    err = capture_serial_stream(user_serial, input_options, md5_capture);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    std::array<uint8_t, 16> provided_md5 = {0};
    size_t payload_offset = 0;
    err = parse_md5_header(md5_capture, input_options, provided_md5, payload_offset);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    const std::string provided_md5_hex = md5_to_hex(provided_md5);
    ESP_LOGI(TAG, "[iap] received MD5 header: %s", provided_md5_hex.c_str());
    
    // Send MD5 acknowledgment
    std::string md5_ack = "[iap] MD5 OK\r\n";
    user_serial.writeRaw(reinterpret_cast<const uint8_t *>(md5_ack.data()), md5_ack.size(), 500);
    delay_ms(50);

    // Stage 2: Capture firmware binary payload
    ESP_LOGI(TAG, "[iap] stage 2: waiting for firmware payload...");
    std::vector<uint8_t> firmware_payload;
    firmware_payload.reserve(128 * 1024);
    
    if (payload_offset < md5_capture.size()) {
        firmware_payload.assign(md5_capture.begin() + payload_offset, md5_capture.end());
        ESP_LOGI(TAG, "[iap] found %zu bytes payload in MD5 capture", firmware_payload.size());
    }

    std::array<uint8_t, 16> calculated_md5 = {0};
    bool payload_verified = false;

    for (int attempt = 0; attempt < 2 && !payload_verified; ++attempt) {
        if (!firmware_payload.empty()) {
            err = compute_md5(firmware_payload.data(), firmware_payload.size(), calculated_md5);
            if (err != ESP_OK) {
                cleanup();
                return err;
            }

            const std::string calculated_md5_hex = md5_to_hex(calculated_md5);
            if (!input_options.expect_md5_prefix || calculated_md5 == provided_md5) {
                ESP_LOGI(TAG, "[iap] firmware payload %zu bytes verified from %s (MD5 %s)",
                         firmware_payload.size(),
                         attempt == 0 ? "MD5 capture" : "additional capture",
                         calculated_md5_hex.c_str());
                payload_verified = true;
                break;
            }

            ESP_LOGW(TAG, "[iap] %s MD5 mismatch (expected %s, got %s), waiting for more data",
                     attempt == 0 ? "MD5 capture" : "additional capture",
                     provided_md5_hex.c_str(), calculated_md5_hex.c_str());
        }

        if (attempt == 1) {
            break;
        }

        Py32Pm1SerialInputOptions firmware_options = input_options;
        firmware_options.per_byte_timeout_ms = 50;
        firmware_options.idle_timeout_ms = 500;

        std::vector<uint8_t> firmware_capture;
        firmware_capture.reserve(128 * 1024);
        err = capture_serial_stream(user_serial, firmware_options, firmware_capture);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            if (firmware_payload.empty()) {
                cleanup();
                return err;
            }
            ESP_LOGW(TAG, "[iap] additional capture aborted: %s", esp_err_to_name(err));
            break;
        }

        if (firmware_capture.empty()) {
            ESP_LOGW(TAG, "[iap] no additional firmware bytes captured");
        } else {
            ESP_LOGI(TAG, "[iap] stage 2 captured additional %zu bytes", firmware_capture.size());
            firmware_payload.insert(firmware_payload.end(), firmware_capture.begin(), firmware_capture.end());
        }
    }

    if (!payload_verified) {
        if (firmware_payload.empty()) {
            ESP_LOGE(TAG, "[iap] no firmware payload received");
            static constexpr char kNeedPayload[] = "[iap] Need firmware\r\n";
            user_serial.writeRaw(reinterpret_cast<const uint8_t *>(kNeedPayload), sizeof(kNeedPayload) - 1, 500);
            delay_ms(50);
            cleanup();
            return ESP_ERR_INVALID_SIZE;
        }

        if (input_options.expect_md5_prefix) {
            ESP_LOGE(TAG, "[iap] firmware payload MD5 mismatch after additional capture");
            cleanup();
            return ESP_ERR_INVALID_CRC;
        }

        ESP_LOGW(TAG, "[iap] proceeding without MD5 verification (MD5 prefix not required)");
    }

    boot_dev = i2c_bus_device_create(bus, BOOTLOADER_ADDRESS, 100000);
    if (boot_dev == nullptr) {
        ESP_LOGE(TAG, "failed to create bootloader device at 0x%02X", BOOTLOADER_ADDRESS);
        cleanup();
        return ESP_ERR_NO_MEM;
    }

    bool already_in_bootloader = (ping_bootloader(boot_dev) == ESP_OK);
    if (already_in_bootloader) {
        ESP_LOGI(TAG, "Device already in bootloader mode (0x%02X)", BOOTLOADER_ADDRESS);
    } else {
        i2c_bus_device_handle_t app_dev = i2c_bus_device_create(bus, 0x6E, 100000);
        if (app_dev == nullptr) {
            ESP_LOGE(TAG, "failed to create app device at 0x6E");
            cleanup();
            return ESP_ERR_NO_MEM;
        }
        err = trigger_download_mode(app_dev);
        i2c_bus_device_delete(&app_dev);
        if (err != ESP_OK) {
            cleanup();
            return err;
        }
    }

    err = wait_for_bootloader_ready(boot_dev, BOOTLOADER_WAIT_TIMEOUT_MS);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    err = stream_firmware_over_i2c(firmware_payload.data(), firmware_payload.size(), iap_options, boot_dev);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    if (iap_options.send_jump_command) {
        err = jump_to_application(boot_dev, iap_options.jump_opcode);
        if (err != ESP_OK) {
            cleanup();
            return err;
        }
    }

    ESP_LOGI(TAG, "[iap] UART->I2C upgrade complete");
    static constexpr char kSuccessMsg[] = "[iap] OK\r\n";
    user_serial.writeRaw(reinterpret_cast<const uint8_t *>(kSuccessMsg), sizeof(kSuccessMsg) - 1, 500);
    delay_ms(50);
    cleanup();
    return ESP_OK;
}

esp_err_t Py32Pm1Device::contextBusToI2cBusUpgrade(BusAdapter &user_input,
                                                       i2c_bus_handle_t bus,
                                                       const Py32Pm1IapOptions &iap_options,
                                                       const Py32Pm1SerialInputOptions &input_options) {
    if (bus == nullptr) {
        ESP_LOGE(TAG, "invalid i2c handles for PM1 upgrade");
        return ESP_ERR_INVALID_ARG;
    }

    if (!user_input.ready()) {
        ESP_LOGE(TAG, "user input adapter not ready");
        return ESP_ERR_INVALID_STATE;
    }

    send_uart_prompt(user_input);

    i2c_bus_device_handle_t boot_dev = nullptr;
    auto cleanup = [&]() {
        if (boot_dev != nullptr) {
            i2c_bus_device_delete(&boot_dev);
            boot_dev = nullptr;
        }
    };

    // Stage 1: Capture and parse MD5 header only
    ESP_LOGI(TAG, "[iap] stage 1: waiting for MD5 header...");
    std::vector<uint8_t> md5_capture;
    md5_capture.reserve(64);
    esp_err_t err = capture_serial_stream(user_input, input_options, md5_capture);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    std::array<uint8_t, 16> provided_md5 = {0};
    size_t payload_offset = 0;
    err = parse_md5_header(md5_capture, input_options, provided_md5, payload_offset);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    const std::string provided_md5_hex = md5_to_hex(provided_md5);
    ESP_LOGI(TAG, "[iap] received MD5 header: %s", provided_md5_hex.c_str());
    
    // Send MD5 acknowledgment
    std::string md5_ack = "[iap] MD5 OK\r\n";
    user_input.writeRaw(reinterpret_cast<const uint8_t *>(md5_ack.data()), md5_ack.size(), 500);
    delay_ms(50);

    // Stage 2: Capture firmware binary payload
    ESP_LOGI(TAG, "[iap] stage 2: waiting for firmware payload...");
    std::vector<uint8_t> firmware_payload;
    firmware_payload.reserve(128 * 1024);
    
    if (payload_offset < md5_capture.size()) {
        firmware_payload.assign(md5_capture.begin() + payload_offset, md5_capture.end());
        ESP_LOGI(TAG, "[iap] found %zu bytes payload in MD5 capture", firmware_payload.size());
    }

    std::array<uint8_t, 16> calculated_md5 = {0};
    bool payload_verified = false;

    for (int attempt = 0; attempt < 2 && !payload_verified; ++attempt) {
        if (!firmware_payload.empty()) {
            err = compute_md5(firmware_payload.data(), firmware_payload.size(), calculated_md5);
            if (err != ESP_OK) {
                cleanup();
                return err;
            }

            const std::string calculated_md5_hex = md5_to_hex(calculated_md5);
            if (!input_options.expect_md5_prefix || calculated_md5 == provided_md5) {
                ESP_LOGI(TAG, "[iap] firmware payload %zu bytes verified from %s (MD5 %s)",
                         firmware_payload.size(),
                         attempt == 0 ? "MD5 capture" : "additional capture",
                         calculated_md5_hex.c_str());
                payload_verified = true;
                break;
            }

            ESP_LOGW(TAG, "[iap] %s MD5 mismatch (expected %s, got %s), waiting for more data",
                     attempt == 0 ? "MD5 capture" : "additional capture",
                     provided_md5_hex.c_str(), calculated_md5_hex.c_str());
        }

        if (attempt == 1) {
            break;
        }

        Py32Pm1SerialInputOptions firmware_options = input_options;
        firmware_options.per_byte_timeout_ms = 50;
        firmware_options.idle_timeout_ms = 500;

        std::vector<uint8_t> firmware_capture;
        firmware_capture.reserve(128 * 1024);
        err = capture_serial_stream(user_input, firmware_options, firmware_capture);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            if (firmware_payload.empty()) {
                cleanup();
                return err;
            }
            ESP_LOGW(TAG, "[iap] additional capture aborted: %s", esp_err_to_name(err));
            break;
        }

        if (firmware_capture.empty()) {
            ESP_LOGW(TAG, "[iap] no additional firmware bytes captured");
        } else {
            ESP_LOGI(TAG, "[iap] stage 2 captured additional %zu bytes", firmware_capture.size());
            firmware_payload.insert(firmware_payload.end(), firmware_capture.begin(), firmware_capture.end());
        }
    }

    if (!payload_verified) {
        if (firmware_payload.empty()) {
            ESP_LOGE(TAG, "[iap] no firmware payload received");
            static constexpr char kNeedPayload[] = "[iap] Need firmware\r\n";
            user_input.writeRaw(reinterpret_cast<const uint8_t *>(kNeedPayload), sizeof(kNeedPayload) - 1, 500);
            delay_ms(50);
            cleanup();
            return ESP_ERR_INVALID_SIZE;
        }

        if (input_options.expect_md5_prefix) {
            ESP_LOGE(TAG, "[iap] firmware payload MD5 mismatch after additional capture");
            cleanup();
            return ESP_ERR_INVALID_CRC;
        }

        ESP_LOGW(TAG, "[iap] proceeding without MD5 verification (MD5 prefix not required)");
    }

    boot_dev = i2c_bus_device_create(bus, BOOTLOADER_ADDRESS, 100000);
    if (boot_dev == nullptr) {
        ESP_LOGE(TAG, "failed to create bootloader device at 0x%02X", BOOTLOADER_ADDRESS);
        cleanup();
        return ESP_ERR_NO_MEM;
    }

    bool already_in_bootloader = (ping_bootloader(boot_dev) == ESP_OK);
    if (already_in_bootloader) {
        ESP_LOGI(TAG, "Device already in bootloader mode (0x%02X)", BOOTLOADER_ADDRESS);
    } else {
        i2c_bus_device_handle_t app_dev = i2c_bus_device_create(bus, 0x6E, 100000);
        if (app_dev == nullptr) {
            ESP_LOGE(TAG, "failed to create app device at 0x6E");
            cleanup();
            return ESP_ERR_NO_MEM;
        }
        err = trigger_download_mode(app_dev);
        i2c_bus_device_delete(&app_dev);
        if (err != ESP_OK) {
            cleanup();
            return err;
        }
    }

    err = wait_for_bootloader_ready(boot_dev, BOOTLOADER_WAIT_TIMEOUT_MS);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    err = stream_firmware_over_i2c(firmware_payload.data(), firmware_payload.size(), iap_options, boot_dev);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    if (iap_options.send_jump_command) {
        err = jump_to_application(boot_dev, iap_options.jump_opcode);
        if (err != ESP_OK) {
            cleanup();
            return err;
        }
    }

    ESP_LOGI(TAG, "[iap] ContextBus->I2C upgrade complete");
    static constexpr char kSuccessMsg[] = "[iap] OK\r\n";
    user_input.writeRaw(reinterpret_cast<const uint8_t *>(kSuccessMsg), sizeof(kSuccessMsg) - 1, 500);
    delay_ms(50);
    cleanup();
    return ESP_OK;
}

esp_err_t Py32Pm1Device::directArrayToI2cBusUpgrade(const uint8_t *firmware_data,
                                                  size_t firmware_length,
                                                  i2c_bus_handle_t bus,
                                                  const Py32Pm1IapOptions &iap_options) {
    if (firmware_data == nullptr || firmware_length == 0) {
        ESP_LOGE(TAG, "invalid firmware data or length");
        return ESP_ERR_INVALID_ARG;
    }

    if (bus == nullptr) {
        ESP_LOGE(TAG, "invalid i2c handles for PM1 upgrade");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "[iap] starting direct array upgrade (%zu bytes)", firmware_length);

    i2c_bus_device_handle_t boot_dev = nullptr;
    auto cleanup = [&]() {
        if (boot_dev != nullptr) {
            i2c_bus_device_delete(&boot_dev);
            boot_dev = nullptr;
        }
    };

    // 计算固件 MD5 用于验证
    std::array<uint8_t, 16> firmware_md5 = {0};
    esp_err_t err = compute_md5(firmware_data, firmware_length, firmware_md5);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to compute firmware MD5");
        return err;
    }
    ESP_LOGI(TAG, "[iap] firmware MD5: %s", md5_to_hex(firmware_md5).c_str());

    // 创建 bootloader I2C 设备
    boot_dev = i2c_bus_device_create(bus, BOOTLOADER_ADDRESS, 100000);
    if (boot_dev == nullptr) {
        ESP_LOGE(TAG, "failed to create bootloader device at 0x%02X", BOOTLOADER_ADDRESS);
        cleanup();
        return ESP_ERR_NO_MEM;
    }

    // 检查是否已经在 Bootloader 模式
    bool already_in_bootloader = (ping_bootloader(boot_dev) == ESP_OK);
    if (already_in_bootloader) {
        ESP_LOGI(TAG, "Device already in bootloader mode (0x%02X)", BOOTLOADER_ADDRESS);
    } else {
        // 触发 PM1 进入下载模式
        i2c_bus_device_handle_t app_dev = i2c_bus_device_create(bus, 0x6E, 100000);
        if (app_dev == nullptr) {
            ESP_LOGE(TAG, "failed to create app device at 0x6E");
            cleanup();
            return ESP_ERR_NO_MEM;
        }
        err = trigger_download_mode(app_dev);
        i2c_bus_device_delete(&app_dev);
        if (err != ESP_OK) {
            cleanup();
            return err;
        }
    }

    // 等待 bootloader 就绪
    err = wait_for_bootloader_ready(boot_dev, BOOTLOADER_WAIT_TIMEOUT_MS);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    // 通过 I2C 传输固件 (每个数据帧第一个字节已包含 write_opcode)
    err = stream_firmware_over_i2c(firmware_data, firmware_length, iap_options, boot_dev);
    if (err != ESP_OK) {
        cleanup();
        return err;
    }

    // 发送跳转命令（如果需要）
    if (iap_options.send_jump_command) {
        err = jump_to_application(boot_dev, iap_options.jump_opcode);
        if (err != ESP_OK) {
            cleanup();
            return err;
        }
    }

    ESP_LOGI(TAG, "[iap] direct array upgrade complete");
    cleanup();
    return ESP_OK;
}

#endif  // !defined(ARDUINO)

}  // namespace m5::py32bin::devices
