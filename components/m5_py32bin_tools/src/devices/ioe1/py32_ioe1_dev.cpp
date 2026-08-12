#include "devices/ioe1/py32_ioe1_dev.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

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
constexpr char TAG[] = "py32-ioe1";

// IOE1 GPIO 寄存器地址
constexpr uint32_t REG_DEVICE_ID = 0x40;
constexpr uint32_t REG_GPIO_DIR  = 0x50;
constexpr uint32_t REG_GPIO_OUT  = 0x52;
constexpr uint32_t REG_GPIO_IN   = 0x53;

// PY32 IAP 协议常量（与参考代码 ioe1_upgrade.cpp 一致）
// 触发寄存器：向应用地址的 0xFD 写 0x1A 进入 bootloader
constexpr uint8_t  DOWNLOAD_REGISTER           = 0xFD;
constexpr uint8_t  DOWNLOAD_KEY                = 0x1A;
// IOE1 IAP address is fixed. The application address can vary (for example
// 0x6F or 0x4F), but all IAP transfers go through 0x5F.
constexpr uint8_t  BOOTLOADER_ADDRESS          = 0x5F;
constexpr uint32_t FLASH_BASE_ADDRESS          = 0x08001000;
constexpr size_t   IAP_HEADER_SIZE             = 8;
constexpr uint8_t  IAP_PADDING_BYTE            = 0xFF;
constexpr uint32_t DOWNLOAD_TRIGGER_DELAY_MS   = 200;
constexpr uint32_t BOOTLOADER_WAIT_TIMEOUT_MS  = 30000;
constexpr uint32_t BOOTLOADER_POLL_INTERVAL_MS = 100;
constexpr uint32_t JUMP_RELEASE_TIMEOUT_MS     = 5000;

// ---- 通用工具 ----

void delay_ms(uint32_t ms) {
    if (ms == 0) return;
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

esp_err_t compute_md5(const uint8_t *data, size_t length, std::array<uint8_t, 16> &digest) {
    if (data == nullptr || length == 0) return ESP_ERR_INVALID_ARG;
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, data, static_cast<uint32_t>(length));
    esp_rom_md5_final(digest.data(), &ctx);
    return ESP_OK;
}

std::string md5_to_hex(const std::array<uint8_t, 16> &digest) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (uint8_t b : digest) {
        out.push_back(HEX[(b >> 4) & 0x0F]);
        out.push_back(HEX[b & 0x0F]);
    }
    return out;
}

// ---- I2CBusAdapter 辅助函数（参考 ioe1_upgrade.cpp）----

esp_err_t ping_bootloader_via_adapter(I2CBusAdapter &adapter) {
    // 只检测 bootloader 是否应答（不读数据）
    uint8_t dummy = 0;
    return adapter.readRaw(&dummy, 1, 100);
}

esp_err_t wait_for_bootloader_via_adapter(I2CBusAdapter &adapter, uint32_t timeout_ms) {
    const uint64_t deadline = runtime_millis() + timeout_ms;
    uint32_t attempts = 0;
    while (runtime_millis() < deadline) {
        if (ping_bootloader_via_adapter(adapter) == ESP_OK) return ESP_OK;
        ESP_LOGD(TAG, "[iap] 等待 bootloader 应答... %lu", static_cast<unsigned long>(++attempts));
        delay_ms(BOOTLOADER_POLL_INTERVAL_MS);
    }
    ESP_LOGE(TAG, "bootloader 在 %lu ms 内未应答", static_cast<unsigned long>(timeout_ms));
    return ESP_ERR_TIMEOUT;
}

// 参考 py32_func_enter_iap：bootloader 就绪后发送单个 WREN 字节作为 IAP 入口握手
esp_err_t enter_iap_via_adapter(I2CBusAdapter &adapter, uint8_t write_opcode) {
    ESP_LOGI(TAG, "[iap] 发送 WREN 入口握手 (0x%02X)", write_opcode);
    esp_err_t err = adapter.writeRaw(&write_opcode, 1, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[iap] WREN 握手失败: %s", esp_err_to_name(err));
    }
    return err;
}

// 参考 py32_func_iap_upgrade：分块写入固件
esp_err_t stream_firmware_via_adapter(const uint8_t *data, size_t length,
                                       const Py32Ioe1IapOptions &options,
                                       I2CBusAdapter &adapter) {
    if (data == nullptr || length == 0) return ESP_ERR_INVALID_SIZE;
    if (options.chunk_size == 0 || options.chunk_size > 255) {
        ESP_LOGE(TAG, "chunk_size 无效: %u", static_cast<unsigned>(options.chunk_size));
        return ESP_ERR_INVALID_ARG;
    }

    std::array<uint8_t, IAP_HEADER_SIZE + 255> frame{};
    const size_t total = (length + options.chunk_size - 1) / options.chunk_size;
    uint32_t address = FLASH_BASE_ADDRESS;
    size_t offset = 0;
    size_t pkt = 0;

    while (offset < length) {
        const size_t chunk = std::min(static_cast<size_t>(options.chunk_size), length - offset);

        // 包头格式（8 字节）：[WREN][addr_H][addr_MH][addr_ML][addr_L][0x00][len][0xFF]
        frame[0] = options.write_opcode;
        frame[1] = static_cast<uint8_t>((address >> 24) & 0xFF);
        frame[2] = static_cast<uint8_t>((address >> 16) & 0xFF);
        frame[3] = static_cast<uint8_t>((address >> 8)  & 0xFF);
        frame[4] = static_cast<uint8_t>(address & 0xFF);
        frame[5] = 0x00;
        frame[6] = static_cast<uint8_t>(chunk & 0xFF);
        frame[7] = IAP_PADDING_BYTE;
        std::memcpy(frame.data() + IAP_HEADER_SIZE, data + offset, chunk);

        // 等待 bootloader 可用（参考代码在每包前检查）
        uint32_t wait_count = 0;
        while (ping_bootloader_via_adapter(adapter) != ESP_OK) {
            delay_ms(100);
            if (++wait_count >= 50) {
                ESP_LOGE(TAG, "[iap] bootloader 超时，地址 0x%08lX",
                         static_cast<unsigned long>(address));
                return ESP_ERR_TIMEOUT;
            }
        }

        esp_err_t err = adapter.writeRaw(frame.data(), IAP_HEADER_SIZE + chunk,
                                          options.transfer_timeout_ms);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[iap] 写入失败 0x%08lX: %s",
                     static_cast<unsigned long>(address), esp_err_to_name(err));
            return err;
        }

        offset  += chunk;
        address += static_cast<uint32_t>(chunk);
        ++pkt;
        const uint32_t progress_x10 = total == 0 ? 1000U :
            static_cast<uint32_t>((pkt * 1000U + total / 2U) / total);
        ESP_LOGI(TAG, "[iap] data packet %03zu/%03zu | %lu.%lu%%",
                 pkt, total,
                 static_cast<unsigned long>(progress_x10 / 10U),
                 static_cast<unsigned long>(progress_x10 % 10U));

        if (options.chunk_delay_ms > 0) delay_ms(options.chunk_delay_ms);
    }
    return ESP_OK;
}

// 参考 py32_func_iap_upgrade 结尾：发送跳转指令并等待 bootloader 退出
esp_err_t jump_to_application_via_adapter(I2CBusAdapter &adapter, uint8_t opcode) {
    esp_err_t err = adapter.writeRaw(&opcode, 1, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[iap] 跳转指令发送失败: %s", esp_err_to_name(err));
        return err;
    }
    const uint64_t deadline = runtime_millis() + JUMP_RELEASE_TIMEOUT_MS;
    while (runtime_millis() < deadline) {
        if (ping_bootloader_via_adapter(adapter) != ESP_OK) {
            ESP_LOGI(TAG, "[iap] bootloader 已退出，应用已启动");
            return ESP_OK;
        }
        // 继续发送跳转指令直到 bootloader 不再应答
        adapter.writeRaw(&opcode, 1, 1000);
        delay_ms(200);
    }
    ESP_LOGW(TAG, "[iap] bootloader 在跳转指令后仍在应答");
    return ESP_OK;
}

#if !defined(ARDUINO)
// ---- ESP-IDF i2c_bus 辅助函数 ----

esp_err_t ping_bootloader(i2c_bus_device_handle_t dev) {
    uint8_t dummy = 0;
    return i2c_bus_read_byte(dev, NULL_I2C_MEM_ADDR, &dummy);
}

esp_err_t wait_for_bootloader(i2c_bus_device_handle_t dev, uint32_t timeout_ms) {
    const uint64_t deadline = runtime_millis() + timeout_ms;
    uint32_t attempts = 0;
    while (runtime_millis() < deadline) {
        if (ping_bootloader(dev) == ESP_OK) return ESP_OK;
        ESP_LOGD(TAG, "[iap] 等待 bootloader... %lu", static_cast<unsigned long>(++attempts));
        delay_ms(BOOTLOADER_POLL_INTERVAL_MS);
    }
    ESP_LOGE(TAG, "[iap] bootloader 在 %lu ms 内未应答", static_cast<unsigned long>(timeout_ms));
    return ESP_ERR_TIMEOUT;
}

// 参考 py32_func_enter_iap：发送单个 WREN 字节作为 IAP 入口握手
esp_err_t enter_iap(i2c_bus_device_handle_t dev, uint8_t write_opcode) {
    ESP_LOGI(TAG, "[iap] 发送 WREN 入口握手 (0x%02X)", write_opcode);
    esp_err_t err = i2c_bus_write_bytes(dev, NULL_I2C_MEM_ADDR, 1, &write_opcode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[iap] WREN 握手失败: %s", esp_err_to_name(err));
    }
    return err;
}

// 参考 py32_func_iap_upgrade：分块写入固件
esp_err_t stream_firmware_over_i2c(const uint8_t *data, size_t length,
                                    const Py32Ioe1IapOptions &options,
                                    i2c_bus_device_handle_t dev) {
    if (data == nullptr || length == 0) return ESP_ERR_INVALID_SIZE;
    if (options.chunk_size == 0 || options.chunk_size > 255) return ESP_ERR_INVALID_ARG;

    std::array<uint8_t, IAP_HEADER_SIZE + 255> frame{};
    const size_t total = (length + options.chunk_size - 1) / options.chunk_size;
    uint32_t address = FLASH_BASE_ADDRESS;
    size_t offset = 0;
    size_t pkt = 0;

    while (offset < length) {
        const size_t chunk = std::min(static_cast<size_t>(options.chunk_size), length - offset);

        frame[0] = options.write_opcode;
        frame[1] = static_cast<uint8_t>((address >> 24) & 0xFF);
        frame[2] = static_cast<uint8_t>((address >> 16) & 0xFF);
        frame[3] = static_cast<uint8_t>((address >> 8)  & 0xFF);
        frame[4] = static_cast<uint8_t>(address & 0xFF);
        frame[5] = 0x00;
        frame[6] = static_cast<uint8_t>(chunk & 0xFF);
        frame[7] = IAP_PADDING_BYTE;
        std::memcpy(frame.data() + IAP_HEADER_SIZE, data + offset, chunk);

        // 等待 bootloader 可用
        uint32_t wait_count = 0;
        while (ping_bootloader(dev) != ESP_OK) {
            delay_ms(100);
            if (++wait_count >= 50) {
                ESP_LOGE(TAG, "[iap] bootloader 超时，地址 0x%08lX",
                         static_cast<unsigned long>(address));
                return ESP_ERR_TIMEOUT;
            }
        }

        esp_err_t err = i2c_bus_write_bytes(dev, NULL_I2C_MEM_ADDR,
                                             IAP_HEADER_SIZE + chunk, frame.data());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[iap] 写入失败 0x%08lX: %s",
                     static_cast<unsigned long>(address), esp_err_to_name(err));
            return err;
        }

        offset  += chunk;
        address += static_cast<uint32_t>(chunk);
        ++pkt;
        const uint32_t progress_x10 = total == 0 ? 1000U :
            static_cast<uint32_t>((pkt * 1000U + total / 2U) / total);
        ESP_LOGI(TAG, "[iap] data packet %03zu/%03zu | %lu.%lu%%",
                 pkt, total,
                 static_cast<unsigned long>(progress_x10 / 10U),
                 static_cast<unsigned long>(progress_x10 % 10U));

        if (options.chunk_delay_ms > 0) delay_ms(options.chunk_delay_ms);
    }
    return ESP_OK;
}

// 参考 py32_func_iap_upgrade 结尾
esp_err_t jump_to_application(i2c_bus_device_handle_t dev, uint8_t opcode) {
    esp_err_t err = i2c_bus_write_bytes(dev, NULL_I2C_MEM_ADDR, 1, &opcode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[iap] 跳转指令发送失败: %s", esp_err_to_name(err));
        return err;
    }
    const uint64_t deadline = runtime_millis() + JUMP_RELEASE_TIMEOUT_MS;
    while (runtime_millis() < deadline) {
        if (ping_bootloader(dev) != ESP_OK) {
            ESP_LOGI(TAG, "[iap] bootloader 已退出，应用已启动");
            return ESP_OK;
        }
        i2c_bus_write_bytes(dev, NULL_I2C_MEM_ADDR, 1, &opcode);
        delay_ms(200);
    }
    ESP_LOGW(TAG, "[iap] bootloader 在跳转指令后仍在应答");
    return ESP_OK;
}
#endif  // !defined(ARDUINO)

}  // anonymous namespace

// ============================================================
// GPIO 操作
// ============================================================

esp_err_t Py32Ioe1Device::detect() {
    uint8_t chip_id = 0;
    esp_err_t err = readRegister(REG_DEVICE_ID, &chip_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read chip id failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "%s detected (ID 0x%02X)", name(), chip_id);
    return ESP_OK;
}

esp_err_t Py32Ioe1Device::configureDefaults() {
    uint8_t all_inputs = 0x00;
    return writeRegister(REG_GPIO_DIR, &all_inputs, 1);
}

esp_err_t Py32Ioe1Device::configureOutputs(uint8_t mask, bool as_output) {
    uint8_t dirs = 0;
    esp_err_t err = readRegister(REG_GPIO_DIR, &dirs, 1);
    if (err != ESP_OK) return err;
    if (as_output) dirs |= mask;
    else           dirs &= static_cast<uint8_t>(~mask);
    return writeRegister(REG_GPIO_DIR, &dirs, 1);
}

esp_err_t Py32Ioe1Device::writeOutputs(uint8_t mask, bool high) {
    uint8_t state = 0;
    esp_err_t err = readRegister(REG_GPIO_OUT, &state, 1);
    if (err != ESP_OK) return err;
    if (high) state |= mask;
    else      state &= static_cast<uint8_t>(~mask);
    return writeRegister(REG_GPIO_OUT, &state, 1);
}

esp_err_t Py32Ioe1Device::readInputs(uint8_t &state) {
    return readRegister(REG_GPIO_IN, &state, 1);
}

// ============================================================
// I2CBusAdapter 升级（Arduino & ESP-IDF 双平台）
// ============================================================

esp_err_t Py32Ioe1Device::i2cArrayUpgrade(I2CBusAdapter &ioe1_adapter,
                                            const Py32Ioe1FirmwareImage &image,
                                            const Py32Ioe1IapOptions &options) {
    if (!ioe1_adapter.ready()) {
        ESP_LOGE(TAG, "ioe1 adapter not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (image.data == nullptr || image.length == 0) {
        ESP_LOGE(TAG, "firmware image invalid");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "[iap] array upgrade: %zu bytes", image.length);
    std::array<uint8_t, 16> fw_md5 = {0};
    if (compute_md5(image.data, image.length, fw_md5) == ESP_OK) {
        ESP_LOGI(TAG, "[iap] MD5: %s", md5_to_hex(fw_md5).c_str());
    }

    Py32I2CConfig app_cfg = ioe1_adapter.config();
    uint8_t boot_addr = BOOTLOADER_ADDRESS;
    ESP_LOGI(TAG, "[iap] app=0x%02X iap=0x%02X", app_cfg.address, boot_addr);

    // 创建 bootloader 适配器
    I2CBusAdapter boot_adapter;
    Py32I2CConfig boot_cfg = ioe1_adapter.createConfigWithAddress(boot_addr);
    esp_err_t err = boot_adapter.configure(boot_cfg);
    if (err != ESP_OK) return err;
    err = boot_adapter.begin();
    if (err != ESP_OK) return err;

    // 检查是否已在 bootloader
    bool already_boot = (ping_bootloader_via_adapter(boot_adapter) == ESP_OK);
    if (!already_boot) {
        // 触发进入 bootloader：向应用地址的 0xFD 写 0x1A
        uint8_t key = DOWNLOAD_KEY;
        err = ioe1_adapter.writeRegister(DOWNLOAD_REGISTER, &key, 1);
        if (err != ESP_OK) ESP_LOGW(TAG, "[iap] 触发下载模式失败: %s", esp_err_to_name(err));
        delay_ms(DOWNLOAD_TRIGGER_DELAY_MS);
    } else {
        ESP_LOGI(TAG, "[iap] 设备已在 bootloader 模式");
    }

    // 等待 bootloader 就绪
    err = wait_for_bootloader_via_adapter(boot_adapter, BOOTLOADER_WAIT_TIMEOUT_MS);
    if (err != ESP_OK) { boot_adapter.end(); return err; }

    // enter IAP 握手（参考 py32_func_enter_iap）
    err = enter_iap_via_adapter(boot_adapter, options.write_opcode);
    if (err != ESP_OK) { boot_adapter.end(); return err; }

    // 再次等待 bootloader 就绪（握手后等待）
    err = wait_for_bootloader_via_adapter(boot_adapter, BOOTLOADER_WAIT_TIMEOUT_MS);
    if (err != ESP_OK) { boot_adapter.end(); return err; }

    // 流式写入固件
    err = stream_firmware_via_adapter(image.data, image.length, options, boot_adapter);
    if (err != ESP_OK) { boot_adapter.end(); return err; }

    // 跳转到应用
    if (options.send_jump_command) {
        err = jump_to_application_via_adapter(boot_adapter, options.jump_opcode);
    }

    boot_adapter.end();
    if (err == ESP_OK) ESP_LOGI(TAG, "[iap] array upgrade 完成");
    return err;
}

esp_err_t Py32Ioe1Device::i2cBridgeUpgrade(I2CBusAdapter &ioe1_adapter,
                                             BusAdapter &user_input,
                                             const Py32Ioe1IapOptions &iap_options,
                                             const Py32Ioe1SerialInputOptions &input_options) {
    (void)ioe1_adapter;
    (void)user_input;
    (void)iap_options;
    (void)input_options;
    ESP_LOGW(TAG, "i2cBridgeUpgrade: not implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

#if !defined(ARDUINO)
// ============================================================
// ESP-IDF 专用升级函数
// ============================================================

esp_err_t Py32Ioe1Device::directArrayToI2cBusUpgrade(uint8_t app_i2c_addr,
                                                       const uint8_t *firmware_data,
                                                       size_t firmware_length,
                                                       i2c_bus_handle_t bus,
                                                       const Py32Ioe1IapOptions &iap_options) {
    if (bus == nullptr) {
        ESP_LOGE(TAG, "i2c bus handle invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (firmware_data == nullptr || firmware_length == 0) {
        ESP_LOGE(TAG, "firmware data invalid");
        return ESP_ERR_INVALID_ARG;
    }

    constexpr uint8_t boot_addr = BOOTLOADER_ADDRESS;
    ESP_LOGI(TAG, "[iap] directArray upgrade: %zu bytes, app=0x%02X iap=0x%02X",
             firmware_length, app_i2c_addr, boot_addr);

    std::array<uint8_t, 16> fw_md5 = {0};
    if (compute_md5(firmware_data, firmware_length, fw_md5) == ESP_OK) {
        ESP_LOGI(TAG, "[iap] MD5: %s", md5_to_hex(fw_md5).c_str());
    }

    std::array<uint8_t, 3> app_candidates{};
    size_t app_candidate_count = 0;
    auto add_candidate = [&](uint8_t app_addr) {
        for (size_t i = 0; i < app_candidate_count; ++i) {
            uint8_t candidate = app_candidates[i];
            if (candidate == app_addr) return;
        }
        if (app_candidate_count < app_candidates.size()) {
            app_candidates[app_candidate_count++] = app_addr;
        }
    };

    add_candidate(app_i2c_addr);
    // Try known IOE1 application addresses. IAP remains fixed at 0x5F.
    add_candidate(0x6F);
    add_candidate(0x4F);

    i2c_bus_device_handle_t boot_dev = nullptr;
    uint8_t selected_app_addr = app_i2c_addr;
    esp_err_t last_err = ESP_ERR_NOT_FOUND;

    auto cleanup_boot_dev = [&]() {
        if (boot_dev != nullptr) {
            i2c_bus_device_delete(&boot_dev);
            boot_dev = nullptr;
        }
    };

    boot_dev = i2c_bus_device_create(bus, boot_addr, 100000);
    if (boot_dev == nullptr) {
        ESP_LOGE(TAG, "[iap] create IAP device failed at 0x%02X", boot_addr);
        return ESP_ERR_NO_MEM;
    }

    if (ping_bootloader(boot_dev) == ESP_OK) {
        ESP_LOGI(TAG, "[iap] device already in IAP mode at 0x%02X", boot_addr);
    } else {
        bool triggered = false;
        for (size_t i = 0; i < app_candidate_count; ++i) {
            uint8_t app_addr = app_candidates[i];
            ESP_LOGI(TAG, "[iap] trigger IAP via app=0x%02X, expect iap=0x%02X",
                     app_addr, boot_addr);
            i2c_bus_device_handle_t app_dev = i2c_bus_device_create(bus, app_addr, 100000);
            if (app_dev == nullptr) {
                last_err = ESP_ERR_NO_MEM;
                ESP_LOGW(TAG, "[iap] create app device failed at 0x%02X", app_addr);
                continue;
            }

            esp_err_t trigger_err = i2c_bus_write_byte(app_dev, DOWNLOAD_REGISTER, DOWNLOAD_KEY);
            i2c_bus_device_delete(&app_dev);
            if (trigger_err != ESP_OK) {
                last_err = trigger_err;
                ESP_LOGW(TAG, "[iap] trigger IAP failed at app=0x%02X: %s",
                         app_addr, esp_err_to_name(trigger_err));
                continue;
            }

            delay_ms(DOWNLOAD_TRIGGER_DELAY_MS);

            esp_err_t boot_err = wait_for_bootloader(boot_dev, BOOTLOADER_WAIT_TIMEOUT_MS);
            if (boot_err == ESP_OK) {
                selected_app_addr = app_addr;
                triggered = true;
                break;
            }

            last_err = boot_err;
            ESP_LOGW(TAG, "[iap] IAP 0x%02X not ready after app=0x%02X trigger: %s",
                     boot_addr, app_addr, esp_err_to_name(boot_err));
        }

        if (!triggered) {
            ESP_LOGE(TAG, "[iap] no IOE1 app address entered IAP 0x%02X; tried 0x%02X, 0x6F, 0x4F",
                     boot_addr, app_i2c_addr);
            cleanup_boot_dev();
            return last_err;
        }
    }

    ESP_LOGI(TAG, "[iap] selected app=0x%02X iap=0x%02X",
             selected_app_addr, boot_addr);

    // 等待 bootloader 就绪
    esp_err_t err = wait_for_bootloader(boot_dev, BOOTLOADER_WAIT_TIMEOUT_MS);
    if (err != ESP_OK) { cleanup_boot_dev(); return err; }

    // enter IAP 握手（参考 py32_func_enter_iap）
    err = enter_iap(boot_dev, iap_options.write_opcode);
    if (err != ESP_OK) { cleanup_boot_dev(); return err; }

    // 再次等待 bootloader 就绪（握手后等待）
    err = wait_for_bootloader(boot_dev, BOOTLOADER_WAIT_TIMEOUT_MS);
    if (err != ESP_OK) { cleanup_boot_dev(); return err; }

    // 流式写入固件
    err = stream_firmware_over_i2c(firmware_data, firmware_length, iap_options, boot_dev);
    if (err != ESP_OK) { cleanup_boot_dev(); return err; }

    // 跳转到应用
    if (iap_options.send_jump_command) {
        err = jump_to_application(boot_dev, iap_options.jump_opcode);
    }

    cleanup_boot_dev();
    if (err == ESP_OK) ESP_LOGI(TAG, "[iap] directArray upgrade 完成");
    return err;
}

esp_err_t Py32Ioe1Device::uartToI2cBusUpgrade(const Py32SerialConfig &uart_config,
                                                i2c_bus_handle_t bus,
                                                const Py32Ioe1IapOptions &iap_options,
                                                const Py32Ioe1SerialInputOptions &input_options) {
    (void)uart_config; (void)bus; (void)iap_options; (void)input_options;
    ESP_LOGW(TAG, "uartToI2cBusUpgrade: not implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t Py32Ioe1Device::contextBusToI2cBusUpgrade(BusAdapter &user_input,
                                                      i2c_bus_handle_t bus,
                                                      const Py32Ioe1IapOptions &iap_options,
                                                      const Py32Ioe1SerialInputOptions &input_options) {
    (void)user_input; (void)bus; (void)iap_options; (void)input_options;
    ESP_LOGW(TAG, "contextBusToI2cBusUpgrade: not implemented");
    return ESP_ERR_NOT_SUPPORTED;
}
#endif  // !defined(ARDUINO)

}  // namespace m5::py32bin::devices

#if 0  // Stale Py32IoExt duplicate kept out of the IOE1 build.
#include <algorithm>
#include <array>
#include <cstring>
#include <string>

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
constexpr char TAG[] = "py32-ioext";
constexpr uint32_t REG_DEVICE_ID = 0x40;
constexpr uint32_t REG_GPIO_DIR = 0x50;
constexpr uint32_t REG_GPIO_OUT = 0x52;
constexpr uint32_t REG_GPIO_IN = 0x53;

// IAP 协议常量（与 PM1 相同的 PY32 IAP 协议）
constexpr uint8_t  DOWNLOAD_REGISTER        = 0xFD;
constexpr uint8_t  DOWNLOAD_KEY             = 0x1A;
// IOE1 IAP address is fixed at 0x5F.
constexpr uint8_t  BOOTLOADER_ADDRESS       = 0x5F;
constexpr uint32_t FLASH_BASE_ADDRESS       = 0x08001000;
constexpr size_t   IAP_HEADER_SIZE          = 8;
constexpr uint8_t  IAP_PADDING_BYTE         = 0xFF;
constexpr uint32_t DOWNLOAD_TRIGGER_DELAY_MS  = 200;
constexpr uint32_t BOOTLOADER_WAIT_TIMEOUT_MS = 30000;
constexpr uint32_t BOOTLOADER_POLL_INTERVAL_MS = 100;
constexpr uint32_t JUMP_RELEASE_TIMEOUT_MS  = 5000;

void delay_ms(uint32_t ms) {
    if (ms == 0) return;
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

esp_err_t compute_md5(const uint8_t *data, size_t length, std::array<uint8_t, 16> &digest) {
    if (data == nullptr || length == 0) return ESP_ERR_INVALID_ARG;
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, data, static_cast<uint32_t>(length));
    esp_rom_md5_final(digest.data(), &ctx);
    return ESP_OK;
}

std::string md5_to_hex(const std::array<uint8_t, 16> &digest) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (uint8_t b : digest) {
        out.push_back(HEX[(b >> 4) & 0x0F]);
        out.push_back(HEX[b & 0x0F]);
    }
    return out;
}

// ---- I2CBusAdapter 辅助函数 ----

esp_err_t trigger_download_mode_via_adapter(I2CBusAdapter &adapter) {
    esp_err_t err = adapter.writeRegister(DOWNLOAD_REGISTER, &DOWNLOAD_KEY, 1, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to trigger download mode: %s", esp_err_to_name(err));
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
    const uint64_t deadline = runtime_millis() + timeout_ms;
    while (runtime_millis() < deadline) {
        if (ping_bootloader_via_adapter(adapter) == ESP_OK) return ESP_OK;
        delay_ms(BOOTLOADER_POLL_INTERVAL_MS);
    }
    ESP_LOGE(TAG, "bootloader 0x%02X did not respond within %lu ms",
             BOOTLOADER_ADDRESS, static_cast<unsigned long>(timeout_ms));
    return ESP_ERR_TIMEOUT;
}

esp_err_t stream_firmware_via_adapter(const uint8_t *data, size_t length,
                                       const Py32IoExtIapOptions &options,
                                       I2CBusAdapter &adapter) {
    if (data == nullptr || length == 0) return ESP_ERR_INVALID_SIZE;
    if (options.chunk_size == 0 || options.chunk_size > 255) {
        ESP_LOGE(TAG, "invalid chunk_size %u", static_cast<unsigned>(options.chunk_size));
        return ESP_ERR_INVALID_ARG;
    }

    std::array<uint8_t, IAP_HEADER_SIZE + 255> frame{};
    const size_t total = (length + options.chunk_size - 1) / options.chunk_size;
    uint32_t address = FLASH_BASE_ADDRESS;
    size_t offset = 0;
    size_t pkt = 0;

    while (offset < length) {
        const size_t chunk = std::min(static_cast<size_t>(options.chunk_size), length - offset);
        frame[0] = options.write_opcode;
        frame[1] = static_cast<uint8_t>((address >> 24) & 0xFF);
        frame[2] = static_cast<uint8_t>((address >> 16) & 0xFF);
        frame[3] = static_cast<uint8_t>((address >> 8)  & 0xFF);
        frame[4] = static_cast<uint8_t>(address & 0xFF);
        frame[5] = 0x00;
        frame[6] = static_cast<uint8_t>(chunk & 0xFF);
        frame[7] = IAP_PADDING_BYTE;
        std::memcpy(frame.data() + IAP_HEADER_SIZE, data + offset, chunk);

        uint32_t wait_count = 0;
        while (ping_bootloader_via_adapter(adapter) != ESP_OK) {
            delay_ms(100);
            if (++wait_count >= 50) {
                ESP_LOGE(TAG, "bootloader not responding before write at 0x%08lX",
                         static_cast<unsigned long>(address));
                return ESP_ERR_TIMEOUT;
            }
        }

        esp_err_t err = adapter.writeRaw(frame.data(), IAP_HEADER_SIZE + chunk,
                                          options.transfer_timeout_ms);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c write failed at 0x%08lX: %s",
                     static_cast<unsigned long>(address), esp_err_to_name(err));
            return err;
        }

        offset  += chunk;
        address += static_cast<uint32_t>(chunk);
        ++pkt;

        const uint32_t progress_x10 = total == 0 ? 1000U :
            static_cast<uint32_t>((pkt * 1000U + total / 2U) / total);
        ESP_LOGI(TAG, "[iap] chunk %03zu/%03zu (%lu.%lu%%)",
                 pkt, total,
                 static_cast<unsigned long>(progress_x10 / 10U),
                 static_cast<unsigned long>(progress_x10 % 10U));

        if (options.chunk_delay_ms > 0) delay_ms(options.chunk_delay_ms);
    }
    return ESP_OK;
}

esp_err_t jump_to_application_via_adapter(I2CBusAdapter &adapter, uint8_t opcode) {
    esp_err_t err = adapter.writeRaw(&opcode, 1, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to send jump opcode: %s", esp_err_to_name(err));
        return err;
    }
    const uint64_t deadline = runtime_millis() + JUMP_RELEASE_TIMEOUT_MS;
    while (runtime_millis() < deadline) {
        if (ping_bootloader_via_adapter(adapter) != ESP_OK) {
            ESP_LOGI(TAG, "bootloader released bus, app is running");
            return ESP_OK;
        }
        adapter.writeRaw(&opcode, 1, 1000);
        delay_ms(200);
    }
    ESP_LOGW(TAG, "bootloader still responding after jump command");
    return ESP_OK;
}

#if !defined(ARDUINO)
// ---- ESP-IDF i2c_bus 辅助函数 ----

esp_err_t ping_bootloader(i2c_bus_device_handle_t dev) {
    uint8_t dummy = 0;
    return i2c_bus_read_byte(dev, NULL_I2C_MEM_ADDR, &dummy);
}

esp_err_t wait_for_bootloader_ready(i2c_bus_device_handle_t dev, uint32_t timeout_ms) {
    const uint64_t deadline = runtime_millis() + timeout_ms;
    while (runtime_millis() < deadline) {
        if (ping_bootloader(dev) == ESP_OK) return ESP_OK;
        delay_ms(BOOTLOADER_POLL_INTERVAL_MS);
    }
    ESP_LOGE(TAG, "bootloader 0x%02X did not respond", BOOTLOADER_ADDRESS);
    return ESP_ERR_TIMEOUT;
}

esp_err_t stream_firmware_over_i2c(const uint8_t *data, size_t length,
                                    const Py32IoExtIapOptions &options,
                                    i2c_bus_device_handle_t dev) {
    if (data == nullptr || length == 0) return ESP_ERR_INVALID_SIZE;
    if (options.chunk_size == 0 || options.chunk_size > 255) return ESP_ERR_INVALID_ARG;

    std::array<uint8_t, IAP_HEADER_SIZE + 255> frame{};
    const size_t total = (length + options.chunk_size - 1) / options.chunk_size;
    uint32_t address = FLASH_BASE_ADDRESS;
    size_t offset = 0;
    size_t pkt = 0;

    while (offset < length) {
        const size_t chunk = std::min(static_cast<size_t>(options.chunk_size), length - offset);
        frame[0] = options.write_opcode;
        frame[1] = static_cast<uint8_t>((address >> 24) & 0xFF);
        frame[2] = static_cast<uint8_t>((address >> 16) & 0xFF);
        frame[3] = static_cast<uint8_t>((address >> 8)  & 0xFF);
        frame[4] = static_cast<uint8_t>(address & 0xFF);
        frame[5] = 0x00;
        frame[6] = static_cast<uint8_t>(chunk & 0xFF);
        frame[7] = IAP_PADDING_BYTE;
        std::memcpy(frame.data() + IAP_HEADER_SIZE, data + offset, chunk);

        uint32_t wait_count = 0;
        while (ping_bootloader(dev) != ESP_OK) {
            delay_ms(100);
            if (++wait_count >= 50) {
                ESP_LOGE(TAG, "bootloader not responding before write at 0x%08lX",
                         static_cast<unsigned long>(address));
                return ESP_ERR_TIMEOUT;
            }
        }

        esp_err_t err = i2c_bus_write_bytes(dev, NULL_I2C_MEM_ADDR,
                                             IAP_HEADER_SIZE + chunk, frame.data());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c write failed at 0x%08lX: %s",
                     static_cast<unsigned long>(address), esp_err_to_name(err));
            return err;
        }

        offset  += chunk;
        address += static_cast<uint32_t>(chunk);
        ++pkt;

        const uint32_t progress_x10 = total == 0 ? 1000U :
            static_cast<uint32_t>((pkt * 1000U + total / 2U) / total);
        ESP_LOGI(TAG, "[iap] chunk %03zu/%03zu (%lu.%lu%%)",
                 pkt, total,
                 static_cast<unsigned long>(progress_x10 / 10U),
                 static_cast<unsigned long>(progress_x10 % 10U));

        if (options.chunk_delay_ms > 0) delay_ms(options.chunk_delay_ms);
    }
    return ESP_OK;
}

esp_err_t jump_to_application(i2c_bus_device_handle_t dev, uint8_t opcode) {
    esp_err_t err = i2c_bus_write_bytes(dev, NULL_I2C_MEM_ADDR, 1, &opcode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to send jump opcode: %s", esp_err_to_name(err));
        return err;
    }
    const uint64_t deadline = runtime_millis() + JUMP_RELEASE_TIMEOUT_MS;
    while (runtime_millis() < deadline) {
        if (ping_bootloader(dev) != ESP_OK) {
            ESP_LOGI(TAG, "bootloader released bus, app is running");
            return ESP_OK;
        }
        i2c_bus_write_bytes(dev, NULL_I2C_MEM_ADDR, 1, &opcode);
        delay_ms(200);
    }
    ESP_LOGW(TAG, "bootloader still responding after jump command");
    return ESP_OK;
}
#endif  // !defined(ARDUINO)

}  // anonymous namespace

// ---- GPIO 操作 ----

esp_err_t Py32IoExtDevice::detect() {
    uint8_t chip_id = 0;
    esp_err_t err = readRegister(REG_DEVICE_ID, &chip_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read chip id: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "%s detected (ID 0x%02X)", name(), chip_id);
    return ESP_OK;
}

esp_err_t Py32IoExtDevice::configureDefaults() {
    uint8_t all_inputs = 0x00;
    return writeRegister(REG_GPIO_DIR, &all_inputs, 1);
}

esp_err_t Py32IoExtDevice::configureOutputs(uint8_t mask, bool as_output) {
    uint8_t dirs = 0;
    esp_err_t err = readRegister(REG_GPIO_DIR, &dirs, 1);
    if (err != ESP_OK) return err;
    if (as_output) {
        dirs |= mask;
    } else {
        dirs &= static_cast<uint8_t>(~mask);
    }
    return writeRegister(REG_GPIO_DIR, &dirs, 1);
}

esp_err_t Py32IoExtDevice::writeOutputs(uint8_t mask, bool high) {
    uint8_t state = 0;
    esp_err_t err = readRegister(REG_GPIO_OUT, &state, 1);
    if (err != ESP_OK) return err;
    if (high) {
        state |= mask;
    } else {
        state &= static_cast<uint8_t>(~mask);
    }
    return writeRegister(REG_GPIO_OUT, &state, 1);
}

esp_err_t Py32IoExtDevice::readInputs(uint8_t &state) {
    return readRegister(REG_GPIO_IN, &state, 1);
}

// ---- I2CBusAdapter 升级函数（Arduino & ESP-IDF 双平台）----

esp_err_t Py32IoExtDevice::i2cArrayUpgrade(I2CBusAdapter &ioext_adapter,
                                             const Py32IoExtFirmwareImage &image,
                                             const Py32IoExtIapOptions &options) {
    if (!ioext_adapter.ready()) {
        ESP_LOGE(TAG, "IoExt I2C adapter not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (image.data == nullptr || image.length == 0) {
        ESP_LOGE(TAG, "invalid firmware image");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "[iap] starting array upgrade (%zu bytes)", image.length);

    std::array<uint8_t, 16> fw_md5 = {0};
    if (compute_md5(image.data, image.length, fw_md5) == ESP_OK) {
        ESP_LOGI(TAG, "[iap] firmware MD5: %s", md5_to_hex(fw_md5).c_str());
    }

    // Step 1: 创建 bootloader/IAP 适配器（0x5F）
    I2CBusAdapter boot_adapter;
    Py32I2CConfig boot_cfg = ioext_adapter.createConfigWithAddress(BOOTLOADER_ADDRESS);
    esp_err_t err = boot_adapter.configure(boot_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure bootloader adapter");
        return err;
    }
    err = boot_adapter.begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to begin bootloader adapter");
        return err;
    }

    // Step 2: 检查是否已在 bootloader
    bool already_boot = (ping_bootloader_via_adapter(boot_adapter) == ESP_OK);
    if (already_boot) {
        ESP_LOGI(TAG, "device already in bootloader (0x%02X)", BOOTLOADER_ADDRESS);
    } else {
        // Step 3: 触发进入 bootloader
        err = trigger_download_mode_via_adapter(ioext_adapter);
        if (err != ESP_OK) {
            boot_adapter.end();
            return err;
        }
    }

    // Step 4: 等待 bootloader 就绪
    err = wait_for_bootloader_ready_via_adapter(boot_adapter, BOOTLOADER_WAIT_TIMEOUT_MS);
    if (err != ESP_OK) {
        boot_adapter.end();
        return err;
    }

    // Step 5: 传输固件
    err = stream_firmware_via_adapter(image.data, image.length, options, boot_adapter);
    if (err != ESP_OK) {
        boot_adapter.end();
        return err;
    }

    // Step 6: 跳转到应用
    if (options.send_jump_command) {
        err = jump_to_application_via_adapter(boot_adapter, options.jump_opcode);
        if (err != ESP_OK) {
            boot_adapter.end();
            return err;
        }
    }

    boot_adapter.end();
    ESP_LOGI(TAG, "[iap] array upgrade complete");
    return ESP_OK;
}

esp_err_t Py32IoExtDevice::i2cBridgeUpgrade(I2CBusAdapter &ioext_adapter,
                                              BusAdapter &user_input,
                                              const Py32IoExtIapOptions &iap_options,
                                              const Py32IoExtSerialInputOptions &input_options) {
    if (!ioext_adapter.ready() || !user_input.ready()) {
        ESP_LOGE(TAG, "adapter not ready");
        return ESP_ERR_INVALID_STATE;
    }
    // 简单转发：接收数据后调用 i2cArrayUpgrade
    (void)input_options;
    ESP_LOGW(TAG, "i2cBridgeUpgrade: not fully implemented, use directArrayToI2cBusUpgrade");
    return ESP_ERR_NOT_SUPPORTED;
}

#if !defined(ARDUINO)
// ---- ESP-IDF 专用升级函数 ----

esp_err_t Py32IoExtDevice::directArrayToI2cBusUpgrade(const uint8_t *firmware_data,
                                                        size_t firmware_length,
                                                        i2c_bus_handle_t bus,
                                                        const Py32IoExtIapOptions &iap_options) {
    if (bus == nullptr) {
        ESP_LOGE(TAG, "invalid i2c bus handle");
        return ESP_ERR_INVALID_ARG;
    }
    if (firmware_data == nullptr || firmware_length == 0) {
        ESP_LOGE(TAG, "invalid firmware data");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "[iap] directArray upgrade: %zu bytes, boot=0x%02X",
             firmware_length, BOOTLOADER_ADDRESS);

    std::array<uint8_t, 16> fw_md5 = {0};
    if (compute_md5(firmware_data, firmware_length, fw_md5) == ESP_OK) {
        ESP_LOGI(TAG, "[iap] firmware MD5: %s", md5_to_hex(fw_md5).c_str());
    }

    // 创建应用地址设备句柄（用于触发下载模式）
    // 注意：应用地址需调用方通过 I2CBusAdapter 传入，此处用 i2c_bus 直接操作
    // 先尝试在 bootloader 地址检测是否已经在 IAP 模式
    i2c_bus_device_handle_t boot_dev = i2c_bus_device_create(bus, BOOTLOADER_ADDRESS, 100000);
    if (boot_dev == nullptr) {
        ESP_LOGE(TAG, "failed to create bootloader device at 0x%02X", BOOTLOADER_ADDRESS);
        return ESP_ERR_NO_MEM;
    }

    bool already_boot = (ping_bootloader(boot_dev) == ESP_OK);
    if (already_boot) {
        ESP_LOGI(TAG, "device already in bootloader (0x%02X)", BOOTLOADER_ADDRESS);
    } else {
        // 通过应用地址触发 IAP
        i2c_bus_device_handle_t app_dev = i2c_bus_device_create(bus, 0x4F, 100000);
        if (app_dev != nullptr) {
            esp_err_t terr = i2c_bus_write_byte(app_dev, DOWNLOAD_REGISTER, DOWNLOAD_KEY);
            if (terr != ESP_OK) {
                ESP_LOGW(TAG, "trigger download mode failed: %s", esp_err_to_name(terr));
            }
            i2c_bus_device_delete(&app_dev);
        }
        delay_ms(DOWNLOAD_TRIGGER_DELAY_MS);
    }

    esp_err_t err = wait_for_bootloader_ready(boot_dev, BOOTLOADER_WAIT_TIMEOUT_MS);
    if (err != ESP_OK) {
        i2c_bus_device_delete(&boot_dev);
        return err;
    }

    err = stream_firmware_over_i2c(firmware_data, firmware_length, iap_options, boot_dev);
    if (err != ESP_OK) {
        i2c_bus_device_delete(&boot_dev);
        return err;
    }

    if (iap_options.send_jump_command) {
        err = jump_to_application(boot_dev, iap_options.jump_opcode);
    }

    i2c_bus_device_delete(&boot_dev);
    ESP_LOGI(TAG, "[iap] directArray upgrade complete");
    return err;
}

esp_err_t Py32IoExtDevice::uartToI2cBusUpgrade(const Py32SerialConfig &uart_config,
                                                  i2c_bus_handle_t bus,
                                                  const Py32IoExtIapOptions &iap_options,
                                                  const Py32IoExtSerialInputOptions &input_options) {
    (void)uart_config;
    (void)bus;
    (void)iap_options;
    (void)input_options;
    ESP_LOGW(TAG, "uartToI2cBusUpgrade: not implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t Py32IoExtDevice::contextBusToI2cBusUpgrade(BusAdapter &user_input,
                                                       i2c_bus_handle_t bus,
                                                       const Py32IoExtIapOptions &iap_options,
                                                       const Py32IoExtSerialInputOptions &input_options) {
    (void)user_input;
    (void)bus;
    (void)iap_options;
    (void)input_options;
    ESP_LOGW(TAG, "contextBusToI2cBusUpgrade: not implemented");
    return ESP_ERR_NOT_SUPPORTED;
}
#endif  // !defined(ARDUINO)

}  // namespace m5::py32bin::devices
#endif  // stale Py32IoExt duplicate
