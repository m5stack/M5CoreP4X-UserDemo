#include "ui_function_widgets.h"

#include "driver/gpio.h"
#include "esp_system.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_POWER";
constexpr int32_t POWER_BUTTON_ROW_WIDTH = 404;
constexpr int32_t CHARGE_CONTROL_HEIGHT = 36;
constexpr int32_t CHARGE_BUBBLE_WIDTH = 146;
constexpr int32_t CHARGE_BUTTON_WIDTH = 78;
constexpr int32_t M5BUS_GROUP_WIDTH = 128;
constexpr int32_t M5BUS_GROUP_HEIGHT = 316;
constexpr int32_t M5BUS_GROUP_GAP = 24;
constexpr int32_t M5BUS_COLUMN_WIDTH = 60;
constexpr int32_t M5BUS_COLUMN_HEIGHT = 294;
constexpr int32_t M5BUS_COLUMN_GAP = 8;
constexpr int32_t M5BUS_COLUMN_HEADER_HEIGHT = 20;
constexpr int32_t M5BUS_PIN_CELL_HEIGHT = 50;
constexpr int32_t M5BUS_LEVEL_INDICATOR_SIZE = 10;
constexpr int32_t M5BUS_LEVEL_INDICATOR_INSET = 4;
constexpr int32_t M5BUS_SIGNAL_AREA_Y = 20;
constexpr int32_t M5BUS_SIGNAL_AREA_TWO_LINE_Y = M5BUS_SIGNAL_AREA_Y - 5;
constexpr int32_t M5BUS_SIGNAL_AREA_HEIGHT = 28;
constexpr uint32_t M5BUS_COLOR_GPIO_BG = 0xDCFCE7;
constexpr uint32_t M5BUS_COLOR_GPIO_BORDER = 0x86EFAC;
constexpr uint32_t M5BUS_COLOR_GPIO_OUTPUT_BG = 0xFEF3C7;
constexpr uint32_t M5BUS_COLOR_GPIO_OUTPUT_BORDER = 0xFBBF24;
constexpr uint32_t M5BUS_COLOR_POWER_BG = 0xFEE2E2;
constexpr uint32_t M5BUS_COLOR_POWER_BORDER = 0xFCA5A5;
constexpr uint32_t M5BUS_COLOR_GND_BG = 0x4B5563;
constexpr uint32_t M5BUS_COLOR_GND_BORDER = 0x000000;
constexpr uint32_t M5BUS_COLOR_LABEL_STRONG = 0x000000;
constexpr uint32_t M5BUS_COLOR_LEVEL_HIGH = COLOR_PRIMARY;
constexpr uint32_t M5BUS_COLOR_LEVEL_OUTPUT_HIGH = COLOR_ERROR;
constexpr uint32_t M5BUS_COLOR_LEVEL_LOW = 0xCBD5E1;
constexpr uint32_t M5BUS_COLOR_LEVEL_LOW_BORDER = 0x94A3B8;
constexpr uint32_t M5BUS_SAMPLE_PERIOD_MS = 250;
constexpr size_t M5BUS_MAX_GPIO_BINDINGS = 20;
constexpr int32_t M5BUS_GPIO_POPUP_BUTTON_W = 92;
constexpr int32_t M5BUS_GPIO_POPUP_BUTTON_H = 32;
constexpr uint32_t VBAT_SAMPLE_PERIOD_MS = 250;
constexpr size_t VBAT_SAMPLE_COUNT = 14;
constexpr size_t VBAT_TRIM_COUNT = 2;
constexpr uint8_t CHARGE_VALUE_UNKNOWN = 0xFF;

struct PowerToggleBinding {
    StatusParam param;
    const char* name;
    lv_obj_t* button;
};

struct ChargeOption {
    uint8_t value;
    const char* text;
    lv_obj_t* button;
};

enum class M5BusPinKind : uint8_t {
    GPIO,
    TEXT,
    VOLTAGE,
};

struct M5BusPinDef {
    const char* pos;
    const char* side;
    M5BusPinKind kind;
    gpio_num_t gpio;
    const char* text;
};

struct M5BusPinPair {
    const M5BusPinDef* left;
    const M5BusPinDef* right;
};

struct M5BusPinGroup {
    const char* title;
    const M5BusPinPair* rows;
    size_t row_count;
};

enum class M5BusGpioMode : uint8_t {
    Input,
    Output,
};

enum class M5BusGpioDrive : uint8_t {
    OpenDrain,
    PushPull,
};

enum class M5BusGpioPull : uint8_t {
    None,
    Up,
    Down,
};

struct M5BusGpioOptions {
    M5BusGpioMode mode = M5BusGpioMode::Input;
    M5BusGpioDrive drive = M5BusGpioDrive::OpenDrain;
    M5BusGpioPull pull = M5BusGpioPull::None;
    bool output_high = false;
};

struct M5BusGpioConfig {
    const M5BusPinDef* pin = nullptr;
    lv_obj_t* cell = nullptr;
    lv_obj_t* level_indicator = nullptr;
    lv_obj_t* signal_label = nullptr;
    int last_level = -1;
    M5BusGpioOptions options = {};
};

enum class M5BusGpioPopupAction : uint8_t {
    MODE_INPUT,
    MODE_OUTPUT,
    LEVEL_LOW,
    LEVEL_HIGH,
    DRIVE_OPEN_DRAIN,
    DRIVE_PUSH_PULL,
    PULL_NONE,
    PULL_UP,
    PULL_DOWN,
    APPLY,
    CLOSE,
};

struct M5BusGpioPopupState {
    CoreP4FunctionDialogHandle dialog = {};
    M5BusGpioConfig* target = nullptr;
    M5BusGpioOptions draft = {};
    lv_obj_t* status_label = nullptr;
    lv_obj_t* mode_input_button = nullptr;
    lv_obj_t* mode_output_button = nullptr;
    lv_obj_t* level_low_button = nullptr;
    lv_obj_t* level_high_button = nullptr;
    lv_obj_t* drive_od_button = nullptr;
    lv_obj_t* drive_pp_button = nullptr;
    lv_obj_t* pull_none_button = nullptr;
    lv_obj_t* pull_up_button = nullptr;
    lv_obj_t* pull_down_button = nullptr;
};

struct PowerPageState {
    lv_obj_t* status_label = nullptr;
    lv_obj_t* result_label = nullptr;
    lv_obj_t* brightness_slider = nullptr;
    lv_obj_t* brightness_val_label = nullptr;
    lv_obj_t* vbat_label = nullptr;
    lv_timer_t* refresh_timer = nullptr;
    lv_timer_t* vbat_timer = nullptr;
    lv_timer_t* m5bus_timer = nullptr;
    M5BusGpioConfig m5bus_bindings[M5BUS_MAX_GPIO_BINDINGS] = {};
    size_t m5bus_binding_count = 0;
    uint16_t vbat_samples[VBAT_SAMPLE_COUNT] = {};
    uint8_t vbat_sample_count = 0;
    uint8_t vbat_sample_index = 0;
    uint8_t observed_charge_value = CHARGE_VALUE_UNKNOWN;
    volatile bool page_alive = false;
    volatile uint32_t restart_token = 0;
};

PowerPageState s_power_page;
M5BusGpioPopupState s_m5bus_gpio_popup;

enum class PowerDialogAction {
    NONE,
    LCD_POWER_OFF,
    L3B_POWER_OFF,
    AUDIO_CHIP_POWER_OFF,
};

struct PowerDialogState {
    CoreP4FunctionDialogHandle dialog = {};
    lv_timer_t* restart_button_timer = nullptr;
    PowerDialogAction action = PowerDialogAction::NONE;
};

struct RestartTaskContext {
    uint32_t delay_ms = 0;
    uint32_t token = 0;
    bool cancelable = false;
};

PowerDialogState s_dialog;

constexpr uint8_t DIALOG_BUTTON_CANCEL = 0;
constexpr uint8_t DIALOG_BUTTON_CONFIRM = 1;
constexpr uint8_t DIALOG_BUTTON_RESTART = 2;

constexpr M5BusGpioPopupAction GPIO_ACTION_MODE_INPUT = M5BusGpioPopupAction::MODE_INPUT;
constexpr M5BusGpioPopupAction GPIO_ACTION_MODE_OUTPUT = M5BusGpioPopupAction::MODE_OUTPUT;
constexpr M5BusGpioPopupAction GPIO_ACTION_LEVEL_LOW = M5BusGpioPopupAction::LEVEL_LOW;
constexpr M5BusGpioPopupAction GPIO_ACTION_LEVEL_HIGH = M5BusGpioPopupAction::LEVEL_HIGH;
constexpr M5BusGpioPopupAction GPIO_ACTION_DRIVE_OPEN_DRAIN = M5BusGpioPopupAction::DRIVE_OPEN_DRAIN;
constexpr M5BusGpioPopupAction GPIO_ACTION_DRIVE_PUSH_PULL = M5BusGpioPopupAction::DRIVE_PUSH_PULL;
constexpr M5BusGpioPopupAction GPIO_ACTION_PULL_NONE = M5BusGpioPopupAction::PULL_NONE;
constexpr M5BusGpioPopupAction GPIO_ACTION_PULL_UP = M5BusGpioPopupAction::PULL_UP;
constexpr M5BusGpioPopupAction GPIO_ACTION_PULL_DOWN = M5BusGpioPopupAction::PULL_DOWN;
constexpr M5BusGpioPopupAction GPIO_ACTION_APPLY = M5BusGpioPopupAction::APPLY;
constexpr M5BusGpioPopupAction GPIO_ACTION_CLOSE = M5BusGpioPopupAction::CLOSE;

PowerToggleBinding s_power_toggles[] = {
    {StatusParam::POWER_PYB_L3B,     "L3B",      nullptr},
    {StatusParam::POWER_LCD,         "LCD",      nullptr},
    {StatusParam::POWER_AUDIO_CHIP,  "AudioIC",  nullptr},
    {StatusParam::POWER_AUDIO_PA,    "AudioPA",  nullptr},
    {StatusParam::POWER_PYB_USBA,    "USBA",     nullptr},
    {StatusParam::POWER_P4USBC,      "P4USBC",   nullptr},
    {StatusParam::POWER_GROVE,       "Grove",    nullptr},
    {StatusParam::POWER_M5BUS,       "M5BUS",    nullptr},
};

ChargeOption s_charge_options[] = {
    {static_cast<uint8_t>(CHARGE_CURRENT_OFF), "OFF", nullptr},
    {static_cast<uint8_t>(CHARGE_CURRENT_180MA), "180mA", nullptr},
    {static_cast<uint8_t>(CHARGE_CURRENT_650MA), "650mA", nullptr},
};

constexpr M5BusPinDef M5BUS_P1  = {"P1",  "L", M5BusPinKind::TEXT,    GPIO_NUM_NC, COREP4_M5BUS_P1};
constexpr M5BusPinDef M5BUS_P2  = {"P2",  "R", M5BusPinKind::GPIO,    COREP4_M5BUS_P2, nullptr};
constexpr M5BusPinDef M5BUS_P3  = {"P3",  "L", M5BusPinKind::TEXT,    GPIO_NUM_NC, COREP4_M5BUS_P3};
constexpr M5BusPinDef M5BUS_P4  = {"P4",  "R", M5BusPinKind::GPIO,    COREP4_M5BUS_P4, nullptr};
constexpr M5BusPinDef M5BUS_P5  = {"P5",  "L", M5BusPinKind::TEXT,    GPIO_NUM_NC, COREP4_M5BUS_P5};
constexpr M5BusPinDef M5BUS_P6  = {"P6",  "R", M5BusPinKind::TEXT,    GPIO_NUM_NC, COREP4_M5BUS_P6};
constexpr M5BusPinDef M5BUS_P7  = {"P7",  "L", M5BusPinKind::GPIO,    COREP4_M5BUS_P7, nullptr};
constexpr M5BusPinDef M5BUS_P8  = {"P8",  "R", M5BusPinKind::GPIO,    COREP4_M5BUS_P8, nullptr};
constexpr M5BusPinDef M5BUS_P9  = {"P9",  "L", M5BusPinKind::GPIO,    COREP4_M5BUS_P9, nullptr};
constexpr M5BusPinDef M5BUS_P10 = {"P10", "R", M5BusPinKind::GPIO,    COREP4_M5BUS_P10, nullptr};
constexpr M5BusPinDef M5BUS_P11 = {"P11", "L", M5BusPinKind::GPIO,    COREP4_M5BUS_P11, nullptr};
constexpr M5BusPinDef M5BUS_P12 = {"P12", "R", M5BusPinKind::VOLTAGE, GPIO_NUM_NC, COREP4_M5BUS_P12};
constexpr M5BusPinDef M5BUS_P13 = {"P13", "L", M5BusPinKind::GPIO,    COREP4_M5BUS_P13, nullptr};
constexpr M5BusPinDef M5BUS_P14 = {"P14", "R", M5BusPinKind::GPIO,    COREP4_M5BUS_P14, nullptr};
constexpr M5BusPinDef M5BUS_P15 = {"P15", "L", M5BusPinKind::GPIO,    COREP4_M5BUS_P15, nullptr};
constexpr M5BusPinDef M5BUS_P16 = {"P16", "R", M5BusPinKind::GPIO,    COREP4_M5BUS_P16, nullptr};
constexpr M5BusPinDef M5BUS_P17 = {"P17", "L", M5BusPinKind::GPIO,    COREP4_M5BUS_P17, nullptr};
constexpr M5BusPinDef M5BUS_P18 = {"P18", "R", M5BusPinKind::GPIO,    COREP4_M5BUS_P18, nullptr};
constexpr M5BusPinDef M5BUS_P19 = {"P19", "L", M5BusPinKind::GPIO,    COREP4_M5BUS_P19, nullptr};
constexpr M5BusPinDef M5BUS_P20 = {"P20", "R", M5BusPinKind::GPIO,    COREP4_M5BUS_P20, nullptr};
constexpr M5BusPinDef M5BUS_P21 = {"P21", "L", M5BusPinKind::GPIO,    COREP4_M5BUS_P21, nullptr};
constexpr M5BusPinDef M5BUS_P22 = {"P22", "R", M5BusPinKind::GPIO,    COREP4_M5BUS_P22, nullptr};
constexpr M5BusPinDef M5BUS_P23 = {"P23", "L", M5BusPinKind::GPIO,    COREP4_M5BUS_P23, nullptr};
constexpr M5BusPinDef M5BUS_P24 = {"P24", "R", M5BusPinKind::GPIO,    COREP4_M5BUS_P24, nullptr};
constexpr M5BusPinDef M5BUS_P25 = {"P25", "L", M5BusPinKind::VOLTAGE, GPIO_NUM_NC, COREP4_M5BUS_P25};
constexpr M5BusPinDef M5BUS_P26 = {"P26", "R", M5BusPinKind::GPIO,    COREP4_M5BUS_P26, nullptr};
constexpr M5BusPinDef M5BUS_P27 = {"P27", "L", M5BusPinKind::VOLTAGE, GPIO_NUM_NC, COREP4_M5BUS_P27};
constexpr M5BusPinDef M5BUS_P28 = {"P28", "R", M5BusPinKind::VOLTAGE, GPIO_NUM_NC, COREP4_M5BUS_P28};
constexpr M5BusPinDef M5BUS_P29 = {"P29", "L", M5BusPinKind::VOLTAGE, GPIO_NUM_NC, COREP4_M5BUS_P29};
constexpr M5BusPinDef M5BUS_P30 = {"P30", "R", M5BusPinKind::VOLTAGE, GPIO_NUM_NC, COREP4_M5BUS_P30};

constexpr M5BusPinPair M5BUS_PIN_GROUP_1[] = {
    {&M5BUS_P1, &M5BUS_P2},
    {&M5BUS_P3, &M5BUS_P4},
    {&M5BUS_P5, &M5BUS_P6},
    {&M5BUS_P7, &M5BUS_P8},
    {&M5BUS_P9, &M5BUS_P10},
};

constexpr M5BusPinPair M5BUS_PIN_GROUP_2[] = {
    {&M5BUS_P11, &M5BUS_P12},
    {&M5BUS_P13, &M5BUS_P14},
    {&M5BUS_P15, &M5BUS_P16},
    {&M5BUS_P17, &M5BUS_P18},
    {&M5BUS_P19, &M5BUS_P20},
};

constexpr M5BusPinPair M5BUS_PIN_GROUP_3[] = {
    {&M5BUS_P21, &M5BUS_P22},
    {&M5BUS_P23, &M5BUS_P24},
    {&M5BUS_P25, &M5BUS_P26},
    {&M5BUS_P27, &M5BUS_P28},
    {&M5BUS_P29, &M5BUS_P30},
};

constexpr M5BusPinGroup M5BUS_PIN_GROUPS[] = {
    {"P01-P10", M5BUS_PIN_GROUP_1, sizeof(M5BUS_PIN_GROUP_1) / sizeof(M5BUS_PIN_GROUP_1[0])},
    {"P11-P20", M5BUS_PIN_GROUP_2, sizeof(M5BUS_PIN_GROUP_2) / sizeof(M5BUS_PIN_GROUP_2[0])},
    {"P21-P30", M5BUS_PIN_GROUP_3, sizeof(M5BUS_PIN_GROUP_3) / sizeof(M5BUS_PIN_GROUP_3[0])},
};

void reset_m5bus_bindings();
void close_m5bus_gpio_config_popup();
void refresh_m5bus_gpio_levels();
void refresh_m5bus_gpio_card_style(M5BusGpioConfig& config);
void set_m5bus_level_indicator(lv_obj_t* indicator, int level, bool output_mode = false);

const char* power_param_name(StatusParam param)
{
    switch (param) {
        case StatusParam::POWER_LCD: return "LCD";
        case StatusParam::POWER_AUDIO_CHIP: return "AudioIC";
        case StatusParam::POWER_AUDIO_PA: return "AudioPA";
        case StatusParam::POWER_PYB_L3B: return "L3B";
        case StatusParam::POWER_PYB_USBA: return "USBA";
        case StatusParam::POWER_GROVE: return "Grove";
        case StatusParam::POWER_P4USBC: return "P4USBC";
        case StatusParam::POWER_M5BUS: return "M5BUS";
        default: return "Power";
    }
}

bool read_power_bool(const device_realtime_status_t* status, StatusParam param)
{
    if (!status) {
        return false;
    }

    switch (param) {
        case StatusParam::POWER_LCD: return status->power.lcd_power;
        case StatusParam::POWER_AUDIO_CHIP: return status->power.audio_chip_power;
        case StatusParam::POWER_AUDIO_PA: return status->power.audio_pa_power;
        case StatusParam::POWER_PYB_L3B: return status->power.pyb_l3b_power;
        case StatusParam::POWER_PYB_USBA: return status->power.pyb_usba_power;
        case StatusParam::POWER_GROVE: return status->power.grove_power;
        case StatusParam::POWER_P4USBC: return status->power.p4usbc_power;
        case StatusParam::POWER_M5BUS: return status->power.m5bus_power;
        default: return false;
    }
}

void set_power_button_colors(lv_obj_t* button, uint32_t bg_color, uint32_t text_color)
{
    if (!button) {
        return;
    }

    lv_obj_set_style_bg_color(button, lv_color_hex(bg_color), LV_PART_MAIN);
    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (label) {
        lv_obj_set_style_text_color(label, lv_color_hex(text_color), LV_PART_MAIN);
    }
}

void set_result_text(const char* text, uint32_t color)
{
    set_label_text(s_power_page.result_label, text);
    if (s_power_page.result_label) {
        lv_obj_set_style_text_color(s_power_page.result_label,
                                    lv_color_hex(color),
                                    LV_PART_MAIN);
    }
}

void set_vbat_pending_text()
{
    set_label_text(s_power_page.vbat_label, "-.--V");
}

void reset_vbat_samples()
{
    s_power_page.vbat_sample_count = 0;
    s_power_page.vbat_sample_index = 0;
    set_vbat_pending_text();
}

void set_vbat_text_mv(uint16_t mv)
{
    char text[12];
    const uint16_t centivolts = static_cast<uint16_t>((static_cast<uint32_t>(mv) + 5) / 10);
    std::snprintf(text, sizeof(text), "%u.%02uV",
                  static_cast<unsigned>(centivolts / 100),
                  static_cast<unsigned>(centivolts % 100));
    set_label_text(s_power_page.vbat_label, text);
}

bool calculate_filtered_vbat_mv(uint16_t* filtered_mv)
{
    if (!filtered_mv || s_power_page.vbat_sample_count < VBAT_SAMPLE_COUNT) {
        return false;
    }

    uint16_t sorted[VBAT_SAMPLE_COUNT];
    for (size_t i = 0; i < VBAT_SAMPLE_COUNT; ++i) {
        sorted[i] = s_power_page.vbat_samples[i];
    }

    std::sort(sorted, sorted + VBAT_SAMPLE_COUNT);

    uint32_t sum = 0;
    constexpr size_t kept_count = VBAT_SAMPLE_COUNT - (VBAT_TRIM_COUNT * 2);
    for (size_t i = VBAT_TRIM_COUNT; i < VBAT_SAMPLE_COUNT - VBAT_TRIM_COUNT; ++i) {
        sum += sorted[i];
    }

    *filtered_mv = static_cast<uint16_t>((sum + kept_count / 2) / kept_count);
    return true;
}

void add_vbat_sample(uint16_t mv)
{
    s_power_page.vbat_samples[s_power_page.vbat_sample_index] = mv;
    s_power_page.vbat_sample_index =
        static_cast<uint8_t>((s_power_page.vbat_sample_index + 1) % VBAT_SAMPLE_COUNT);
    if (s_power_page.vbat_sample_count < VBAT_SAMPLE_COUNT) {
        ++s_power_page.vbat_sample_count;
    }

    uint16_t filtered_mv = 0;
    if (calculate_filtered_vbat_mv(&filtered_mv)) {
        set_vbat_text_mv(filtered_mv);
    } else {
        set_vbat_pending_text();
    }
}

void sample_vbat_once()
{
    if (!s_power_page.page_alive) {
        return;
    }

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        set_vbat_pending_text();
        return;
    }

    uint16_t mv = 0;
    const m5pm1_err_t ret = tool->pm1.readVbat(&mv);
    if (ret != M5PM1_OK) {
        if (s_power_page.vbat_sample_count < VBAT_SAMPLE_COUNT) {
            set_vbat_pending_text();
        }
        ESP_LOGD(TAG, "readVbat failed: %d", ret);
        return;
    }

    add_vbat_sample(mv);
}

void vbat_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    sample_vbat_once();
}

void close_power_dialog()
{
    if (s_dialog.restart_button_timer) {
        lv_timer_delete(s_dialog.restart_button_timer);
        s_dialog.restart_button_timer = nullptr;
    }

    corep4_ui_close_function_dialog(&s_dialog.dialog);

    s_dialog = {};
}

esp_err_t apply_power_status(StatusParam param, bool enabled)
{
    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        return ESP_ERR_INVALID_STATE;
    }

    return tool->set_status(param,
                            enabled,
                            StatusConfigFlag::APPLY_HARDWARE |
                                StatusConfigFlag::UPDATE_STATUS |
                                StatusConfigFlag::SYNC_UI);
}

void delayed_restart_task(void* param)
{
    auto* ctx = static_cast<RestartTaskContext*>(param);
    if (!ctx) {
        vTaskDelete(nullptr);
        return;
    }

    const uint32_t step_ms = 100;
    uint32_t elapsed_ms = 0;
    while (elapsed_ms < ctx->delay_ms) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        elapsed_ms += step_ms;
        if (ctx->cancelable && ctx->token != s_power_page.restart_token) {
            delete ctx;
            vTaskDelete(nullptr);
            return;
        }
    }

    if (ctx->cancelable && ctx->token != s_power_page.restart_token) {
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGW(TAG, "Restarting after protected power action");
    delete ctx;
    esp_restart();
}

esp_err_t schedule_restart(uint32_t delay_ms, bool cancelable)
{
    auto* ctx = new RestartTaskContext();
    ctx->delay_ms = delay_ms;
    ctx->cancelable = cancelable;
    if (cancelable) {
        const uint32_t next_token = static_cast<uint32_t>(s_power_page.restart_token) + 1;
        s_power_page.restart_token = next_token;
        ctx->token = next_token;
    }

    BaseType_t task_ret = xTaskCreate(delayed_restart_task,
                                      "pwr_rst",
                                      3072,
                                      ctx,
                                      configMAX_PRIORITIES - 4,
                                      nullptr);
    if (task_ret != pdPASS) {
        if (cancelable) {
            s_power_page.restart_token = static_cast<uint32_t>(s_power_page.restart_token) + 1;
        }
        delete ctx;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void cancel_scheduled_restart()
{
    s_power_page.restart_token = static_cast<uint32_t>(s_power_page.restart_token) + 1;
}

void restart_now_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    ESP_LOGW(TAG, "Restart requested from protected power dialog");
    esp_restart();
}

void show_restart_button_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    s_dialog.restart_button_timer = nullptr;

    if (!s_dialog.dialog.box) {
        return;
    }

    const char* text = "The selected power rail is now off.\n"
                       "Restart is required to restore the full demo experience.";
    if (s_dialog.action == PowerDialogAction::AUDIO_CHIP_POWER_OFF) {
        text = "AudioIC power is now off.\n"
               "Restart is required before audio playback or recording is used again.";
    } else if (s_dialog.action == PowerDialogAction::L3B_POWER_OFF) {
        text = "L3B power is now off.\n"
               "Restart is required to restore the full peripheral experience.";
    }

    corep4_ui_set_function_dialog_text(&s_dialog.dialog, text);
    corep4_ui_set_function_dialog_button_hidden(&s_dialog.dialog,
                                                 DIALOG_BUTTON_RESTART,
                                                 false);
}

void dialog_cancel_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    close_power_dialog();
    set_result_text("Operation canceled.", COLOR_MUTED);
}

void dialog_confirm_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    switch (s_dialog.action) {
        case PowerDialogAction::LCD_POWER_OFF: {
            esp_err_t restart_ret = schedule_restart(10000, true);
            if (restart_ret != ESP_OK) {
                corep4_ui_set_function_dialog_text(
                    &s_dialog.dialog,
                    "The automatic restart could not be scheduled. "
                    "LCD power was left unchanged.");
                set_result_text("LCD power was not changed: restart task failed.", COLOR_ERROR);
                return;
            }

            set_result_text("LCD power will turn off. Restarting in 10 seconds.", COLOR_WARNING);
            esp_err_t ret = apply_power_status(StatusParam::POWER_LCD, false);
            if (ret != ESP_OK) {
                cancel_scheduled_restart();
                set_result_text("LCD power update failed. Auto restart canceled.", COLOR_ERROR);
                ESP_LOGW(TAG, "LCD power off failed: %s", esp_err_to_name(ret));
                return;
            }
            close_power_dialog();
            break;
        }

        case PowerDialogAction::L3B_POWER_OFF:
        case PowerDialogAction::AUDIO_CHIP_POWER_OFF: {
            const StatusParam param = (s_dialog.action == PowerDialogAction::L3B_POWER_OFF)
                                          ? StatusParam::POWER_PYB_L3B
                                          : StatusParam::POWER_AUDIO_CHIP;
            esp_err_t ret = apply_power_status(param, false);
            if (ret != ESP_OK) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s update failed: %s.",
                              power_param_name(param), esp_err_to_name(ret));
                corep4_ui_set_function_dialog_text(&s_dialog.dialog, buf);
                set_result_text(buf, COLOR_ERROR);
                ESP_LOGW(TAG, "%s power off failed: %s",
                         power_param_name(param), esp_err_to_name(ret));
                return;
            }

            corep4_ui_set_function_dialog_button_hidden(&s_dialog.dialog,
                                                        DIALOG_BUTTON_CANCEL,
                                                        true);
            corep4_ui_set_function_dialog_button_hidden(&s_dialog.dialog,
                                                        DIALOG_BUTTON_CONFIRM,
                                                        true);
            corep4_ui_set_function_dialog_text(&s_dialog.dialog,
                                               "Applying the power change...");

            set_result_text("Power rail is off. Restart is required.", COLOR_WARNING);
            if (s_dialog.restart_button_timer) {
                lv_timer_delete(s_dialog.restart_button_timer);
            }
            s_dialog.restart_button_timer = lv_timer_create(show_restart_button_timer_cb, 2000, nullptr);
            lv_timer_set_repeat_count(s_dialog.restart_button_timer, 1);
            break;
        }

        case PowerDialogAction::NONE:
        default:
            close_power_dialog();
            break;
    }
}

void open_power_warning_dialog(PowerDialogAction action,
                               const char* title,
                               const char* message)
{
    close_power_dialog();

    s_dialog.action = action;

    const CoreP4FunctionDialogButtonConfig buttons[] = {
        {
            "Cancel",
            dialog_cancel_event_cb,
            nullptr,
            COLOR_BUTTON_BG,
            COLOR_TEXT,
            false,
        },
        {
            "Confirm",
            dialog_confirm_event_cb,
            nullptr,
            COLOR_ERROR,
            0xFFFFFF,
            false,
        },
        {
            "Restart",
            restart_now_event_cb,
            nullptr,
            COLOR_WARNING,
            0xFFFFFF,
            true,
        },
    };

    s_dialog.dialog = corep4_ui_open_function_dialog(title,
                                                     message,
                                                     CoreP4FunctionDialogSize::MEDIUM,
                                                     buttons,
                                                     sizeof(buttons) / sizeof(buttons[0]));
}

void set_toggle_button(PowerToggleBinding& binding, bool enabled)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s %s", binding.name, enabled ? "ON" : "OFF");

    set_button_text(binding.button, buf);
    set_power_button_colors(binding.button,
                            enabled ? COLOR_SUCCESS : COLOR_BUTTON_BG,
                            enabled ? 0xFFFFFF : COLOR_TEXT);
}

void set_power_controls_enabled(bool enabled)
{
    for (auto& binding : s_power_toggles) {
        set_button_enabled(binding.button, enabled);
    }

    if (s_power_page.brightness_slider) {
        if (enabled) {
            lv_obj_clear_state(s_power_page.brightness_slider, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_power_page.brightness_slider, LV_STATE_DISABLED);
        }
    }

    for (auto& option : s_charge_options) {
        set_button_enabled(option.button, enabled);
    }
}

void refresh_power_ui()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;

    if (!status) {
        set_label_text(s_power_page.status_label, "Device tool: not ready");
        set_power_controls_enabled(false);
        return;
    }

    char status_buf[128];
    std::snprintf(status_buf, sizeof(status_buf),
                  "PYB:%s  PM1:%s  FW:%u  BL:%u%%",
                  status->power.pyb_initialized ? "OK" : "--",
                  status->power.pm1_initialized ? "OK" : "--",
                  static_cast<unsigned>(status->power.pyb_firmware_version),
                  static_cast<unsigned>(status->power.lcd_brightness));
    set_label_text(s_power_page.status_label, status_buf);

    for (auto& binding : s_power_toggles) {
        set_toggle_button(binding, read_power_bool(status, binding.param));
    }

    if (s_power_page.brightness_slider) {
        if (!lv_obj_has_state(s_power_page.brightness_slider, LV_STATE_EDITED) &&
            !lv_obj_has_state(s_power_page.brightness_slider, LV_STATE_FOCUSED)) {
            lv_slider_set_value(s_power_page.brightness_slider, status->power.lcd_brightness, LV_ANIM_OFF);
        }
    }

    if (s_power_page.brightness_val_label) {
        char text[8];
        std::snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(status->power.lcd_brightness));
        set_label_text(s_power_page.brightness_val_label, text);
    }

    const uint8_t charge_value = static_cast<uint8_t>(status->power.charge_current);
    if (s_power_page.observed_charge_value != CHARGE_VALUE_UNKNOWN &&
        s_power_page.observed_charge_value != charge_value) {
        reset_vbat_samples();
    }
    s_power_page.observed_charge_value = charge_value;

    for (auto& option : s_charge_options) {
        const bool selected = (charge_value == option.value);
        set_button_text(option.button, option.text);
        set_power_button_colors(option.button,
                                selected ? COLOR_PRIMARY : COLOR_BUTTON_BG,
                                selected ? 0xFFFFFF : COLOR_TEXT);
    }

    set_power_controls_enabled(true);
}

void power_refresh_timer_cb(lv_timer_t* timer)
{
    (void)timer;

    if (s_power_page.page_alive) {
        refresh_power_ui();
    }
}

void activate_power_page()
{
    if (!s_power_page.page_alive) {
        s_power_page.page_alive = true;
        s_power_page.observed_charge_value = CHARGE_VALUE_UNKNOWN;
        reset_vbat_samples();
    }

    if (!s_power_page.refresh_timer) {
        s_power_page.refresh_timer = lv_timer_create(power_refresh_timer_cb, 1000, nullptr);
    }

    if (!s_power_page.vbat_timer) {
        s_power_page.vbat_timer = lv_timer_create(vbat_timer_cb, VBAT_SAMPLE_PERIOD_MS, nullptr);
    }
}

void power_toggle_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    auto* binding = static_cast<PowerToggleBinding*>(lv_event_get_user_data(e));
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    if (!binding || !tool || !status) {
        set_result_text("Power update failed: device tool is not ready.", COLOR_ERROR);
        return;
    }

    const bool next = !read_power_bool(status, binding->param);

    if (!next && binding->param == StatusParam::POWER_LCD) {
        open_power_warning_dialog(
            PowerDialogAction::LCD_POWER_OFF,
            "LCD Controller Power",
            "You are about to turn off the power rail that drives the LCD controller.\n\n"
            "The display and touch input will stop working immediately. "
            "To keep this demo recoverable, the device will restart automatically "
            "10 seconds after you confirm.");
        return;
    }

    if (!next && binding->param == StatusParam::POWER_PYB_L3B) {
        open_power_warning_dialog(
            PowerDialogAction::L3B_POWER_OFF,
            "L3B Power Rail",
            "You are about to turn off L3B, the shared peripheral power rail.\n\n"
            "Touch, display support, audio and sensors may stop working after this rail is off. "
            "For a complete demo experience, restart is required after this operation.");
        return;
    }

    if (!next && binding->param == StatusParam::POWER_AUDIO_CHIP) {
        open_power_warning_dialog(
            PowerDialogAction::AUDIO_CHIP_POWER_OFF,
            "AudioIC Power",
            "You are about to turn off the audio codec power rail.\n\n"
            "Audio playback and recording will stop, and the audio pipeline may not recover "
            "cleanly without a restart. For a complete demo experience, restart is required "
            "after this operation.");
        return;
    }

    esp_err_t ret = tool->set_status(binding->param,
                                     next,
                                     StatusConfigFlag::APPLY_HARDWARE |
                                         StatusConfigFlag::UPDATE_STATUS |
                                         StatusConfigFlag::SYNC_UI);
    if (ret == ESP_OK) {
        char result_buf[80];
        std::snprintf(result_buf, sizeof(result_buf),
                      "%s power set to %s.",
                      binding->name,
                      next ? "ON" : "OFF");
        set_result_text(result_buf, COLOR_SUCCESS);
        ESP_LOGI(TAG, "%s power -> %s", binding->name, next ? "ON" : "OFF");
    } else {
        char result_buf[96];
        std::snprintf(result_buf, sizeof(result_buf),
                      "%s update failed: %s.",
                      binding->name,
                      esp_err_to_name(ret));
        set_result_text(result_buf, COLOR_ERROR);
        ESP_LOGW(TAG, "%s power update failed: %s", binding->name, esp_err_to_name(ret));
    }

    refresh_power_ui();
}

void brightness_slider_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        set_result_text("Brightness update failed: device tool is not ready.", COLOR_ERROR);
        return;
    }

    lv_obj_t* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = lv_slider_get_value(slider);

    if (value < 1) {
        value = 1;
        lv_slider_set_value(slider, 1, LV_ANIM_OFF);
    }

    esp_err_t ret = tool->set_status(StatusParam::POWER_LCD_BRIGHTNESS,
                                     static_cast<uint8_t>(value),
                                     StatusConfigFlag::APPLY_HARDWARE |
                                         StatusConfigFlag::UPDATE_STATUS |
                                         StatusConfigFlag::SYNC_UI);
    if (ret == ESP_OK) {
        char result_buf[80];
        std::snprintf(result_buf, sizeof(result_buf),
                      "Backlight set to %d%%.",
                      value);
        set_result_text(result_buf, COLOR_SUCCESS);
        ESP_LOGI(TAG, "Backlight -> %d%%", value);
    } else {
        char result_buf[96];
        std::snprintf(result_buf, sizeof(result_buf),
                      "Backlight update failed: %s.",
                      esp_err_to_name(ret));
        set_result_text(result_buf, COLOR_ERROR);
        ESP_LOGW(TAG, "Backlight update failed: %s", esp_err_to_name(ret));
    }

    if (s_power_page.brightness_val_label) {
        char text[16];
        std::snprintf(text, sizeof(text), "%d%%", value);
        set_label_text(s_power_page.brightness_val_label, text);
    }
}

void charge_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    auto* option = static_cast<ChargeOption*>(lv_event_get_user_data(e));
    auto* tool = corep4_ui_sync_get_tool();
    if (!option || !tool) {
        set_result_text("Charge update failed: device tool is not ready.", COLOR_ERROR);
        return;
    }

    esp_err_t ret = tool->set_status(StatusParam::POWER_CHARGE_CURRENT,
                                     option->value,
                                     StatusConfigFlag::APPLY_HARDWARE |
                                         StatusConfigFlag::UPDATE_STATUS |
                                         StatusConfigFlag::SYNC_UI);
    if (ret == ESP_OK) {
        s_power_page.observed_charge_value = option->value;
        reset_vbat_samples();

        char result_buf[80];
        std::snprintf(result_buf, sizeof(result_buf),
                      "Charge current set to %s.",
                      option->text);
        set_result_text(result_buf, COLOR_SUCCESS);
        ESP_LOGI(TAG, "Charge current -> %s", option->text);
    } else {
        char result_buf[96];
        std::snprintf(result_buf, sizeof(result_buf),
                      "Charge update failed: %s.",
                      esp_err_to_name(ret));
        set_result_text(result_buf, COLOR_ERROR);
        ESP_LOGW(TAG, "Charge current update failed: %s", esp_err_to_name(ret));
    }

    refresh_power_ui();
}

void power_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    close_m5bus_gpio_config_popup();
    close_power_dialog();

    if (s_power_page.refresh_timer) {
        lv_timer_delete(s_power_page.refresh_timer);
        s_power_page.refresh_timer = nullptr;
    }

    if (s_power_page.vbat_timer) {
        lv_timer_delete(s_power_page.vbat_timer);
        s_power_page.vbat_timer = nullptr;
    }

    if (s_power_page.m5bus_timer) {
        lv_timer_delete(s_power_page.m5bus_timer);
        s_power_page.m5bus_timer = nullptr;
    }

    s_power_page.page_alive = false;
    s_power_page.status_label = nullptr;
    s_power_page.result_label = nullptr;
    s_power_page.brightness_slider = nullptr;
    s_power_page.brightness_val_label = nullptr;
    s_power_page.vbat_label = nullptr;
    s_power_page.vbat_sample_count = 0;
    s_power_page.vbat_sample_index = 0;
    s_power_page.observed_charge_value = CHARGE_VALUE_UNKNOWN;
    reset_m5bus_bindings();

    for (auto& binding : s_power_toggles) {
        binding.button = nullptr;
    }

    for (auto& option : s_charge_options) {
        option.button = nullptr;
    }
}

lv_obj_t* add_power_button_row(lv_obj_t* parent,
                               PowerToggleBinding& left,
                               PowerToggleBinding& right)
{
    lv_obj_t* row = add_row(parent, 112);
    left.button = add_button(row, "--", COLOR_BUTTON_BG, COLOR_TEXT, 198, 110);
    right.button = add_button(row, "--", COLOR_BUTTON_BG, COLOR_TEXT, 198, 110);

    // 强制设置字体为 28 号字体
    lv_obj_t* left_label = lv_obj_get_child(left.button, 0);
    if (left_label) {
        lv_obj_set_style_text_font(left_label, &lv_font_montserrat_28, LV_PART_MAIN);
    }
    lv_obj_t* right_label = lv_obj_get_child(right.button, 0);
    if (right_label) {
        lv_obj_set_style_text_font(right_label, &lv_font_montserrat_28, LV_PART_MAIN);
    }

    lv_obj_add_event_cb(left.button, power_toggle_event_cb,
                        LV_EVENT_CLICKED, &left);
    lv_obj_add_event_cb(right.button, power_toggle_event_cb,
                        LV_EVENT_CLICKED, &right);
    return row;
}

lv_obj_t* add_charge_bubble(lv_obj_t* parent)
{
    lv_obj_t* bubble = lv_obj_create(parent);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bubble, CHARGE_BUBBLE_WIDTH, CHARGE_CONTROL_HEIGHT);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(0xE0F2FE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bubble, lv_color_hex(0x93C5FD), LV_PART_MAIN);
    lv_obj_set_style_border_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, CHARGE_CONTROL_HEIGHT / 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(bubble, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(bubble, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(bubble, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bubble,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = add_label(bubble, "Charge", COLOR_TEXT, 64);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);

    s_power_page.vbat_label = add_label(bubble, "-.--V", COLOR_PRIMARY, 54);
    lv_label_set_long_mode(s_power_page.vbat_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_power_page.vbat_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    return bubble;
}

void m5bus_pin_text(const M5BusPinDef& pin, char* buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return;
    }

    if (pin.kind == M5BusPinKind::GPIO) {
        std::snprintf(buf, buf_size, "GP-%02d", static_cast<int>(pin.gpio));
    } else if (pin.text && std::strcmp(pin.text, "CHIP_EN") == 0) {
        std::snprintf(buf, buf_size, "CHIP\nEN");
    } else if (pin.text && std::strcmp(pin.text, "P4_3V3_L3B") == 0) {
        std::snprintf(buf, buf_size, "P4_3V3\nL3B");
    } else if (pin.text && std::strcmp(pin.text, "5V_INOUT") == 0) {
        std::snprintf(buf, buf_size, "5V\nINOUT");
    } else {
        std::snprintf(buf, buf_size, "%s", pin.text ? pin.text : "");
    }
}

uint32_t m5bus_pin_text_color(const M5BusPinDef& pin)
{
    if (pin.kind == M5BusPinKind::VOLTAGE || pin.kind == M5BusPinKind::GPIO) {
        return COLOR_TEXT;
    }

    if (pin.text && std::strcmp(pin.text, "GND") == 0) {
        return 0xFFFFFF;
    }

    return COLOR_MUTED;
}

bool m5bus_pin_is_gnd(const M5BusPinDef& pin)
{
    return pin.text && std::strcmp(pin.text, "GND") == 0;
}

uint32_t m5bus_pin_bg_color(const M5BusPinDef& pin)
{
    if (pin.kind == M5BusPinKind::GPIO) {
        return M5BUS_COLOR_GPIO_BG;
    }

    if (pin.kind == M5BusPinKind::VOLTAGE) {
        return M5BUS_COLOR_POWER_BG;
    }

    if (m5bus_pin_is_gnd(pin)) {
        return M5BUS_COLOR_GND_BG;
    }

    return COLOR_BUTTON_BG;
}

uint32_t m5bus_pin_border_color(const M5BusPinDef& pin)
{
    if (pin.kind == M5BusPinKind::GPIO) {
        return M5BUS_COLOR_GPIO_BORDER;
    }

    if (pin.kind == M5BusPinKind::VOLTAGE) {
        return M5BUS_COLOR_POWER_BORDER;
    }

    if (m5bus_pin_is_gnd(pin)) {
        return M5BUS_COLOR_GND_BORDER;
    }

    return COLOR_BORDER;
}

void reset_m5bus_bindings()
{
    close_m5bus_gpio_config_popup();
    s_power_page.m5bus_binding_count = 0;
    for (auto& binding : s_power_page.m5bus_bindings) {
        binding = {};
    }
}

bool m5bus_gpio_is_output(const M5BusGpioConfig& config)
{
    return config.options.mode == M5BusGpioMode::Output;
}

bool m5bus_gpio_is_active_popup_target(const M5BusGpioConfig& config)
{
    return s_m5bus_gpio_popup.target == &config;
}

bool m5bus_gpio_is_effective_output(const M5BusGpioConfig& config)
{
    if (m5bus_gpio_is_active_popup_target(config)) {
        return s_m5bus_gpio_popup.draft.mode == M5BusGpioMode::Output;
    }

    return config.options.mode == M5BusGpioMode::Output;
}

bool m5bus_gpio_effective_output_high(const M5BusGpioConfig& config)
{
    return m5bus_gpio_is_active_popup_target(config)
               ? s_m5bus_gpio_popup.draft.output_high
               : config.options.output_high;
}

uint32_t m5bus_gpio_bg_color(const M5BusGpioConfig& config)
{
    return m5bus_gpio_is_output(config) ? M5BUS_COLOR_GPIO_OUTPUT_BG
                                        : M5BUS_COLOR_GPIO_BG;
}

uint32_t m5bus_gpio_border_color(const M5BusGpioConfig& config)
{
    return m5bus_gpio_is_output(config) ? M5BUS_COLOR_GPIO_OUTPUT_BORDER
                                        : M5BUS_COLOR_GPIO_BORDER;
}

void refresh_m5bus_gpio_card_style(M5BusGpioConfig& config)
{
    if (!config.cell) {
        return;
    }

    lv_obj_set_style_bg_color(config.cell,
                              lv_color_hex(m5bus_gpio_bg_color(config)),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(config.cell,
                                  lv_color_hex(m5bus_gpio_border_color(config)),
                                  LV_PART_MAIN);
}

void set_m5bus_level_indicator(lv_obj_t* indicator, int level, bool output_mode)
{
    if (!indicator) {
        return;
    }

    const bool high = level != 0;
    const uint32_t high_color = output_mode ? M5BUS_COLOR_LEVEL_OUTPUT_HIGH
                                            : M5BUS_COLOR_LEVEL_HIGH;
    lv_obj_set_style_bg_color(indicator,
                              lv_color_hex(high ? high_color
                                                : M5BUS_COLOR_LEVEL_LOW),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(indicator,
                                  lv_color_hex(high ? high_color
                                                    : M5BUS_COLOR_LEVEL_LOW_BORDER),
                                  LV_PART_MAIN);
}

M5BusGpioConfig* add_m5bus_gpio_binding(const M5BusPinDef* pin,
                                        lv_obj_t* cell,
                                        lv_obj_t* signal_label,
                                        lv_obj_t* level_indicator)
{
    if (!pin || pin->kind != M5BusPinKind::GPIO || !cell || !level_indicator ||
        s_power_page.m5bus_binding_count >= M5BUS_MAX_GPIO_BINDINGS) {
        return nullptr;
    }

    auto& binding = s_power_page.m5bus_bindings[s_power_page.m5bus_binding_count++];
    binding.pin = pin;
    binding.cell = cell;
    binding.level_indicator = level_indicator;
    binding.signal_label = signal_label;
    binding.last_level = -1;
    binding.options = {};
    refresh_m5bus_gpio_card_style(binding);
    return &binding;
}

void refresh_m5bus_gpio_indicator(M5BusGpioConfig& config)
{
    if (!config.pin || !config.level_indicator) {
        return;
    }

    if (!GPIO_IS_VALID_GPIO(config.pin->gpio)) {
        set_m5bus_level_indicator(config.level_indicator, 0, false);
        return;
    }

    if (m5bus_gpio_is_effective_output(config)) {
        set_m5bus_level_indicator(config.level_indicator,
                                  m5bus_gpio_effective_output_high(config) ? 1 : 0,
                                  true);
        return;
    }

    const int level = gpio_get_level(config.pin->gpio) ? 1 : 0;
    set_m5bus_level_indicator(config.level_indicator, level, false);
    config.last_level = level;
}

const char* m5bus_gpio_mode_text(M5BusGpioMode mode)
{
    return mode == M5BusGpioMode::Output ? "Output" : "Input";
}

const char* m5bus_gpio_drive_text(M5BusGpioDrive drive)
{
    return drive == M5BusGpioDrive::PushPull ? "PushPull" : "OpenDrain";
}

const char* m5bus_gpio_pull_text(M5BusGpioPull pull)
{
    switch (pull) {
        case M5BusGpioPull::Up: return "PullUp";
        case M5BusGpioPull::Down: return "PullDown";
        case M5BusGpioPull::None:
        default:
            return "NoPull";
    }
}

void close_m5bus_gpio_config_popup()
{
    corep4_ui_close_function_dialog(&s_m5bus_gpio_popup.dialog);
    s_m5bus_gpio_popup = {};
}

void refresh_m5bus_gpio_popup_buttons()
{
    const auto& draft = s_m5bus_gpio_popup.draft;
    const bool output_mode = draft.mode == M5BusGpioMode::Output;

    set_power_button_colors(s_m5bus_gpio_popup.mode_input_button,
                            !output_mode ? M5BUS_COLOR_GPIO_BG : COLOR_BUTTON_BG,
                            COLOR_TEXT);
    set_power_button_colors(s_m5bus_gpio_popup.mode_output_button,
                            output_mode ? M5BUS_COLOR_GPIO_OUTPUT_BG : COLOR_BUTTON_BG,
                            COLOR_TEXT);
    set_power_button_colors(s_m5bus_gpio_popup.level_low_button,
                            !draft.output_high ? M5BUS_COLOR_LEVEL_LOW : COLOR_BUTTON_BG,
                            COLOR_TEXT);
    set_power_button_colors(s_m5bus_gpio_popup.level_high_button,
                            draft.output_high ? M5BUS_COLOR_LEVEL_OUTPUT_HIGH : COLOR_BUTTON_BG,
                            COLOR_TEXT);
    set_power_button_colors(s_m5bus_gpio_popup.drive_od_button,
                            draft.drive == M5BusGpioDrive::OpenDrain ? M5BUS_COLOR_GPIO_BG : COLOR_BUTTON_BG,
                            COLOR_TEXT);
    set_power_button_colors(s_m5bus_gpio_popup.drive_pp_button,
                            draft.drive == M5BusGpioDrive::PushPull ? M5BUS_COLOR_GPIO_OUTPUT_BG : COLOR_BUTTON_BG,
                            COLOR_TEXT);
    set_power_button_colors(s_m5bus_gpio_popup.pull_none_button,
                            draft.pull == M5BusGpioPull::None ? M5BUS_COLOR_GPIO_BG : COLOR_BUTTON_BG,
                            COLOR_TEXT);
    set_power_button_colors(s_m5bus_gpio_popup.pull_up_button,
                            draft.pull == M5BusGpioPull::Up ? M5BUS_COLOR_GPIO_BG : COLOR_BUTTON_BG,
                            COLOR_TEXT);
    set_power_button_colors(s_m5bus_gpio_popup.pull_down_button,
                            draft.pull == M5BusGpioPull::Down ? M5BUS_COLOR_GPIO_BG : COLOR_BUTTON_BG,
                            COLOR_TEXT);

    set_button_enabled(s_m5bus_gpio_popup.level_low_button, output_mode);
    set_button_enabled(s_m5bus_gpio_popup.level_high_button, output_mode);

    if (s_m5bus_gpio_popup.status_label) {
        char status[128];
        std::snprintf(status, sizeof(status), "%s / %s / %s / %s",
                      m5bus_gpio_mode_text(draft.mode),
                      output_mode ? (draft.output_high ? "High" : "Low") : "Level ignored",
                      m5bus_gpio_drive_text(draft.drive),
                      m5bus_gpio_pull_text(draft.pull));
        set_label_text(s_m5bus_gpio_popup.status_label, status);
    }

    refresh_m5bus_gpio_levels();
}

esp_err_t apply_m5bus_gpio_config(M5BusGpioConfig& config,
                                  const M5BusGpioOptions& options)
{
    if (!config.pin || !GPIO_IS_VALID_GPIO(config.pin->gpio)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (options.mode == M5BusGpioMode::Output &&
        !GPIO_IS_VALID_OUTPUT_GPIO(config.pin->gpio)) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << static_cast<uint32_t>(config.pin->gpio);
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = (options.mode == M5BusGpioMode::Output)
                       ? (options.drive == M5BusGpioDrive::PushPull
                              ? GPIO_MODE_OUTPUT
                              : GPIO_MODE_OUTPUT_OD)
                       : GPIO_MODE_INPUT;
    io_conf.pull_up_en = (options.pull == M5BusGpioPull::Up)
                             ? GPIO_PULLUP_ENABLE
                             : GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = (options.pull == M5BusGpioPull::Down)
                               ? GPIO_PULLDOWN_ENABLE
                               : GPIO_PULLDOWN_DISABLE;

    esp_err_t ret = gpio_config(&io_conf);
    if (ret == ESP_OK && options.mode == M5BusGpioMode::Output) {
        ret = gpio_set_level(config.pin->gpio, options.output_high ? 1 : 0);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    config.options = options;
    config.last_level = -1;
    refresh_m5bus_gpio_card_style(config);
    refresh_m5bus_gpio_levels();
    return ESP_OK;
}

void m5bus_gpio_popup_action_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    const auto* action = static_cast<const M5BusGpioPopupAction*>(lv_event_get_user_data(e));
    if (!action) {
        return;
    }

    switch (*action) {
        case M5BusGpioPopupAction::MODE_INPUT:
            s_m5bus_gpio_popup.draft.mode = M5BusGpioMode::Input;
            break;
        case M5BusGpioPopupAction::MODE_OUTPUT:
            s_m5bus_gpio_popup.draft.mode = M5BusGpioMode::Output;
            break;
        case M5BusGpioPopupAction::LEVEL_LOW:
            s_m5bus_gpio_popup.draft.output_high = false;
            break;
        case M5BusGpioPopupAction::LEVEL_HIGH:
            s_m5bus_gpio_popup.draft.output_high = true;
            break;
        case M5BusGpioPopupAction::DRIVE_OPEN_DRAIN:
            s_m5bus_gpio_popup.draft.drive = M5BusGpioDrive::OpenDrain;
            break;
        case M5BusGpioPopupAction::DRIVE_PUSH_PULL:
            s_m5bus_gpio_popup.draft.drive = M5BusGpioDrive::PushPull;
            break;
        case M5BusGpioPopupAction::PULL_NONE:
            s_m5bus_gpio_popup.draft.pull = M5BusGpioPull::None;
            break;
        case M5BusGpioPopupAction::PULL_UP:
            s_m5bus_gpio_popup.draft.pull = M5BusGpioPull::Up;
            break;
        case M5BusGpioPopupAction::PULL_DOWN:
            s_m5bus_gpio_popup.draft.pull = M5BusGpioPull::Down;
            break;
        case M5BusGpioPopupAction::APPLY: {
            if (!s_m5bus_gpio_popup.target) {
                return;
            }

            esp_err_t ret = apply_m5bus_gpio_config(*s_m5bus_gpio_popup.target,
                                                    s_m5bus_gpio_popup.draft);
            if (ret == ESP_OK) {
                char result[96];
                std::snprintf(result, sizeof(result), "%s GPIO%d configured as %s.",
                              s_m5bus_gpio_popup.target->pin->pos,
                              static_cast<int>(s_m5bus_gpio_popup.target->pin->gpio),
                              m5bus_gpio_mode_text(s_m5bus_gpio_popup.draft.mode));
                set_result_text(result, COLOR_SUCCESS);
                close_m5bus_gpio_config_popup();
            } else {
                char result[96];
                std::snprintf(result, sizeof(result), "GPIO config failed: %s.",
                              esp_err_to_name(ret));
                set_result_text(result, COLOR_ERROR);
                set_label_text(s_m5bus_gpio_popup.status_label, result);
                ESP_LOGW(TAG, "M5BUS GPIO config failed: %s", esp_err_to_name(ret));
            }
            return;
        }
        case M5BusGpioPopupAction::CLOSE:
            close_m5bus_gpio_config_popup();
            refresh_m5bus_gpio_levels();
            return;
    }

    refresh_m5bus_gpio_popup_buttons();
}

lv_obj_t* add_m5bus_gpio_popup_button(lv_obj_t* parent,
                                      const char* text,
                                      const M5BusGpioPopupAction* action)
{
    lv_obj_t* button = add_button(parent,
                                  text,
                                  COLOR_BUTTON_BG,
                                  COLOR_TEXT,
                                  M5BUS_GPIO_POPUP_BUTTON_W,
                                  M5BUS_GPIO_POPUP_BUTTON_H);
    lv_obj_add_event_cb(button,
                        m5bus_gpio_popup_action_cb,
                        LV_EVENT_CLICKED,
                        const_cast<M5BusGpioPopupAction*>(action));
    return button;
}

lv_obj_t* add_m5bus_gpio_popup_row(lv_obj_t* panel, const char* name)
{
    lv_obj_t* row = add_row(panel, M5BUS_GPIO_POPUP_BUTTON_H);
    add_label(row, name, COLOR_MUTED, 74);
    return row;
}

void open_m5bus_gpio_config_popup(M5BusGpioConfig* config)
{
    if (!config || !config->pin || config->pin->kind != M5BusPinKind::GPIO) {
        return;
    }

    close_m5bus_gpio_config_popup();
    refresh_m5bus_gpio_levels();
    s_m5bus_gpio_popup.target = config;
    s_m5bus_gpio_popup.draft = config->options;

    char title[64];
    std::snprintf(title, sizeof(title), "%s  GPIO%d",
                  config->pin->pos ? config->pin->pos : "M5BUS",
                  static_cast<int>(config->pin->gpio));
    const CoreP4FunctionDialogButtonConfig buttons[] = {
        {"Apply", m5bus_gpio_popup_action_cb, const_cast<M5BusGpioPopupAction*>(&GPIO_ACTION_APPLY),
         COLOR_BUTTON_BG, COLOR_TEXT, false},
        {"Close", m5bus_gpio_popup_action_cb, const_cast<M5BusGpioPopupAction*>(&GPIO_ACTION_CLOSE),
         COLOR_BUTTON_BG, COLOR_TEXT, false},
    };
    s_m5bus_gpio_popup.dialog = corep4_ui_open_function_dialog(
        title, "", CoreP4FunctionDialogSize::LARGE, buttons, sizeof(buttons) / sizeof(buttons[0]));
    if (s_m5bus_gpio_popup.dialog.text_label) {
        lv_obj_add_flag(s_m5bus_gpio_popup.dialog.text_label, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* content = lv_msgbox_get_content(s_m5bus_gpio_popup.dialog.box);
    if (!content) {
        close_m5bus_gpio_config_popup();
        return;
    }

    lv_obj_set_style_pad_top(content, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(content, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    s_m5bus_gpio_popup.status_label = add_label(content, "", COLOR_MUTED);

    lv_obj_t* mode_row = add_m5bus_gpio_popup_row(content, "Mode");
    s_m5bus_gpio_popup.mode_input_button = add_m5bus_gpio_popup_button(mode_row, "Input", &GPIO_ACTION_MODE_INPUT);
    s_m5bus_gpio_popup.mode_output_button = add_m5bus_gpio_popup_button(mode_row, "Output", &GPIO_ACTION_MODE_OUTPUT);

    lv_obj_t* level_row = add_m5bus_gpio_popup_row(content, "Level");
    s_m5bus_gpio_popup.level_low_button = add_m5bus_gpio_popup_button(level_row, "Low", &GPIO_ACTION_LEVEL_LOW);
    s_m5bus_gpio_popup.level_high_button = add_m5bus_gpio_popup_button(level_row, "High", &GPIO_ACTION_LEVEL_HIGH);

    lv_obj_t* drive_row = add_m5bus_gpio_popup_row(content, "Drive");
    s_m5bus_gpio_popup.drive_od_button = add_m5bus_gpio_popup_button(drive_row, "OpenDrain", &GPIO_ACTION_DRIVE_OPEN_DRAIN);
    s_m5bus_gpio_popup.drive_pp_button = add_m5bus_gpio_popup_button(drive_row, "PushPull", &GPIO_ACTION_DRIVE_PUSH_PULL);

    lv_obj_t* pull_row = add_m5bus_gpio_popup_row(content, "Pull");
    s_m5bus_gpio_popup.pull_none_button = add_m5bus_gpio_popup_button(pull_row, "None", &GPIO_ACTION_PULL_NONE);
    s_m5bus_gpio_popup.pull_up_button = add_m5bus_gpio_popup_button(pull_row, "Up", &GPIO_ACTION_PULL_UP);
    s_m5bus_gpio_popup.pull_down_button = add_m5bus_gpio_popup_button(pull_row, "Down", &GPIO_ACTION_PULL_DOWN);

    refresh_m5bus_gpio_popup_buttons();
}

void m5bus_gpio_cell_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    auto* config = static_cast<M5BusGpioConfig*>(lv_event_get_user_data(e));
    open_m5bus_gpio_config_popup(config);
}

void refresh_m5bus_gpio_levels()
{
    for (size_t i = 0; i < s_power_page.m5bus_binding_count; ++i) {
        refresh_m5bus_gpio_indicator(s_power_page.m5bus_bindings[i]);
    }
}

void m5bus_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    if (s_power_page.page_alive) {
        refresh_m5bus_gpio_levels();
    }
}

void activate_m5bus_page()
{
    if (!s_power_page.m5bus_timer) {
        s_power_page.m5bus_timer = lv_timer_create(m5bus_timer_cb,
                                                   M5BUS_SAMPLE_PERIOD_MS,
                                                   nullptr);
    }

    refresh_m5bus_gpio_levels();
}

lv_obj_t* add_m5bus_layout(lv_obj_t* parent)
{
    lv_obj_t* layout = lv_obj_create(parent);
    lv_obj_remove_flag(layout, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(layout, lv_pct(100));
    lv_obj_set_height(layout, M5BUS_GROUP_HEIGHT);
    lv_obj_set_style_bg_opa(layout, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(layout, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(layout, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(layout, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(layout, M5BUS_GROUP_GAP, LV_PART_MAIN);
    lv_obj_set_flex_flow(layout, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(layout,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    return layout;
}

lv_obj_t* add_m5bus_column_header(lv_obj_t* parent, const char* text)
{
    lv_obj_t* header = lv_obj_create(parent);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(header, lv_pct(100), M5BUS_COLUMN_HEADER_HEIGHT);
    lv_obj_set_style_bg_color(header, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(header);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_width(label, M5BUS_COLUMN_WIDTH - 6);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(M5BUS_COLOR_LABEL_STRONG), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_center(label);
    return header;
}

lv_obj_t* add_m5bus_pin_card(lv_obj_t* parent, const M5BusPinDef* pin)
{
    lv_obj_t* cell = lv_button_create(parent);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cell, lv_pct(100), M5BUS_PIN_CELL_HEIGHT);
    lv_obj_set_layout(cell, LV_LAYOUT_NONE);
    lv_obj_set_style_bg_color(cell,
                              lv_color_hex(pin ? m5bus_pin_bg_color(*pin)
                                               : COLOR_BUTTON_BG),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(cell,
                                  lv_color_hex(pin ? m5bus_pin_border_color(*pin)
                                                   : COLOR_BORDER),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(cell, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cell, 2, LV_PART_MAIN);

    if (!pin) {
        return cell;
    }

    char signal_text[20];
    m5bus_pin_text(*pin, signal_text, sizeof(signal_text));
    const uint32_t label_color = m5bus_pin_text_color(*pin);
    const bool signal_two_line = std::strchr(signal_text, '\n') != nullptr;

    lv_obj_t* pos_label = lv_label_create(cell);
    lv_label_set_text(pos_label, pin->pos ? pin->pos : "");
    lv_obj_set_pos(pos_label, 4, 2);
    lv_obj_set_width(pos_label, M5BUS_COLUMN_WIDTH - 10);
    lv_label_set_long_mode(pos_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(pos_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_color(pos_label, lv_color_hex(label_color), LV_PART_MAIN);
    lv_obj_set_style_text_font(pos_label, &lv_font_montserrat_10, LV_PART_MAIN);

    lv_obj_t* signal_area = lv_obj_create(cell);
    lv_obj_remove_flag(signal_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(signal_area, 0, signal_two_line ? M5BUS_SIGNAL_AREA_TWO_LINE_Y : M5BUS_SIGNAL_AREA_Y);
    lv_obj_set_size(signal_area, lv_pct(100), M5BUS_SIGNAL_AREA_HEIGHT);
    lv_obj_set_style_bg_opa(signal_area, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(signal_area, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(signal_area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(signal_area, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(signal_area, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(signal_area,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* signal_label = lv_label_create(signal_area);
    lv_label_set_text(signal_label, signal_text);
    lv_obj_set_width(signal_label, M5BUS_COLUMN_WIDTH - 8);
    lv_label_set_long_mode(signal_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(signal_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(signal_label, lv_color_hex(label_color), LV_PART_MAIN);
    lv_obj_set_style_text_font(signal_label, &lv_font_montserrat_12, LV_PART_MAIN);

    M5BusGpioConfig* gpio_config = nullptr;
    if (pin->kind == M5BusPinKind::GPIO) {
        lv_obj_t* level_indicator = lv_obj_create(cell);
        lv_obj_remove_flag(level_indicator, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(level_indicator,
                        M5BUS_LEVEL_INDICATOR_SIZE,
                        M5BUS_LEVEL_INDICATOR_SIZE);
        lv_obj_align(level_indicator,
                     LV_ALIGN_TOP_RIGHT,
                     -M5BUS_LEVEL_INDICATOR_INSET,
                     M5BUS_LEVEL_INDICATOR_INSET);
        lv_obj_set_style_bg_opa(level_indicator, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(level_indicator, 1, LV_PART_MAIN);
        lv_obj_set_style_border_opa(level_indicator, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(level_indicator, 99, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(level_indicator, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(level_indicator, 0, LV_PART_MAIN);
        set_m5bus_level_indicator(level_indicator, 0, false);
        gpio_config = add_m5bus_gpio_binding(pin, cell, signal_label, level_indicator);
    }

    if (gpio_config) {
        lv_obj_add_event_cb(cell,
                            m5bus_gpio_cell_event_cb,
                            LV_EVENT_CLICKED,
                            gpio_config);
    }
    return cell;
}

void add_m5bus_pin_column(lv_obj_t* parent,
                          const char* side,
                          const M5BusPinGroup& group)
{
    lv_obj_t* column = lv_obj_create(parent);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(column, M5BUS_COLUMN_WIDTH, M5BUS_COLUMN_HEIGHT);
    lv_obj_set_style_bg_color(column, lv_color_hex(COLOR_CARD_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(column, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(column, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(column, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(column, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(column, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(column, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(column, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(column, 3, LV_PART_MAIN);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    add_m5bus_column_header(column, side);

    const bool left_side = side && side[0] == 'L';
    for (size_t i = 0; i < group.row_count; ++i) {
        add_m5bus_pin_card(column, left_side ? group.rows[i].left
                                            : group.rows[i].right);
    }
}

void add_m5bus_group(lv_obj_t* parent, const M5BusPinGroup& group)
{
    lv_obj_t* group_box = lv_obj_create(parent);
    lv_obj_remove_flag(group_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(group_box, M5BUS_GROUP_WIDTH, M5BUS_GROUP_HEIGHT);
    lv_obj_set_style_bg_opa(group_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(group_box, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(group_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(group_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(group_box, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(group_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(group_box,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(group_box);
    lv_label_set_text(title, group.title ? group.title : "M5BUS");
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(M5BUS_COLOR_LABEL_STRONG), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_10, LV_PART_MAIN);

    lv_obj_t* columns = lv_obj_create(group_box);
    lv_obj_remove_flag(columns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(columns, lv_pct(100), M5BUS_COLUMN_HEIGHT);
    lv_obj_set_style_bg_opa(columns, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(columns, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(columns, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(columns, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(columns, M5BUS_COLUMN_GAP, LV_PART_MAIN);
    lv_obj_set_flex_flow(columns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(columns,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    add_m5bus_pin_column(columns, "L", group);
    add_m5bus_pin_column(columns, "R", group);
}

void build_tab1_content(lv_obj_t* parent, const CoreP4FunctionPagedWindowConfig& /*config*/)
{
    prepare_content(parent);
    activate_power_page();
    lv_obj_add_event_cb(parent, power_content_delete_event_cb, LV_EVENT_DELETE, nullptr);

    lv_obj_t* card = add_card(parent, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);

    add_label(card, "Peripheral Power", COLOR_TEXT);

    // USBA (4) 和 P4USBC (5)
    add_power_button_row(card, s_power_toggles[4], s_power_toggles[5]);
    // Grove (6) 和 M5BUS (7)
    add_power_button_row(card, s_power_toggles[6], s_power_toggles[7]);

    // 背光亮度标题与数值行（Header 行）
    lv_obj_t* brightness_header_row = add_row(card, 26);
    add_label(brightness_header_row, "Backlight", COLOR_MUTED, 120);
    s_power_page.brightness_val_label = add_label(brightness_header_row, "--%", COLOR_TEXT, 262);
    lv_obj_set_style_text_align(s_power_page.brightness_val_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    // 背光滑条独占的一行（Slider 行）
    lv_obj_t* brightness_slider_row = add_row(card, 32);
    s_power_page.brightness_slider = lv_slider_create(brightness_slider_row);
    lv_obj_set_width(s_power_page.brightness_slider, 390);
    lv_obj_set_height(s_power_page.brightness_slider, 20);
    lv_slider_set_range(s_power_page.brightness_slider, 1, 100);

    lv_obj_set_style_bg_color(s_power_page.brightness_slider, lv_color_hex(COLOR_BUTTON_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_power_page.brightness_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_power_page.brightness_slider, 10, LV_PART_MAIN);

    lv_obj_set_style_bg_color(s_power_page.brightness_slider, lv_color_hex(COLOR_PRIMARY), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_power_page.brightness_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_power_page.brightness_slider, 10, LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(s_power_page.brightness_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_power_page.brightness_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(s_power_page.brightness_slider, 15, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_power_page.brightness_slider, 6, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_power_page.brightness_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_power_page.brightness_slider, lv_color_hex(COLOR_BORDER), LV_PART_KNOB);

    // 绑定滑条事件
    lv_obj_add_event_cb(s_power_page.brightness_slider, brightness_slider_event_cb,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    refresh_power_ui();
}

void build_tab2_content(lv_obj_t* parent, const CoreP4FunctionPagedWindowConfig& /*config*/)
{
    prepare_content(parent);
    activate_power_page();
    lv_obj_add_event_cb(parent, power_content_delete_event_cb, LV_EVENT_DELETE, nullptr);

    lv_obj_t* card = add_card(parent, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);

    add_label(card, "System Power", COLOR_TEXT);

    // L3B (0) 和 LCD (1)
    add_power_button_row(card, s_power_toggles[0], s_power_toggles[1]);
    // AudioIC (2) 和 AudioPA (3)
    add_power_button_row(card, s_power_toggles[2], s_power_toggles[3]);

    // 充电电流
    lv_obj_t* charge_row = add_row(card, CHARGE_CONTROL_HEIGHT);
    lv_obj_set_width(charge_row, POWER_BUTTON_ROW_WIDTH);
    add_charge_bubble(charge_row);
    for (auto& option : s_charge_options) {
        option.button = add_button(charge_row,
                                   option.text,
                                   COLOR_BUTTON_BG,
                                   COLOR_TEXT,
                                   CHARGE_BUTTON_WIDTH,
                                   CHARGE_CONTROL_HEIGHT);
        lv_obj_add_event_cb(option.button, charge_event_cb,
                            LV_EVENT_CLICKED, &option);
    }

    refresh_power_ui();
}

void build_tab3_content(lv_obj_t* parent, const CoreP4FunctionPagedWindowConfig& /*config*/)
{
    prepare_content(parent);
    lv_obj_set_style_pad_row(parent, 0, LV_PART_MAIN);
    reset_m5bus_bindings();

    lv_obj_t* layout = add_m5bus_layout(parent);
    for (const auto& group : M5BUS_PIN_GROUPS) {
        add_m5bus_group(layout, group);
    }

    activate_m5bus_page();
}

constexpr CoreP4FunctionPagedWindowConfig WINDOW_CONFIG = {
    "Power",
    "click on ui_func1power1con",
    "Peripheral",
    "System",
    build_tab1_content,
    build_tab2_content,
    "M5BUS",
    build_tab3_content,
};

} // namespace

extern "C" void func2power2clicked2func(lv_event_t* e)
{
    corep4_ui_open_paged_function_window(e, WINDOW_CONFIG);
}
