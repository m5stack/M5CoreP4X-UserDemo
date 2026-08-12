#include "m5_corep4_component.h"
#include <unistd.h>  // unlink()
#include <inttypes.h>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"
#include "esp_private/msc_scsi_bot.h"
#include "ff.h"
#include "esp_timer.h"
#include "esp_random.h"

static const char *MSC_TAG = "usb_msc_demo";

enum class msc_capacity_msg_id_t : uint8_t {
    DeviceConnected = 0,
    DeviceDisconnected,
};

typedef struct {
    msc_capacity_msg_id_t id;
    union {
        uint8_t address;
        msc_host_device_handle_t device;
    } data;
} msc_capacity_msg_t;

static QueueHandle_t s_msc_event_queue = nullptr;
static TaskHandle_t s_usb_host_task_handle = nullptr;
static bool s_msc_demo_started = false;
static msc_host_device_handle_t s_active_msc_device = nullptr;
static constexpr size_t MAX_PARTITIONS = 8;
static msc_host_vfs_handle_t s_vfs_handles[MAX_PARTITIONS] = {nullptr};
static uint8_t s_mounted_partition_count = 0;
static EventGroupHandle_t s_usb_host_event_group = nullptr;
static constexpr EventBits_t USB_HOST_READY_BIT = BIT0;
static constexpr const char* MSC_MOUNT_BASE = "/usb";

static void usb_host_background_task(void *arg);
static void msc_capacity_demo_task(void *arg);
static void msc_event_callback(const msc_host_event_t *event, void *arg);
static void log_descriptor_string(const wchar_t *descriptor, const char *label);
static void log_device_capacity(const msc_host_device_info_t &info);
static void run_usb_speed_test(const char *mount_point);

struct PartitionInfo {
    uint8_t partition_index;
    uint32_t first_lba;
    uint32_t sector_count;
    std::string type_name;
    std::string label;
    bool bootable;
};

static std::vector<PartitionInfo> detect_partitions(msc_host_device_handle_t device,
                                                    const msc_host_device_info_t &info);
static void log_partition_info(const PartitionInfo &info, const char *mount_point, uint32_t sector_size);

static void log_descriptor_string(const wchar_t *descriptor, const char *label)
{
#if CONFIG_NEWLIB_NANO_FORMAT
    (void)descriptor;
    (void)label;
#else
    if (descriptor == nullptr || descriptor[0] == 0) {
        return;
    }

    constexpr size_t kBufferLen = MSC_STR_DESC_SIZE * 4;
    char buffer[kBufferLen] = {0};
    size_t converted = wcstombs(buffer, descriptor, kBufferLen - 1);
    if (converted == static_cast<size_t>(-1)) {
        ESP_LOGW(MSC_TAG, "Failed to convert %s string descriptor", label);
        return;
    }
    buffer[converted] = '\0';
    ESP_LOGI(MSC_TAG, "%s: %s", label, buffer);
#endif
}

static uint16_t read_le16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

static uint32_t read_le32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
}

static uint64_t read_le64(const uint8_t *data)
{
    uint64_t low = read_le32(data);
    uint64_t high = read_le32(data + 4);
    return low | (high << 32);
}

static const char *mbr_type_to_string(uint8_t type)
{
    switch (type) {
        case 0x01: return "FAT12";
        case 0x04: return "FAT16<32M";
        case 0x06: return "FAT16";
        case 0x07: return "NTFS/ExFAT";
        case 0x0B: return "FAT32 CHS";
        case 0x0C: return "FAT32 LBA";
        case 0x0E: return "FAT16 LBA";
        case 0x0F: return "Extended";
        case 0xEE: return "GPT Protective";
        default: return "Other";
    }
}

static void append_utf8(std::string &out, uint16_t codepoint)
{
    if (codepoint < 0x80) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

static std::string utf16le_to_utf8(const uint8_t *data, size_t bytes)
{
    std::string result;
    result.reserve(bytes / 2);
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint16_t codepoint = read_le16(&data[i]);
        if (codepoint == 0) {
            break;
        }
        append_utf8(result, codepoint);
    }
    return result;
}

static std::string guid_to_string(const uint8_t *guid)
{
    uint32_t data1 = read_le32(guid);
    uint16_t data2 = read_le16(guid + 4);
    uint16_t data3 = read_le16(guid + 6);
    const uint8_t *data4 = guid + 8;

    char buffer[37];
    snprintf(buffer, sizeof(buffer),
             "%08" PRIX32 "-%04" PRIX16 "-%04" PRIX16 "-%02X%02X-%02X%02X%02X%02X%02X%02X",
             data1, data2, data3,
             data4[0], data4[1],
             data4[2], data4[3], data4[4], data4[5], data4[6], data4[7]);
    return std::string(buffer);
}

static void add_mbr_partitions(const uint8_t *sector_data, std::vector<PartitionInfo> &partitions)
{
    constexpr size_t kEntrySize = 16;
    constexpr size_t kEntryOffset = 446;
    for (uint8_t i = 0; i < 4 && partitions.size() < MAX_PARTITIONS; ++i) {
        const uint8_t *entry = sector_data + kEntryOffset + (i * kEntrySize);
        uint8_t type = entry[4];
        uint32_t first_lba = read_le32(entry + 8);
        uint32_t sector_count = read_le32(entry + 12);
        if (type == 0 || sector_count == 0) {
            continue;
        }
        bool bootable = (entry[0] == 0x80);

        PartitionInfo info = {
            .partition_index = i,
            .first_lba = first_lba,
            .sector_count = sector_count,
            .type_name = mbr_type_to_string(type),
            .label = "",
            .bootable = bootable,
        };
        partitions.push_back(info);
    }
}

static bool add_gpt_partitions(msc_host_device_handle_t device,
                               uint32_t sector_size,
                               std::vector<PartitionInfo> &partitions)
{
    std::vector<uint8_t> header(static_cast<size_t>(sector_size));
    if (scsi_cmd_read10(device, header.data(), 1, 1, sector_size) != ESP_OK) {
        ESP_LOGW(MSC_TAG, "Failed to read GPT header");
        return false;
    }

    if (memcmp(header.data(), "EFI PART", 8) != 0) {
        return false;
    }

    uint32_t entry_size = read_le32(header.data() + 84);
    uint32_t entry_count = read_le32(header.data() + 80);
    uint64_t entry_lba = read_le64(header.data() + 72);

    if (entry_size == 0 || entry_size > 512 || entry_count == 0) {
        ESP_LOGW(MSC_TAG, "GPT header has unsupported parameters (entry_size=%" PRIu32 ")", entry_size);
        return false;
    }

    if (entry_lba > UINT32_MAX) {
        ESP_LOGW(MSC_TAG, "GPT entry LBA exceeds 32-bit range");
        return false;
    }

    uint32_t entries_to_scan = std::min<uint32_t>(entry_count, static_cast<uint32_t>(MAX_PARTITIONS));
    uint32_t bytes_to_read = entries_to_scan * entry_size;
    uint32_t sectors_to_read = (bytes_to_read + sector_size - 1) / sector_size;

    std::vector<uint8_t> entry_buffer(static_cast<size_t>(sectors_to_read) * static_cast<size_t>(sector_size));
    for (uint32_t i = 0; i < sectors_to_read; ++i) {
        if (scsi_cmd_read10(device,
                            entry_buffer.data() + (i * sector_size),
                            static_cast<uint32_t>(entry_lba + i),
                            1,
                            sector_size) != ESP_OK) {
            ESP_LOGW(MSC_TAG, "Failed to read GPT entries");
            return false;
        }
    }

    for (uint32_t i = 0; i < entries_to_scan && partitions.size() < MAX_PARTITIONS; ++i) {
        const uint8_t *entry = entry_buffer.data() + (i * entry_size);
        bool entry_unused = true;
        for (uint32_t b = 0; b < entry_size; ++b) {
            if (entry[b] != 0) {
                entry_unused = false;
                break;
            }
        }
        if (entry_unused) {
            continue;
        }

        uint64_t first_lba = read_le64(entry + 32);
        uint64_t last_lba = read_le64(entry + 40);
        if (last_lba < first_lba) {
            continue;
        }

        uint64_t sector_count = last_lba - first_lba + 1;
        if (first_lba > UINT32_MAX || sector_count > UINT32_MAX) {
            ESP_LOGW(MSC_TAG, "Skipping GPT partition %u due to 32-bit limit", static_cast<unsigned>(i));
            continue;
        }
        PartitionInfo info = {
            .partition_index = static_cast<uint8_t>(i),
            .first_lba = static_cast<uint32_t>(first_lba),
            .sector_count = static_cast<uint32_t>(sector_count),
            .type_name = guid_to_string(entry),
            .label = utf16le_to_utf8(entry + 56, 72),
            .bootable = false,
        };
        partitions.push_back(info);
    }

    return !partitions.empty();
}

static std::vector<PartitionInfo> detect_partitions(msc_host_device_handle_t device,
                                                    const msc_host_device_info_t &info)
{
    std::vector<PartitionInfo> partitions;
    std::vector<uint8_t> sector(static_cast<size_t>(info.sector_size));
        if (scsi_cmd_read10(device, sector.data(), 0, 1, info.sector_size) != ESP_OK) {
        ESP_LOGW(MSC_TAG, "Failed to read MBR sector");
        return partitions;
    }

    const bool has_signature = sector[info.sector_size - 2] == 0x55 && sector[info.sector_size - 1] == 0xAA;
    if (!has_signature) {
        ESP_LOGW(MSC_TAG, "MBR signature missing; treating disk as single volume");
        return partitions;
    }

    add_mbr_partitions(sector.data(), partitions);

    const bool protective_gpt = std::any_of(partitions.begin(), partitions.end(), [](const PartitionInfo &p) {
        return p.type_name == std::string("GPT Protective");
    });

    if (protective_gpt) {
        partitions.clear();
        if (!add_gpt_partitions(device, info.sector_size, partitions)) {
            ESP_LOGW(MSC_TAG, "Failed to parse GPT partitions; falling back to whole disk");
        }
    }

    return partitions;
}

static void log_device_capacity(const msc_host_device_info_t &info)
{
    const uint64_t total_capacity_bytes = static_cast<uint64_t>(info.sector_size) * info.sector_count;
    const double total_capacity_gb = static_cast<double>(total_capacity_bytes) / (1024.0 * 1024.0 * 1024.0);

    ESP_LOGI(MSC_TAG, "================ USB U-disk info ================");
    ESP_LOGI(MSC_TAG, "VID:PID = 0x%04X:0x%04X",
             static_cast<unsigned>(info.idVendor),
             static_cast<unsigned>(info.idProduct));
    log_descriptor_string(info.iManufacturer, "Manufacturer");
    log_descriptor_string(info.iProduct, "Product");
    log_descriptor_string(info.iSerialNumber, "Serial number");
    ESP_LOGI(MSC_TAG, "Hardware Total Capacity: %.2f GB", total_capacity_gb);
    ESP_LOGI(MSC_TAG, "Sector size: %" PRIu32 " bytes, Sector count: %" PRIu32, info.sector_size, info.sector_count);
    ESP_LOGI(MSC_TAG, "-------------------------------------------------");
}

static void log_partition_info(const PartitionInfo &info, const char *mount_point, uint32_t sector_size)
{
    const uint64_t total_bytes = static_cast<uint64_t>(info.sector_count) * sector_size;
    const double capacity_gb = static_cast<double>(total_bytes) / (1024.0 * 1024.0 * 1024.0);

    ESP_LOGI(MSC_TAG, "Partition %u mounted at: %s",
             static_cast<unsigned>(info.partition_index),
             mount_point);
    ESP_LOGI(MSC_TAG, "  Type: %s%s", info.type_name.c_str(), info.bootable ? " (bootable)" : "");
    if (!info.label.empty()) {
        ESP_LOGI(MSC_TAG, "  Label: %s", info.label.c_str());
    }
    ESP_LOGI(MSC_TAG, "  Size: %.2f GB (start LBA=%" PRIu32 ", sectors=%" PRIu32 ")",
             capacity_gb, info.first_lba, info.sector_count);

    FATFS *fs;
    DWORD free_clusters;
    if (f_getfree(mount_point, &free_clusters, &fs) == FR_OK) {
        const char *fs_type = "Unknown";
        if (fs->fs_type == FS_FAT12) {
            fs_type = "FAT12";
        } else if (fs->fs_type == FS_FAT16) {
            fs_type = "FAT16";
        } else if (fs->fs_type == FS_FAT32) {
            fs_type = "FAT32";
        } else if (fs->fs_type == FS_EXFAT) {
            fs_type = "exFAT";
        }

        uint64_t total_sectors = (fs->n_fatent - 2) * fs->csize;
        uint64_t free_sectors = free_clusters * fs->csize;
        uint64_t used_sectors = total_sectors - free_sectors;

        uint64_t total_bytes_fs = total_sectors * fs->ssize;
        uint64_t used_bytes = used_sectors * fs->ssize;
        double total_gb = static_cast<double>(total_bytes_fs) / (1024.0 * 1024.0 * 1024.0);
        double used_gb = static_cast<double>(used_bytes) / (1024.0 * 1024.0 * 1024.0);
        double usage_percent = total_bytes_fs > 0 ? (static_cast<double>(used_bytes) / total_bytes_fs) * 100.0 : 0.0;

        ESP_LOGI(MSC_TAG, "  Filesystem: %s", fs_type);
        ESP_LOGI(MSC_TAG, "  Usage: %.2f%% (%.2f GB / %.2f GB used)", usage_percent, used_gb, total_gb);
    } else {
        ESP_LOGW(MSC_TAG, "Failed to read filesystem usage for %s", mount_point);
    }
}

static void msc_event_callback(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (!event || s_msc_event_queue == nullptr) {
        return;
    }

    msc_capacity_msg_t msg = {};
    switch (event->event) {
        case 0: // See enum inside msc_host_event_t definition
            msg.id = msc_capacity_msg_id_t::DeviceConnected;
            msg.data.address = event->device.address;
            break;
        case 1:
            msg.id = msc_capacity_msg_id_t::DeviceDisconnected;
            msg.data.device = event->device.handle;
            break;
        default:
            return;
    }

    if (xQueueSend(s_msc_event_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(MSC_TAG, "MSC event queue full, event dropped");
    }
}

static void usb_host_background_task(void *arg)
{
    (void)arg;
    usb_host_config_t host_config;
    memset(&host_config, 0, sizeof(host_config));
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    host_config.peripheral_map = (1U << 0); // ESP32-P4 USB-A 2.0 host uses HS DWC/UTMI.

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(MSC_TAG, "Failed to install USB host library: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    if (s_usb_host_event_group) {
        xEventGroupSetBits(s_usb_host_event_group, USB_HOST_READY_BIT);
    }

    ESP_LOGI(MSC_TAG, "USB host library installed, waiting for devices...");

    while (true) {
        uint32_t event_flags = 0;
        err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(MSC_TAG, "usb_host_lib_handle_events returned %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            usb_host_device_free_all();
        }
    }
}

static void msc_capacity_demo_task(void *arg)
{
    (void)arg;

    if (s_usb_host_event_group == nullptr) {
        ESP_LOGE(MSC_TAG, "USB host event group missing, aborting MSC task");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(MSC_TAG, "Waiting for USB host stack to be ready...");
    xEventGroupWaitBits(s_usb_host_event_group,
                        USB_HOST_READY_BIT,
                        pdFALSE,
                        pdTRUE,
                        portMAX_DELAY);

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = msc_event_callback,
        .callback_arg = nullptr,
    };

    esp_err_t err = msc_host_install(&msc_config);
    if (err != ESP_OK) {
        ESP_LOGE(MSC_TAG, "Failed to install MSC host driver: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(MSC_TAG, "MSC host driver ready. Plug in a USB drive to read its capacity.");

    while (true) {
        msc_capacity_msg_t msg;
        if (xQueueReceive(s_msc_event_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (msg.id == msc_capacity_msg_id_t::DeviceConnected) {
            if (s_active_msc_device != nullptr) {
                ESP_LOGW(MSC_TAG, "Another MSC device is already active. Disconnect it first.");
                continue;
            }

            esp_err_t install_ret = msc_host_install_device(msg.data.address, &s_active_msc_device);
            if (install_ret != ESP_OK) {
                ESP_LOGE(MSC_TAG, "Failed to install MSC device: %s", esp_err_to_name(install_ret));
                s_active_msc_device = nullptr;
                continue;
            }

            // Query device info first
            msc_host_device_info_t device_info = {};
            esp_err_t info_ret = msc_host_get_device_info(s_active_msc_device, &device_info);
            if (info_ret != ESP_OK) {
                ESP_LOGE(MSC_TAG, "Failed to query MSC device info: %s", esp_err_to_name(info_ret));
                msc_host_uninstall_device(s_active_msc_device);
                s_active_msc_device = nullptr;
                continue;
            }

            // Reset mount bookkeeping
            s_mounted_partition_count = 0;
            for (auto &handle : s_vfs_handles) {
                handle = nullptr;
            }

            log_device_capacity(device_info);
            ESP_LOGI(MSC_TAG, "Enumerating partitions...");

            std::vector<PartitionInfo> partitions = detect_partitions(s_active_msc_device, device_info);
            if (partitions.empty()) {
                PartitionInfo whole_disk = {
                    .partition_index = 0,
                    .first_lba = 0,
                    .sector_count = device_info.sector_count,
                    .type_name = "WholeDisk",
                    .label = "",
                    .bootable = false,
                };
                partitions.push_back(whole_disk);
            }

            const size_t mount_limit = std::min({partitions.size(), static_cast<size_t>(MAX_PARTITIONS), static_cast<size_t>(CONFIG_FATFS_VOLUME_COUNT)});
            if (partitions.size() > mount_limit) {
                ESP_LOGW(MSC_TAG, "Detected %u partitions but can only mount %u with current FATFS volume setting",
                         static_cast<unsigned>(partitions.size()), static_cast<unsigned>(mount_limit));
            }

            esp_vfs_fat_mount_config_t mount_config;
            memset(&mount_config, 0, sizeof(mount_config));
            mount_config.format_if_mount_failed = false;
            mount_config.max_files = 3;
            mount_config.allocation_unit_size = 8192;
            mount_config.disk_status_check_enable = false;

            for (size_t idx = 0; idx < mount_limit; ++idx) {
                const PartitionInfo &partition = partitions[idx];
                char mount_point[16];
                snprintf(mount_point, sizeof(mount_point), "%s%u", MSC_MOUNT_BASE, static_cast<unsigned>(idx));

                esp_err_t mount_ret = ESP_FAIL;
                msc_host_vfs_handle_t handle = nullptr;

                if (partition.first_lba == 0 && partition.sector_count == device_info.sector_count && partitions.size() == 1) {
                    mount_ret = msc_host_vfs_register(s_active_msc_device, mount_point, &mount_config, &handle);
                } else {
                    msc_host_partition_desc_t desc = {
                        .first_sector = partition.first_lba,
                        .sector_count = partition.sector_count,
                    };
                    mount_ret = msc_host_vfs_register_partition(s_active_msc_device, mount_point, &mount_config, &desc, &handle);
                }

                if (mount_ret == ESP_OK) {
                    s_vfs_handles[s_mounted_partition_count] = handle;
                    s_mounted_partition_count++;
                    log_partition_info(partition, mount_point, device_info.sector_size);
                } else {
                    ESP_LOGW(MSC_TAG, "Failed to mount partition %u (LBA=%" PRIu32 "): %s",
                             static_cast<unsigned>(partition.partition_index),
                             partition.first_lba,
                             esp_err_to_name(mount_ret));
                }
            }

            if (s_mounted_partition_count == 0) {
                ESP_LOGE(MSC_TAG, "No partitions could be mounted!");
                ESP_LOGI(MSC_TAG, "==================================================");
                msc_host_uninstall_device(s_active_msc_device);
                s_active_msc_device = nullptr;
            } else {
                ESP_LOGI(MSC_TAG, "Mounted %u partition(s)", static_cast<unsigned>(s_mounted_partition_count));
                ESP_LOGI(MSC_TAG, "==================================================");
                
                // Run speed test on the largest partition (usually the first one for U-disks)
                // Find partition with largest sector count
                int best_idx = -1;
                uint32_t max_sectors = 0;
                
                for (size_t i = 0; i < mount_limit; i++) {
                    if (s_vfs_handles[i] != nullptr) {
                        // We don't have easy access to partition info here without re-scanning or storing it
                        // But we know s_vfs_handles corresponds to partitions[i] if mounted
                        if (partitions[i].sector_count > max_sectors) {
                            max_sectors = partitions[i].sector_count;
                            best_idx = i;
                        }
                    }
                }

                if (best_idx >= 0) {
                    char mount_point[16];
                    snprintf(mount_point, sizeof(mount_point), "%s%u", MSC_MOUNT_BASE, static_cast<unsigned>(best_idx));
                    run_usb_speed_test(mount_point);
                }
            }
        } else if (msg.id == msc_capacity_msg_id_t::DeviceDisconnected) {
            if (s_active_msc_device == nullptr) {
                ESP_LOGW(MSC_TAG, "MSC disconnect event received but no active device");
                continue;
            }

            if (msg.data.device == s_active_msc_device) {
                ESP_LOGI(MSC_TAG, "USB mass storage device removed");
                
                // Unmount all VFS partitions
                for (uint8_t i = 0; i < s_mounted_partition_count; i++) {
                    if (s_vfs_handles[i] != nullptr) {
                        esp_err_t unmount_ret = msc_host_vfs_unregister(s_vfs_handles[i]);
                        if (unmount_ret != ESP_OK) {
                            ESP_LOGW(MSC_TAG, "Failed to unmount partition %u: %s",
                                     static_cast<unsigned>(i),
                                     esp_err_to_name(unmount_ret));
                        }
                        s_vfs_handles[i] = nullptr;
                    }
                }
                s_mounted_partition_count = 0;
                
                esp_err_t uninstall_ret = msc_host_uninstall_device(s_active_msc_device);
                if (uninstall_ret != ESP_OK) {
                    ESP_LOGW(MSC_TAG, "Failed to uninstall MSC device: %s", esp_err_to_name(uninstall_ret));
                }
                s_active_msc_device = nullptr;
            }
        }
    }
}

void start_usb_msc_capacity_demo()
{
    if (s_msc_demo_started) {
        ESP_LOGW(MSC_TAG, "USB MSC capacity demo already running");
        return;
    }

    s_msc_event_queue = xQueueCreate(5, sizeof(msc_capacity_msg_t));
    if (s_msc_event_queue == nullptr) {
        ESP_LOGE(MSC_TAG, "Failed to create MSC event queue");
        return;
    }

    s_usb_host_event_group = xEventGroupCreate();
    if (s_usb_host_event_group == nullptr) {
        ESP_LOGE(MSC_TAG, "Failed to create USB host event group");
        vQueueDelete(s_msc_event_queue);
        s_msc_event_queue = nullptr;
        return;
    }

    if (xTaskCreate(usb_host_background_task, "usb_host_events", 4096, nullptr, 4, &s_usb_host_task_handle) != pdPASS) {
        ESP_LOGE(MSC_TAG, "Failed to create USB host task");
        vQueueDelete(s_msc_event_queue);
        s_msc_event_queue = nullptr;
        vEventGroupDelete(s_usb_host_event_group);
        s_usb_host_event_group = nullptr;
        return;
    }

    if (xTaskCreate(msc_capacity_demo_task, "usb_msc_capacity", 4096, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(MSC_TAG, "Failed to create MSC capacity task");
        vTaskDelete(s_usb_host_task_handle);
        s_usb_host_task_handle = nullptr;
        vQueueDelete(s_msc_event_queue);
        s_msc_event_queue = nullptr;
        vEventGroupDelete(s_usb_host_event_group);
        s_usb_host_event_group = nullptr;
        return;
    }

    s_msc_demo_started = true;
    ESP_LOGI(MSC_TAG, "USB MSC capacity demo started");
}

static void run_usb_speed_test(const char *mount_point)
{
    const size_t test_size = 128 * 1024 * 1024; // 128MB
    const size_t buffer_size = 64 * 1024;       // 64KB buffer
    uint8_t *buffer = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
    
    if (!buffer) {
        buffer = (uint8_t *)malloc(buffer_size);
        if (!buffer) {
            ESP_LOGE(MSC_TAG, "Failed to allocate speed test buffer");
            return;
        }
    }

    // Fill buffer with random data
    for (size_t i = 0; i < buffer_size; i += 4) {
        uint32_t r = esp_random();
        memcpy(buffer + i, &r, 4);
    }

    char file_path[64];
    snprintf(file_path, sizeof(file_path), "%s/speedtest.bin", mount_point);

    ESP_LOGI(MSC_TAG, "Starting speed test on %s (Size: %zu MB, Buffer: %zu KB)", 
             mount_point, test_size / (1024 * 1024), buffer_size / 1024);

    // Write Test
    {
        FILE *f = fopen(file_path, "wb");
        if (!f) {
            ESP_LOGE(MSC_TAG, "Failed to open file for writing: %s", file_path);
            free(buffer);
            return;
        }

        int64_t start_time = esp_timer_get_time();
        int64_t last_log_time = start_time;
        size_t written = 0;
        while (written < test_size) {
            size_t to_write = std::min(buffer_size, test_size - written);
            if (fwrite(buffer, 1, to_write, f) != to_write) {
                ESP_LOGE(MSC_TAG, "Write failed at offset %zu", written);
                break;
            }
            written += to_write;
            
            // Log progress every 5 seconds
            int64_t now = esp_timer_get_time();
            if ((now - last_log_time) >= 5000000) {
                double written_mb = written / (1024.0 * 1024.0);
                double elapsed = (now - start_time) / 1000000.0;
                double current_speed = written_mb / elapsed;
                ESP_LOGI(MSC_TAG, "Writing... %.2f MB / %.2f MB (%.2f MB/s)", 
                         written_mb, test_size / (1024.0 * 1024.0), current_speed);
                last_log_time = now;
            }
        }
        fclose(f);
        int64_t end_time = esp_timer_get_time();
        
        double duration_sec = (end_time - start_time) / 1000000.0;
        double speed_mbps = (written / (1024.0 * 1024.0)) / duration_sec;
        ESP_LOGI(MSC_TAG, "Write Speed: %.2f MB/s (%.2f sec)", speed_mbps, duration_sec);
    }

    // Read Test
    {
        FILE *f = fopen(file_path, "rb");
        if (!f) {
            ESP_LOGE(MSC_TAG, "Failed to open file for reading");
            free(buffer);
            return;
        }

        int64_t start_time = esp_timer_get_time();
        int64_t last_log_time = start_time;
        size_t read_total = 0;
        while (read_total < test_size) {
            size_t to_read = std::min(buffer_size, test_size - read_total);
            if (fread(buffer, 1, to_read, f) != to_read) {
                ESP_LOGE(MSC_TAG, "Read failed at offset %zu", read_total);
                break;
            }
            read_total += to_read;
            
            // Log progress every 5 seconds
            int64_t now = esp_timer_get_time();
            if ((now - last_log_time) >= 5000000) {
                double read_mb = read_total / (1024.0 * 1024.0);
                double elapsed = (now - start_time) / 1000000.0;
                double current_speed = read_mb / elapsed;
                ESP_LOGI(MSC_TAG, "Reading... %.2f MB / %.2f MB (%.2f MB/s)", 
                         read_mb, test_size / (1024.0 * 1024.0), current_speed);
                last_log_time = now;
            }
        }
        fclose(f);
        int64_t end_time = esp_timer_get_time();

        double duration_sec = (end_time - start_time) / 1000000.0;
        double speed_mbps = (read_total / (1024.0 * 1024.0)) / duration_sec;
        ESP_LOGI(MSC_TAG, "Read Speed:  %.2f MB/s (%.2f sec)", speed_mbps, duration_sec);
    }

    // Cleanup
    unlink(file_path);
    free(buffer);
    ESP_LOGI(MSC_TAG, "Speed test completed");
}
