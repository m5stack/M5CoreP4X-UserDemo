#include "ui_function_widgets.h"

#include "esp_timer.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_SD";
constexpr const char* TEST_FILE = "/sdcard/.ui_test_block";
constexpr size_t TEST_SIZE_BYTES = 512 * 1024;
constexpr size_t TEST_CHUNK_BYTES = 4096;
constexpr int SD_MAX_ENTRIES = 48;
constexpr size_t SD_NAME_MAX_LEN = 72;
constexpr size_t SD_PATH_MAX_LEN = 256;

enum class SdAction : uint8_t {
    UP,
    DOWN,
    OPEN,
    BACK,
};

struct SdFileEntry {
    char name[SD_NAME_MAX_LEN] = {};
    bool is_dir = false;
    uint64_t size = 0;
};

struct SdPageState {
    lv_obj_t* status_label = nullptr;
    lv_obj_t* capacity_label = nullptr;
    lv_obj_t* size_label = nullptr;
    lv_obj_t* path_label = nullptr;
    lv_obj_t* list_container = nullptr;
    lv_obj_t* up_button = nullptr;
    lv_obj_t* down_button = nullptr;
    lv_obj_t* open_button = nullptr;
    lv_obj_t* back_button = nullptr;
    lv_obj_t* header_test_button = nullptr;
    CoreP4FunctionDialogHandle test_dialog = {};
    char current_dir[SD_PATH_MAX_LEN] = "/sdcard";
    SdFileEntry entries[SD_MAX_ENTRIES] = {};
    int entry_count = 0;
    int selected_index = -1;
    volatile bool page_alive = false;
    volatile bool task_running = false;
    uint32_t last_checksum = 0;
    bool checksum_valid = false;
};

SdPageState s_sd_page;

void format_bytes(uint64_t bytes, char* out, size_t out_len)
{
    double val = static_cast<double>(bytes);
    if (bytes >= 1024ULL * 1024ULL * 1024ULL * 1024ULL) {
        std::snprintf(out, out_len, "%.2f TB", val / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        std::snprintf(out, out_len, "%.2f GB", val / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        std::snprintf(out, out_len, "%.2f MB", val / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        std::snprintf(out, out_len, "%.2f KB", val / 1024.0);
    } else {
        std::snprintf(out, out_len, "%llu B", static_cast<unsigned long long>(bytes));
    }
}

void reset_obj_defaults(lv_obj_t* obj)
{
    if (!obj) {
        return;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
}

bool sd_ready()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    if (!tool || !status) {
        return false;
    }

    if (tool->is_wifi_init_running()) {
        return false;
    }

    return status->power.pyb_l3b_power &&
           status->sdcard.sd_detected &&
           status->sdcard.sd_initialized &&
           tool->otool_tools.sd_card.is_initialized() &&
           tool->otool_tools.sd_card.is_mounted();
}

void set_sd_controls_enabled(bool enabled)
{
    set_button_enabled(s_sd_page.back_button, enabled && std::strcmp(s_sd_page.current_dir, "/sdcard") != 0);
    set_button_enabled(s_sd_page.header_test_button, enabled && sd_ready());
}

void refresh_sd_status()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    if (!tool || !status) {
        set_label_text(s_sd_page.status_label, "SD: Not Ready");
        set_sd_controls_enabled(false);
        return;
    }

    bool ready = sd_ready();
    uint64_t total = ready ? status->sdcard.sd_total_bytes : 0;
    uint64_t free_bytes = ready ? status->sdcard.sd_free_bytes : 0;
    if (ready && tool->otool_tools.sd_card.get_capacity_info(&total, &free_bytes) == ESP_OK) {
        status->sdcard.sd_total_bytes = total;
        status->sdcard.sd_free_bytes = free_bytes;
        status->sdcard.sd_initialized = tool->otool_tools.sd_card.is_initialized();
        ready = sd_ready();
    }

    char status_buf[160];
    if (ready) {
        double total_gb = static_cast<double>(total) / (1024.0 * 1024.0 * 1024.0);
        double free_gb = static_cast<double>(free_bytes) / (1024.0 * 1024.0 * 1024.0);
        std::snprintf(status_buf, sizeof(status_buf), "%.2f / %.2f GB", free_gb, total_gb);
    } else {
        std::snprintf(status_buf, sizeof(status_buf), "SD: Not Ready");
    }
    set_label_text(s_sd_page.status_label, status_buf);

    set_sd_controls_enabled(!s_sd_page.task_running && ready);
}

uint32_t checksum_update(uint32_t checksum, const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        checksum = (checksum * 33U) ^ data[i];
    }
    return checksum;
}

void fill_pattern(uint8_t* data, size_t len, size_t offset)
{
    for (size_t i = 0; i < len; ++i) {
        data[i] = static_cast<uint8_t>((offset + i) & 0xFF);
    }
}

void join_path(const char* dir, const char* name, char* out, size_t out_len)
{
    if (std::strcmp(dir, "/") == 0) {
        std::snprintf(out, out_len, "/%.71s", name);
    } else {
        std::snprintf(out, out_len, "%.180s/%.71s", dir, name);
    }
}

void file_row_event_cb(lv_event_t* e);

void update_file_view()
{
    set_label_text(s_sd_page.path_label, s_sd_page.current_dir);
    if (s_sd_page.list_container) {
        lv_obj_clean(s_sd_page.list_container);
        
        if (!sd_ready()) {
            lv_obj_t* row_obj = add_label(s_sd_page.list_container, "Check L3B, card detect and mount.", COLOR_MUTED);
            lv_obj_set_style_text_font(row_obj, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(row_obj, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(row_obj, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
            lv_obj_set_style_border_width(row_obj, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(row_obj, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_radius(row_obj, 6, LV_PART_MAIN);
            lv_obj_set_style_pad_hor(row_obj, 12, LV_PART_MAIN);
            lv_obj_set_style_pad_ver(row_obj, 6, LV_PART_MAIN);
        } else if (s_sd_page.entry_count == 0) {
            lv_obj_t* row_obj = add_label(s_sd_page.list_container, "(empty)", COLOR_MUTED);
            lv_obj_set_style_text_font(row_obj, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(row_obj, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(row_obj, lv_color_hex(0xF8FAFC), LV_PART_MAIN);
            lv_obj_set_style_border_width(row_obj, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(row_obj, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_radius(row_obj, 6, LV_PART_MAIN);
            lv_obj_set_style_pad_hor(row_obj, 12, LV_PART_MAIN);
            lv_obj_set_style_pad_ver(row_obj, 6, LV_PART_MAIN);
        } else {
            for (int i = 0; i < s_sd_page.entry_count; ++i) {
                const SdFileEntry& entry = s_sd_page.entries[i];
                char size_buf[24] = "";
                if (!entry.is_dir) {
                    format_bytes(entry.size, size_buf, sizeof(size_buf));
                }
                char line[128];
                if (entry.is_dir) {
                    std::snprintf(line, sizeof(line), "[D]  %.48s", entry.name);
                } else {
                    std::snprintf(line, sizeof(line), "[F]  %.48s  (%s)", entry.name, size_buf);
                }

                lv_obj_t* row_obj = add_label(s_sd_page.list_container, line, COLOR_TEXT);
                lv_obj_set_style_text_font(row_obj, &lv_font_montserrat_14, LV_PART_MAIN);
                lv_label_set_long_mode(row_obj, LV_LABEL_LONG_DOT);
                
                lv_obj_set_style_bg_opa(row_obj, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_radius(row_obj, 6, LV_PART_MAIN);
                lv_obj_set_style_border_width(row_obj, 1, LV_PART_MAIN);
                lv_obj_set_style_pad_hor(row_obj, 12, LV_PART_MAIN);
                lv_obj_set_style_pad_ver(row_obj, 6, LV_PART_MAIN);
                
                if (i == s_sd_page.selected_index) {
                    lv_obj_set_style_bg_color(row_obj, lv_color_hex(COLOR_PRIMARY), LV_PART_MAIN);
                    lv_obj_set_style_border_color(row_obj, lv_color_hex(COLOR_PRIMARY), LV_PART_MAIN);
                    lv_obj_set_style_text_color(row_obj, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
                } else {
                    lv_obj_set_style_bg_color(row_obj, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
                    lv_obj_set_style_border_color(row_obj, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
                    lv_obj_set_style_text_color(row_obj, 
                                                lv_color_hex(entry.is_dir ? 0xD97706 : COLOR_TEXT), 
                                                LV_PART_MAIN);
                }
                
                lv_obj_add_flag(row_obj, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(row_obj, file_row_event_cb, LV_EVENT_CLICKED,
                                    reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
            }
        }
    }

    // Left size details logic based on selection and directory level
    bool is_root = (std::strcmp(s_sd_page.current_dir, "/sdcard") == 0);
    bool has_selected_file = false;
    uint64_t sel_size = 0;
    if (s_sd_page.entry_count > 0 && s_sd_page.selected_index >= 0 && s_sd_page.selected_index < s_sd_page.entry_count) {
        const auto& entry = s_sd_page.entries[s_sd_page.selected_index];
        if (!entry.is_dir) {
            has_selected_file = true;
            sel_size = entry.size;
        }
    }

    char bytes_buf[24];
    char size_buf[64] = "";
    if (has_selected_file) {
        format_bytes(sel_size, bytes_buf, sizeof(bytes_buf));
        std::snprintf(size_buf, sizeof(size_buf), "File: %s", bytes_buf);
        set_label_text(s_sd_page.size_label, size_buf);
    } else {
        if (is_root) {
            set_label_text(s_sd_page.size_label, "");
        } else {
            uint64_t total_dir_size = 0;
            for (int i = 0; i < s_sd_page.entry_count; ++i) {
                if (!s_sd_page.entries[i].is_dir) {
                    total_dir_size += s_sd_page.entries[i].size;
                }
            }
            format_bytes(total_dir_size, bytes_buf, sizeof(bytes_buf));
            std::snprintf(size_buf, sizeof(size_buf), "Folder: %s", bytes_buf);
            set_label_text(s_sd_page.size_label, size_buf);
        }
    }

    set_sd_controls_enabled(!s_sd_page.task_running && sd_ready());
}

bool load_current_dir()
{
    s_sd_page.entry_count = 0;
    s_sd_page.selected_index = -1;

    DIR* dir = opendir(s_sd_page.current_dir);
    if (!dir) {
        return false;
    }

    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr && s_sd_page.entry_count < SD_MAX_ENTRIES) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        SdFileEntry& entry = s_sd_page.entries[s_sd_page.entry_count];
        std::snprintf(entry.name, sizeof(entry.name), "%.71s", ent->d_name);

        char path[SD_PATH_MAX_LEN];
        join_path(s_sd_page.current_dir, ent->d_name, path, sizeof(path));
        struct stat st = {};
        if (stat(path, &st) == 0) {
            entry.is_dir = S_ISDIR(st.st_mode);
            entry.size = static_cast<uint64_t>(st.st_size);
        } else {
            entry.is_dir = false;
            entry.size = 0;
        }
        s_sd_page.entry_count++;
    }
    closedir(dir);

    // Sort entries: directories first, then files, both alphabetically
    std::sort(s_sd_page.entries, s_sd_page.entries + s_sd_page.entry_count, [](const SdFileEntry& a, const SdFileEntry& b) {
        if (a.is_dir != b.is_dir) {
            return a.is_dir > b.is_dir; // true (a is dir, b is file) -> directories come first
        }
        return std::strcmp(a.name, b.name) < 0;
    });

    return true;
}

void reload_file_view()
{
    if (!sd_ready()) {
        set_label_text(s_sd_page.path_label, "/sdcard not ready");
        update_file_view();
        set_sd_controls_enabled(false);
        return;
    }
    if (!load_current_dir()) {
        set_label_text(s_sd_page.path_label, "open dir failed");
        update_file_view();
        set_sd_controls_enabled(false);
        return;
    }
    update_file_view();
}

void write_test(char* out, size_t out_len, uint32_t* color)
{
    FILE* file = std::fopen(TEST_FILE, "wb");
    if (!file) {
        std::snprintf(out, out_len, "Write failed: cannot open %s", TEST_FILE);
        if (color) *color = COLOR_ERROR;
        return;
    }

    uint8_t* buf = static_cast<uint8_t*>(std::malloc(TEST_CHUNK_BYTES));
    if (!buf) {
        std::fclose(file);
        std::snprintf(out, out_len, "Write failed: no memory");
        if (color) *color = COLOR_ERROR;
        return;
    }

    int64_t start_us = esp_timer_get_time();
    size_t written_total = 0;
    uint32_t checksum = 0;
    while (written_total < TEST_SIZE_BYTES) {
        size_t to_write = TEST_SIZE_BYTES - written_total;
        if (to_write > TEST_CHUNK_BYTES) {
            to_write = TEST_CHUNK_BYTES;
        }
        fill_pattern(buf, to_write, written_total);
        checksum = checksum_update(checksum, buf, to_write);
        size_t written = std::fwrite(buf, 1, to_write, file);
        if (written != to_write) {
            std::free(buf);
            std::fclose(file);
            std::snprintf(out, out_len, "Write failed at %u/%u bytes", static_cast<unsigned>(written_total), static_cast<unsigned>(TEST_SIZE_BYTES));
            if (color) *color = COLOR_ERROR;
            return;
        }
        written_total += written;
    }
    std::fclose(file);
    std::free(buf);

    int64_t elapsed_us = esp_timer_get_time() - start_us;
    double kb_s = elapsed_us > 0 ? (static_cast<double>(TEST_SIZE_BYTES) / 1024.0) / (static_cast<double>(elapsed_us) / 1000000.0) : 0.0;
    s_sd_page.last_checksum = checksum;
    s_sd_page.checksum_valid = true;
    std::snprintf(out, out_len,
                  "Write OK: %u bytes to %s\nSpeed: %.1f kB/s\nChecksum: 0x%08lX",
                  static_cast<unsigned>(TEST_SIZE_BYTES),
                  TEST_FILE,
                  kb_s,
                  static_cast<unsigned long>(checksum));
    if (color) *color = COLOR_SUCCESS;
}

void read_test(char* out, size_t out_len, uint32_t* color)
{
    FILE* file = std::fopen(TEST_FILE, "rb");
    if (!file) {
        std::snprintf(out, out_len, "Read failed: test file not found. Run Write Test first.");
        if (color) *color = COLOR_WARNING;
        return;
    }

    uint8_t* buf = static_cast<uint8_t*>(std::malloc(TEST_CHUNK_BYTES));
    if (!buf) {
        std::fclose(file);
        std::snprintf(out, out_len, "Read failed: no memory");
        if (color) *color = COLOR_ERROR;
        return;
    }

    int64_t start_us = esp_timer_get_time();
    size_t read_total = 0;
    uint32_t checksum = 0;
    while (true) {
        size_t n = std::fread(buf, 1, TEST_CHUNK_BYTES, file);
        if (n == 0) {
            break;
        }
        checksum = checksum_update(checksum, buf, n);
        read_total += n;
    }
    std::fclose(file);
    std::free(buf);

    int64_t elapsed_us = esp_timer_get_time() - start_us;
    double kb_s = elapsed_us > 0 ? (static_cast<double>(read_total) / 1024.0) / (static_cast<double>(elapsed_us) / 1000000.0) : 0.0;
    const bool checksum_ok = !s_sd_page.checksum_valid || checksum == s_sd_page.last_checksum;
    std::snprintf(out, out_len,
                  "Read OK: %u bytes from %s\nSpeed: %.1f kB/s\nChecksum: 0x%08lX (%s)",
                  static_cast<unsigned>(read_total),
                  TEST_FILE,
                  kb_s,
                  static_cast<unsigned long>(checksum),
                  checksum_ok ? "OK" : "MISMATCH");
    if (color) *color = checksum_ok ? COLOR_SUCCESS : COLOR_ERROR;
}

void delete_test(char* out, size_t out_len, uint32_t* color)
{
    int ret = std::remove(TEST_FILE);
    if (ret == 0) {
        s_sd_page.checksum_valid = false;
        std::snprintf(out, out_len, "Deleted %s", TEST_FILE);
        if (color) *color = COLOR_SUCCESS;
    } else {
        std::snprintf(out, out_len, "Delete skipped/failed: %s not found", TEST_FILE);
        if (color) *color = COLOR_WARNING;
    }
}

void update_sd_test_dialog(const char* text)
{
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock LVGL for SD update");
        return;
    }
    if (s_sd_page.page_alive) {
        refresh_sd_status();
        corep4_ui_set_function_dialog_text(&s_sd_page.test_dialog, text);
        set_sd_controls_enabled(!s_sd_page.task_running && sd_ready());
    }
    lvgl_port_unlock();
}

void sd_benchmark_task(void* /*param*/)
{
    char progress[768];
    char step[220];
    char write_result[220] = "--";
    char read_result[220] = "--";
    uint32_t color = COLOR_ERROR;
    progress[0] = '\0';

    if (!sd_ready()) {
        std::snprintf(progress, sizeof(progress), "SD is not ready. Check L3B, card detect and mount state.");
    } else {
        std::snprintf(progress, sizeof(progress), "SD speed test\n1/3 Write %u KB...", static_cast<unsigned>(TEST_SIZE_BYTES / 1024));
        update_sd_test_dialog(progress);

        write_test(step, sizeof(step), &color);
    std::snprintf(write_result, sizeof(write_result), "%s", step);
        std::snprintf(progress, sizeof(progress), "SD speed test\n[Write]\n%s\n\n2/3 Read...", step);
        update_sd_test_dialog(progress);

        read_test(step, sizeof(step), &color);
        std::snprintf(read_result, sizeof(read_result), "%s", step);
    std::snprintf(progress, sizeof(progress), "SD speed test\n[Write]\n%s\n\n[Read]\n%s\n\n3/3 Cleanup...", write_result, read_result);
        update_sd_test_dialog(progress);

        delete_test(step, sizeof(step), &color);
        std::snprintf(progress, sizeof(progress), "SD speed test done\n[Write]\n%.220s\n\n[Read]\n%.220s\n\n[Cleanup]\n%.120s",
              write_result,
                      read_result,
                      step);
    }

    ESP_LOGI(TAG, "SD benchmark done");
    s_sd_page.task_running = false;
    update_sd_test_dialog(progress);
    vTaskDelete(nullptr);
}

void close_test_dialog_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    corep4_ui_close_function_dialog(&s_sd_page.test_dialog);
}

void test_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_sd_page.task_running) {
        return;
    }

    s_sd_page.header_test_button = static_cast<lv_obj_t*>(lv_event_get_target(e));
    CoreP4FunctionDialogButtonConfig buttons[] = {
        {"Close", close_test_dialog_event_cb, nullptr, COLOR_BUTTON_BG, COLOR_TEXT, false},
    };
    s_sd_page.test_dialog = corep4_ui_open_function_dialog("SD Speed Test",
                                                           "Preparing SD speed test...",
                                                           CoreP4FunctionDialogSize::LARGE,
                                                           buttons,
                                                           1);

    s_sd_page.task_running = true;
    set_sd_controls_enabled(false);

    BaseType_t ret = xTaskCreate(sd_benchmark_task,
                                 "func_sd_test",
                                 6144,
                                 nullptr,
                                 configMAX_PRIORITIES - 4,
                                 nullptr);
    if (ret != pdPASS) {
        s_sd_page.task_running = false;
        refresh_sd_status();
        corep4_ui_set_function_dialog_text(&s_sd_page.test_dialog, "SD test failed: cannot create worker task.");
    }
}

void file_row_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) {
        return;
    }

    int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index < 0 || index >= s_sd_page.entry_count) {
        return;
    }

    const SdFileEntry& entry = s_sd_page.entries[index];
    uint32_t now = lv_tick_get();

    static uint32_t last_click_time = 0;
    static int last_clicked_index = -1;

    uint32_t elapsed = now - last_click_time;
    if (index == last_clicked_index && elapsed < 400) {
        // Double-click detected!
        last_clicked_index = -1;
        last_click_time = 0;
        
        if (entry.is_dir) {
            char path[SD_PATH_MAX_LEN];
            join_path(s_sd_page.current_dir, entry.name, path, sizeof(path));
            std::snprintf(s_sd_page.current_dir, sizeof(s_sd_page.current_dir), "%s", path);
            reload_file_view();
        }
        return;
    }

    // Otherwise, treat as a single click
    last_clicked_index = index;
    last_click_time = now;

    if (s_sd_page.selected_index == index) {
        s_sd_page.selected_index = -1; // Deselect
    } else {
        s_sd_page.selected_index = index; // Select
    }
    update_file_view();
}

void sd_action_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    SdAction action = static_cast<SdAction>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    switch (action) {
        case SdAction::BACK:
            if (std::strcmp(s_sd_page.current_dir, "/sdcard") != 0) {
                char* slash = std::strrchr(s_sd_page.current_dir, '/');
                if (slash && slash > s_sd_page.current_dir + std::strlen("/sdcard")) {
                    *slash = '\0';
                } else {
                    std::snprintf(s_sd_page.current_dir, sizeof(s_sd_page.current_dir), "/sdcard");
                }
                reload_file_view();
                return;
            }
            break;
        default:
            break;
    }
    update_file_view();
}

void sd_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    s_sd_page.page_alive = false;
    s_sd_page.status_label = nullptr;
    s_sd_page.capacity_label = nullptr;
    s_sd_page.size_label = nullptr;
    s_sd_page.path_label = nullptr;
    s_sd_page.list_container = nullptr;
    s_sd_page.up_button = nullptr;
    s_sd_page.down_button = nullptr;
    s_sd_page.open_button = nullptr;
    s_sd_page.back_button = nullptr;
    s_sd_page.header_test_button = nullptr;
    s_sd_page.test_dialog = {};
}

void build_content(lv_obj_t* parent, const CoreP4FunctionWindowConfig& config)
{
    (void)config;

    prepare_content(parent);
    s_sd_page.page_alive = true;
    lv_obj_add_event_cb(parent, sd_content_delete_event_cb, LV_EVENT_DELETE, nullptr);

    // 1. Unified File Explorer Card (Top & Center, height 348)
    lv_obj_t* file_card = add_card(parent, 348);

    // 1a. Explorer Header Row (Title + Path on left, Button on right)
    lv_obj_t* header_row = add_row(file_card, 32);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Horizontal left box inside header_row to group Title and Path side-by-side
    lv_obj_t* left_box = lv_obj_create(header_row);
    reset_obj_defaults(left_box);
    lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_box, 8, LV_PART_MAIN);

    // Explorer Title
    lv_obj_t* title_lbl = add_label(left_box, "Explorer", COLOR_TEXT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

    // Path Label right next to Explorer title (Primary blue, width 230 for generous spacing before Back button)
    s_sd_page.path_label = add_label(left_box, "/sdcard", COLOR_PRIMARY, 230);
    lv_obj_set_style_text_font(s_sd_page.path_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(s_sd_page.path_label, LV_LABEL_LONG_DOT);

    // Single Back button on the right side of header_row
    s_sd_page.back_button = add_button(header_row, "Back", COLOR_BUTTON_BG, COLOR_TEXT, 60, 28);
    lv_obj_t* back_lbl = lv_obj_get_child(s_sd_page.back_button, 0);
    if (back_lbl) {
        lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    }
    lv_obj_add_event_cb(s_sd_page.back_button, sd_action_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(SdAction::BACK)));

    // 1b. Scrollable List Container (replaces the address bar and static rows)
    s_sd_page.list_container = lv_obj_create(file_card);
    reset_obj_defaults(s_sd_page.list_container);
    lv_obj_set_width(s_sd_page.list_container, lv_pct(100));
    lv_obj_set_height(s_sd_page.list_container, 274); // Fits 7-8 bubble rows perfectly without clipping
    lv_obj_set_flex_flow(s_sd_page.list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_sd_page.list_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_sd_page.list_container, 6, LV_PART_MAIN);
    
    // Enable touch swipe scrolling on the container
    lv_obj_add_flag(s_sd_page.list_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_sd_page.list_container, LV_SCROLLBAR_MODE_AUTO);

    // 2. Simplified Status Bar Card (Bottom, height 36)
    lv_obj_t* status_card = add_card(parent, 36);
    lv_obj_set_style_pad_hor(status_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(status_card, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(status_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Bottom Left: Current File / Folder size
    s_sd_page.size_label = add_label(status_card, "", COLOR_MUTED, 180);
    lv_obj_set_style_text_font(s_sd_page.size_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(s_sd_page.size_label, LV_LABEL_LONG_DOT);

    // Bottom Right: SD Card Info (simplified)
    s_sd_page.status_label = add_label(status_card, "SD: --", COLOR_MUTED, 220);
    lv_obj_set_style_text_font(s_sd_page.status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(s_sd_page.status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_sd_page.status_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    refresh_sd_status();
    std::snprintf(s_sd_page.current_dir, sizeof(s_sd_page.current_dir), "/sdcard");
    reload_file_view();
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "SD Card",
    "click on ui_func1sdcard1con",
    build_content,
    "Test",
    test_button_event_cb,
};

} // namespace

extern "C" void func2sdcard2clicked2func(lv_event_t* e)
{
    corep4_ui_open_function_window(e, WINDOW_CONFIG);
}
