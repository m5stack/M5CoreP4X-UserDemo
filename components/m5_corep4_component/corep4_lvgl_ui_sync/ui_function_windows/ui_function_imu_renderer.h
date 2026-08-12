#pragma once

#include "ui_function_imu_attitude.h"

#include <cstdint>

void corep4_imu_render_device(uint8_t *rgb565_buffer, uint32_t stride_bytes,
                              int32_t width, int32_t height,
                              CoreP4ImuQuaternion relative_orientation);
