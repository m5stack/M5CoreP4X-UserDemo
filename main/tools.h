#ifndef __TOOLS_H__
#define __TOOLS_H__

#include <cstdint>

#include "m5_corep4_component.h"
#include "m5_py32bin_tools.h"

/**
 * @brief 配置 ContextBusAdapter 使用 USB Serial JTAG (CDC)
 *
 * @param adapter ContextBusAdapter 对象引用
 * @return esp_err_t ESP_OK 表示成功
 */
esp_err_t setup_context_bus_with_usb_serial_jtag(m5::py32bin::ContextBusAdapter& adapter);

extern m5_corep4_tool corep4;
void log_firmware_metadata(const char* tag);
void repair_rtc_time_if_needed(m5_corep4_tool& tool);
void start_ethernet_wait_task(m5_corep4_tool& tool);
void play_startup_prompt_sound(m5_corep4_tool& tool, uint32_t delay_ms);
void lcd_and_touch_test(void);

#endif  // __TOOLS_H__
