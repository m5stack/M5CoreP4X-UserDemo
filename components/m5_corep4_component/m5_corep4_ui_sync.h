/**
 * @file m5_corep4_ui_sync.h
 * @brief Public entry points for CoreP4 LVGL UI synchronization.
 */

#ifndef __M5_COREP4_UI_SYNC_H__
#define __M5_COREP4_UI_SYNC_H__

#include "m5_corep4_component.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SENSOR_TYPE_ACC = 0,
    SENSOR_TYPE_GRY,
    SENSOR_TYPE_MAG,
    SENSOR_TYPE_MAX
} sensor_type_t;

typedef struct {
    const char* name;
    uint32_t color;
    float scale_factor;
    int center_offset;
    lv_chart_series_t* series;
} bmi_wave_line_t;

typedef struct {
    sensor_type_t current_sensor;
    bmi_wave_line_t lines[3];
    bool data_updated;
    bool sensor_changed;
} sensor_display_manager_t;

extern sensor_display_manager_t g_sensor_manager;

void switch_sensor_display(sensor_type_t new_sensor_type);
void recreate_chart_series(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

esp_err_t ui_sync_init(m5_corep4_tool& tool);
esp_err_t ui_sync_bindAllEvents(m5_corep4_tool& tool);

/* Compatibility wrappers for code that used the old module-level API. */
esp_err_t power_ui_sync_init(m5_corep4_tool& tool);
esp_err_t power_ui_bind_checkbox_events(m5_corep4_tool& tool);
esp_err_t sleep_ui_sync_init(m5_corep4_tool& tool);
esp_err_t sleep_ui_bind_checkbox_events(m5_corep4_tool& tool);
esp_err_t sleep_ui_sync_to_checkbox(m5_corep4_tool& tool);
esp_err_t sd_ui_sync_init(m5_corep4_tool& tool);
esp_err_t eth_ui_sync_init(m5_corep4_tool& tool);

#endif

#endif // __M5_COREP4_UI_SYNC_H__
