#include "ui_function_widgets.h"

#include "i2c_bus.h"
#include "m5_corep4_pinmap.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_I2C";
constexpr uint8_t I2C_ADDR_MIN = 0x08;
constexpr uint8_t I2C_ADDR_MAX = 0x77;
constexpr int KNOWN_I2C_COUNT = 7;
constexpr int UNKNOWN_I2C_LINES = 3;
constexpr int I2C_SCAN_BUS_COUNT = 2;
constexpr int I2C_RESULT_BUBBLE_COUNT = KNOWN_I2C_COUNT + UNKNOWN_I2C_LINES;
constexpr int I2C_BUBBLE_WIDTH = 198;
constexpr int I2C_BUBBLE_HEIGHT = 38;
constexpr int I2C_SELECT_BUTTON_WIDTH = 198;
constexpr int I2C_SELECT_BUTTON_HEIGHT = 64;

struct KnownI2CDevice {
    uint8_t addr;
    const char* name;
};

constexpr KnownI2CDevice KNOWN_DEVICES[KNOWN_I2C_COUNT] = {
    {I2C_ADDR_PM1,    "PM1"},
    {I2C_ADDR_PYB,    "PYB"},
    {I2C_ADDR_ES8311, "ES8311"},
    {I2C_ADDR_ES7210, "ES7210"},
    {I2C_ADDR_CST3XX, "CST3XX"},
    {I2C_ADDR_BMI270, "BMI270"},
    {I2C_ADDR_RTC,    "RX8130"},
};

struct I2CScanBusInfo {
    const char* title;
    gpio_num_t scl;
    gpio_num_t sda;
    i2c_port_t port;
    uint32_t frequency_hz;
    bool use_core_bus;
};

constexpr I2CScanBusInfo I2C_SCAN_BUSES[I2C_SCAN_BUS_COUNT] = {
    {
        "Internal I2C",
        Pin_COREP4_INTERNAL_I2C_SCL,
        Pin_COREP4_INTERNAL_I2C_SDA,
        COREP4_INTERNAL_I2C_PORT,
        COREP4_INTERNAL_I2C_FREQ_HZ,
        true,
    },
    {
        "PORT.A I2C",
        Pin_COREP4_EXTERNAL_I2C_SCL,
        Pin_COREP4_EXTERNAL_I2C_SDA,
        COREP4_EXTERNAL_I2C_PORT,
        COREP4_EXTERNAL_I2C_FREQ_HZ,
        false,
    },
};

struct I2CBubble {
    lv_obj_t* box = nullptr;
    lv_obj_t* addr_label = nullptr;
    lv_obj_t* name_label = nullptr;
    lv_obj_t* state_label = nullptr;
};

struct I2CScanPageState {
    lv_obj_t* status_label = nullptr;
    lv_obj_t* summary_label = nullptr;
    lv_obj_t* results_grid = nullptr;
    I2CBubble known_bubbles[KNOWN_I2C_COUNT] = {};
    I2CBubble unknown_bubbles[UNKNOWN_I2C_LINES] = {};
    lv_obj_t* scan_buttons[I2C_SCAN_BUS_COUNT] = {};
    int active_bus_index = 0;
    volatile bool page_alive = false;
    volatile bool task_running = false;
};

I2CScanPageState s_i2c_page;

int known_index_for_addr(uint8_t addr)
{
    for (int i = 0; i < KNOWN_I2C_COUNT; ++i) {
        if (KNOWN_DEVICES[i].addr == addr) {
            return i;
        }
        if (KNOWN_DEVICES[i].addr == I2C_ADDR_PYB && (addr == 0x6F || addr == 0x4F)) {
            return i;
        }
    }
    return -1;
}

int sanitize_bus_index(int bus_index)
{
    if (bus_index < 0 || bus_index >= I2C_SCAN_BUS_COUNT) {
        return 0;
    }
    return bus_index;
}

I2CBubble& result_bubble_at(int index)
{
    if (index < KNOWN_I2C_COUNT) {
        return s_i2c_page.known_bubbles[index];
    }
    return s_i2c_page.unknown_bubbles[index - KNOWN_I2C_COUNT];
}

lv_style_selector_t main_selector(lv_state_t state = LV_STATE_DEFAULT)
{
    return static_cast<lv_style_selector_t>(LV_PART_MAIN) |
           static_cast<lv_style_selector_t>(state);
}

void set_scan_enabled(bool enabled)
{
    for (auto* button : s_i2c_page.scan_buttons) {
        if (!button) {
            continue;
        }

        lv_obj_clear_state(button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_COVER, LV_PART_MAIN);
        if (enabled) {
            lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

void refresh_i2c_status()
{
    const auto& bus_info = I2C_SCAN_BUSES[sanitize_bus_index(s_i2c_page.active_bus_index)];

    bool ready = true;
    if (bus_info.use_core_bus) {
        auto* tool = corep4_ui_sync_get_tool();
        auto bus = tool ? tool->otool_tools.i2c_tools.get_i2c_bus_handle() : nullptr;
        ready = (bus != nullptr);
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "%s  I2C%d  SCL:%d SDA:%d  %lu kHz  %s",
                  bus_info.title,
                  static_cast<int>(bus_info.port),
                  static_cast<int>(bus_info.scl),
                  static_cast<int>(bus_info.sda),
                  static_cast<unsigned long>(bus_info.frequency_hz / 1000),
                  ready ? "ready" : "not ready");
    set_label_text(s_i2c_page.status_label, buf);
}

void refresh_scan_button_styles()
{
    for (int i = 0; i < I2C_SCAN_BUS_COUNT; ++i) {
        lv_obj_t* button = s_i2c_page.scan_buttons[i];
        if (!button) {
            continue;
        }

        lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_PRIMARY), main_selector());
        lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_PRIMARY), main_selector(LV_STATE_PRESSED));
        lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_PRIMARY), main_selector(LV_STATE_DISABLED));
        lv_obj_set_style_border_color(button, lv_color_hex(COLOR_PRIMARY), main_selector());
        lv_obj_set_style_border_color(button, lv_color_hex(COLOR_PRIMARY), main_selector(LV_STATE_PRESSED));
        lv_obj_set_style_border_color(button, lv_color_hex(COLOR_PRIMARY), main_selector(LV_STATE_DISABLED));
        lv_obj_set_style_opa(button, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_t* title_label = lv_obj_get_child(button, 0);
        if (title_label) {
            lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        }
        lv_obj_t* detail_label = lv_obj_get_child(button, 1);
        if (detail_label) {
            lv_obj_set_style_text_color(detail_label, lv_color_hex(0xDBEAFE), LV_PART_MAIN);
        }
    }
}

void set_bubble_visible(I2CBubble& bubble, bool visible)
{
    if (!bubble.box) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(bubble.box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bubble.box, LV_OBJ_FLAG_HIDDEN);
    }
}

void clear_i2c_bubbles()
{
    for (auto& bubble : s_i2c_page.known_bubbles) {
        set_bubble_visible(bubble, false);
    }
    for (auto& bubble : s_i2c_page.unknown_bubbles) {
        set_bubble_visible(bubble, false);
    }
}

void set_bubble(I2CBubble& bubble,
                uint8_t addr,
                const char* name,
                const char* state,
                uint32_t bg_color,
                uint32_t border_color,
                uint32_t text_color)
{
    if (!bubble.box) {
        return;
    }

    char addr_text[8];
    std::snprintf(addr_text, sizeof(addr_text), "0x%02X", addr);
    set_label_text(bubble.addr_label, addr_text);
    set_label_text(bubble.name_label, name);
    set_label_text(bubble.state_label, state);

    lv_obj_set_style_bg_color(bubble.box, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_set_style_border_color(bubble.box, lv_color_hex(border_color), LV_PART_MAIN);
    if (bubble.addr_label) {
        lv_obj_set_style_text_color(bubble.addr_label, lv_color_hex(text_color), LV_PART_MAIN);
    }
    if (bubble.name_label) {
        lv_obj_set_style_text_color(bubble.name_label, lv_color_hex(text_color), LV_PART_MAIN);
    }
    if (bubble.state_label) {
        lv_obj_set_style_text_color(bubble.state_label, lv_color_hex(text_color), LV_PART_MAIN);
    }
    set_bubble_visible(bubble, true);
}

I2CBubble create_i2c_bubble(lv_obj_t* parent)
{
    I2CBubble bubble{};
    bubble.box = lv_obj_create(parent);
    lv_obj_remove_flag(bubble.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bubble.box, I2C_BUBBLE_WIDTH, I2C_BUBBLE_HEIGHT);
    lv_obj_set_style_bg_color(bubble.box, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble.box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble.box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bubble.box, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(bubble.box, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble.box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bubble.box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(bubble.box, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(bubble.box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bubble.box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    bubble.addr_label = add_label(bubble.box, "0x--", COLOR_MUTED, 46);
    bubble.name_label = add_label(bubble.box, "--", COLOR_TEXT, 74);
    bubble.state_label = add_label(bubble.box, "--", COLOR_MUTED, 54);
    lv_label_set_long_mode(bubble.addr_label, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(bubble.name_label, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(bubble.state_label, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(bubble.box, LV_OBJ_FLAG_HIDDEN);
    return bubble;
}

void render_i2c_scan_results(const uint8_t* addrs, uint8_t found, bool scan_failed)
{
    clear_i2c_bubbles();
    if (scan_failed) {
        return;
    }

    bool known_found[KNOWN_I2C_COUNT] = {};
    uint8_t known_detected_addr[KNOWN_I2C_COUNT] = {};
    for (int i = 0; i < KNOWN_I2C_COUNT; ++i) {
        known_detected_addr[i] = KNOWN_DEVICES[i].addr;
    }

    uint8_t unknown_addrs[UNKNOWN_I2C_LINES] = {};
    uint8_t unknown_shown = 0;
    uint8_t unknown_total = 0;

    for (uint8_t i = 0; i < found; ++i) {
        const uint8_t addr = addrs[i];
        if (addr < I2C_ADDR_MIN || addr > I2C_ADDR_MAX) {
            continue;
        }
        const int known_index = known_index_for_addr(addr);
        if (known_index >= 0) {
            known_found[known_index] = true;
            known_detected_addr[known_index] = addr;
        } else {
            unknown_total++;
            if (unknown_shown < UNKNOWN_I2C_LINES) {
                unknown_addrs[unknown_shown++] = addr;
            }
        }
    }

    for (int i = 0; i < KNOWN_I2C_COUNT; ++i) {
        uint8_t show_addr = known_detected_addr[i];
        if (show_addr == I2C_ADDR_PYB) {
            show_addr = 0x6F; // 如果是 PYB 且未扫到，则显示 0x6F 作为占位
        }

        if (known_found[i]) {
            set_bubble(s_i2c_page.known_bubbles[i],
                       show_addr,
                       KNOWN_DEVICES[i].name,
                       "OK",
                       0xDCFCE7,
                       0x86EFAC,
                       0x166534);
        } else {
            set_bubble(s_i2c_page.known_bubbles[i],
                       show_addr,
                       KNOWN_DEVICES[i].name,
                       "MISS",
                       0xFFEDD5,
                       0xFDBA74,
                       0x9A3412);
        }
    }

    for (uint8_t i = 0; i < unknown_shown; ++i) {
        const bool is_more_slot = (unknown_total > UNKNOWN_I2C_LINES) && (i == UNKNOWN_I2C_LINES - 1);
        char state[12];
        if (is_more_slot) {
            std::snprintf(state, sizeof(state), "+%u", static_cast<unsigned>(unknown_total - UNKNOWN_I2C_LINES + 1));
        } else {
            std::snprintf(state, sizeof(state), "UNK");
        }
        set_bubble(s_i2c_page.unknown_bubbles[i],
                   unknown_addrs[i],
                   "Unknown",
                   state,
                   0xE2E8F0,
                   0xCBD5E1,
                   0x334155);
    }
}

void render_external_i2c_scan_results(const uint8_t* addrs, uint8_t found, bool scan_failed)
{
    clear_i2c_bubbles();
    if (scan_failed) {
        return;
    }

    uint8_t visible_addrs[I2C_RESULT_BUBBLE_COUNT] = {};
    uint8_t shown = 0;
    uint8_t total = 0;

    for (uint8_t i = 0; i < found; ++i) {
        const uint8_t addr = addrs[i];
        if (addr < I2C_ADDR_MIN || addr > I2C_ADDR_MAX) {
            continue;
        }

        total++;
        if (shown < I2C_RESULT_BUBBLE_COUNT) {
            visible_addrs[shown++] = addr;
        }
    }

    for (uint8_t i = 0; i < shown; ++i) {
        const bool is_more_slot = (total > I2C_RESULT_BUBBLE_COUNT) && (i == I2C_RESULT_BUBBLE_COUNT - 1);
        char state[12];
        if (is_more_slot) {
            std::snprintf(state, sizeof(state), "+%u",
                          static_cast<unsigned>(total - I2C_RESULT_BUBBLE_COUNT + 1));
        } else {
            std::snprintf(state, sizeof(state), "OK");
        }

        set_bubble(result_bubble_at(i),
                   visible_addrs[i],
                   "Device",
                   state,
                   0xDCFCE7,
                   0x86EFAC,
                   0x166534);
    }
}

void render_scan_results_for_bus(int bus_index, const uint8_t* addrs, uint8_t found, bool scan_failed)
{
    if (I2C_SCAN_BUSES[sanitize_bus_index(bus_index)].use_core_bus) {
        render_i2c_scan_results(addrs, found, scan_failed);
    } else {
        render_external_i2c_scan_results(addrs, found, scan_failed);
    }
}

void update_scan_ui_from_task(int bus_index, const char* summary, const uint8_t* addrs, uint8_t found)
{
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock LVGL for I2C update");
        return;
    }

    if (s_i2c_page.page_alive) {
        s_i2c_page.active_bus_index = sanitize_bus_index(bus_index);
        refresh_i2c_status();
        refresh_scan_button_styles();
        set_label_text(s_i2c_page.summary_label, summary);
        const bool scan_failed = std::strncmp(summary, "Scan failed", 11) == 0;
        if (s_i2c_page.summary_label) {
            lv_obj_set_style_text_color(s_i2c_page.summary_label,
                                        lv_color_hex(scan_failed ? COLOR_ERROR : COLOR_MUTED),
                                        LV_PART_MAIN);
        }
        render_scan_results_for_bus(bus_index, addrs, found, scan_failed);
        set_scan_enabled(!s_i2c_page.task_running);
    }

    lvgl_port_unlock();
}

uint8_t scan_i2c_bus_stable(i2c_bus_handle_t bus, uint8_t* addrs, size_t addr_capacity)
{
    uint8_t addr_counts[128] = {};
    for (int scan_iter = 0; scan_iter < 3; ++scan_iter) {
        uint8_t temp_addrs[128] = {};
        uint8_t temp_found = i2c_bus_scan(bus, temp_addrs, sizeof(temp_addrs));
        for (uint8_t i = 0; i < temp_found; ++i) {
            uint8_t addr = temp_addrs[i];
            if (addr < 128) {
                addr_counts[addr]++;
            }
        }
        if (scan_iter < 2) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    uint8_t found = 0;
    for (int addr = I2C_ADDR_MIN; addr <= I2C_ADDR_MAX; ++addr) {
        if (addr_counts[addr] == 3 && found < addr_capacity) {
            addrs[found++] = static_cast<uint8_t>(addr);
        }
    }
    return found;
}

i2c_bus_handle_t create_external_scan_bus(const I2CScanBusInfo& bus_info, bool* out_delete_after_scan)
{
    if (out_delete_after_scan) {
        *out_delete_after_scan = true;
    }

    i2c_master_bus_handle_t existing_master_bus = nullptr;
    if (i2c_master_get_bus_handle(bus_info.port, &existing_master_bus) == ESP_OK) {
        if (out_delete_after_scan) {
            *out_delete_after_scan = false;
        }
    }

    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = bus_info.sda;
    conf.scl_io_num = bus_info.scl;
    conf.sda_pullup_en = true;
    conf.scl_pullup_en = true;
    conf.master.clk_speed = bus_info.frequency_hz;
    conf.clk_flags = 0;

    return i2c_bus_create(bus_info.port, &conf);
}

void i2c_scan_task(void* param)
{
    char summary[96];
    uint8_t addrs[128] = {};
    int bus_index = static_cast<int>(reinterpret_cast<intptr_t>(param));
    bus_index = sanitize_bus_index(bus_index);
    const auto& bus_info = I2C_SCAN_BUSES[bus_index];

    auto* tool = corep4_ui_sync_get_tool();
    bool delete_bus_after_scan = false;
    auto bus = bus_info.use_core_bus
        ? (tool ? tool->otool_tools.i2c_tools.get_i2c_bus_handle() : nullptr)
        : create_external_scan_bus(bus_info, &delete_bus_after_scan);

    uint8_t found = 0;
    if ((bus_info.use_core_bus && !tool) || !bus) {
        std::snprintf(summary, sizeof(summary), "Scan failed: %s bus not ready", bus_info.title);
    } else {
        found = scan_i2c_bus_stable(bus, addrs, sizeof(addrs));

        if (found == 0) {
            std::snprintf(summary, sizeof(summary), "%s: Found 0 devices", bus_info.title);
        } else {
            std::snprintf(summary, sizeof(summary), "%s: Found %u device(s)",
                          bus_info.title, static_cast<unsigned>(found));
        }
    }

    if (bus && delete_bus_after_scan) {
        esp_err_t ret = i2c_bus_delete(&bus);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete temporary %s scan bus: %s",
                     bus_info.title, esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "I2C scan done: %s", summary);
    s_i2c_page.task_running = false;
    update_scan_ui_from_task(bus_index, summary, addrs, found);
    vTaskDelete(nullptr);
}

void scan_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_i2c_page.task_running) {
        return;
    }

    int bus_index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    bus_index = sanitize_bus_index(bus_index);
    const auto& bus_info = I2C_SCAN_BUSES[bus_index];

    s_i2c_page.active_bus_index = bus_index;
    s_i2c_page.task_running = true;
    refresh_i2c_status();
    refresh_scan_button_styles();
    set_scan_enabled(false);
    char scanning_text[72];
    std::snprintf(scanning_text, sizeof(scanning_text), "Scanning %s bus...", bus_info.title);
    set_label_text(s_i2c_page.summary_label, scanning_text);
    if (s_i2c_page.summary_label) {
        lv_obj_set_style_text_color(s_i2c_page.summary_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    }
    clear_i2c_bubbles();

    BaseType_t ret = xTaskCreate(i2c_scan_task,
                                 "func_i2c",
                                 4096,
                                 reinterpret_cast<void*>(static_cast<intptr_t>(bus_index)),
                                 configMAX_PRIORITIES - 4,
                                 nullptr);
    if (ret != pdPASS) {
        s_i2c_page.task_running = false;
        set_scan_enabled(true);
        set_label_text(s_i2c_page.summary_label, "Scan failed: cannot create worker task");
        if (s_i2c_page.summary_label) {
            lv_obj_set_style_text_color(s_i2c_page.summary_label, lv_color_hex(COLOR_ERROR), LV_PART_MAIN);
        }
    }
}

void clean_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_i2c_page.task_running) {
        return;
    }

    clear_i2c_bubbles();
    refresh_i2c_status();
    set_label_text(s_i2c_page.summary_label, "Tap a bus to search 0x08-0x77.");
    if (s_i2c_page.summary_label) {
        lv_obj_set_style_text_color(s_i2c_page.summary_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
    }
}

void i2c_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    s_i2c_page.page_alive = false;
    s_i2c_page.status_label = nullptr;
    s_i2c_page.summary_label = nullptr;
    s_i2c_page.results_grid = nullptr;
    for (auto& bubble : s_i2c_page.known_bubbles) {
        bubble = {};
    }
    for (auto& bubble : s_i2c_page.unknown_bubbles) {
        bubble = {};
    }
    for (auto& button : s_i2c_page.scan_buttons) {
        button = nullptr;
    }
}

lv_obj_t* create_i2c_bus_button(lv_obj_t* parent, int bus_index)
{
    const auto& bus_info = I2C_SCAN_BUSES[sanitize_bus_index(bus_index)];

    lv_obj_t* button = lv_button_create(parent);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(button, I2C_SELECT_BUTTON_WIDTH, I2C_SELECT_BUTTON_HEIGHT);
    lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_BUTTON_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(button,
                        scan_button_event_cb,
                        LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(bus_index)));

    lv_obj_t* title = lv_label_create(button);
    lv_label_set_text(title, bus_info.title);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    char detail[32];
    std::snprintf(detail, sizeof(detail), "SCL%d / SDA%d",
                  static_cast<int>(bus_info.scl),
                  static_cast<int>(bus_info.sda));
    lv_obj_t* pins = lv_label_create(button);
    lv_label_set_text(pins, detail);
    lv_obj_set_width(pins, lv_pct(100));
    lv_label_set_long_mode(pins, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(pins, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(pins, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(pins, lv_color_hex(0xDBEAFE), LV_PART_MAIN);

    return button;
}

void build_content(lv_obj_t* parent, const CoreP4FunctionWindowConfig& config)
{
    (void)config;

    prepare_content(parent);
    s_i2c_page.page_alive = true;
    lv_obj_add_event_cb(parent, i2c_content_delete_event_cb, LV_EVENT_DELETE, nullptr);

    lv_obj_t* status_card = add_card(parent, 84);
    lv_obj_t* bus_row = add_row(status_card, I2C_SELECT_BUTTON_HEIGHT);
    s_i2c_page.scan_buttons[0] = create_i2c_bus_button(bus_row, 0);
    s_i2c_page.scan_buttons[1] = create_i2c_bus_button(bus_row, 1);
    s_i2c_page.status_label = nullptr;
    s_i2c_page.summary_label = nullptr;

    lv_obj_t* list_card = add_card(parent, 300);
    lv_obj_t* results_header = add_row(list_card, 28);
    add_label(results_header, "Scan Results", COLOR_TEXT, 118);
    s_i2c_page.summary_label = add_label(results_header, "Tap a bus to search 0x08-0x77.", COLOR_MUTED, 282);
    lv_label_set_long_mode(s_i2c_page.summary_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_i2c_page.summary_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    s_i2c_page.results_grid = lv_obj_create(list_card);
    lv_obj_remove_flag(s_i2c_page.results_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(s_i2c_page.results_grid, lv_pct(100));
    lv_obj_set_height(s_i2c_page.results_grid, 246);
    lv_obj_set_style_bg_opa(s_i2c_page.results_grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_i2c_page.results_grid, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_i2c_page.results_grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_i2c_page.results_grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_i2c_page.results_grid, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_i2c_page.results_grid, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_i2c_page.results_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_i2c_page.results_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    for (int i = 0; i < KNOWN_I2C_COUNT; ++i) {
        s_i2c_page.known_bubbles[i] = create_i2c_bubble(s_i2c_page.results_grid);
    }
    for (int i = 0; i < UNKNOWN_I2C_LINES; ++i) {
        s_i2c_page.unknown_bubbles[i] = create_i2c_bubble(s_i2c_page.results_grid);
    }

    refresh_i2c_status();
    refresh_scan_button_styles();
    clear_i2c_bubbles();
    set_scan_enabled(!s_i2c_page.task_running);
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "I2C Scan",
    "click on ui_func1i2cscan1con",
    build_content,
    "Clean",
    clean_button_event_cb,
};

} // namespace

extern "C" void func2i2cscan2clicked2func(lv_event_t* e)
{
    corep4_ui_open_function_window(e, WINDOW_CONFIG);
}
