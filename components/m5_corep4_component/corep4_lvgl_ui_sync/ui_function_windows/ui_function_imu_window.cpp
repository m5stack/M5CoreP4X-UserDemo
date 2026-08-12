#include "ui_function_widgets.h"

#include "ui_function_imu_attitude.h"
#include "ui_function_imu_renderer.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>

namespace {

using namespace corep4_function_ui;

constexpr const char *TAG = "UI_FUNC_IMU";
constexpr uint32_t IMU_UPDATE_PERIOD_MS = 25;
constexpr uint32_t IMU_SAMPLE_PERIOD_MS = 10;
constexpr int IMU_INIT_FAILURE_CLOSE_MS = 5000;
constexpr int32_t VIEW_WIDTH = 460;
constexpr int32_t VIEW_HEIGHT = 418;
constexpr float TOUCH_DRAG_RADIANS_PER_PIXEL = 0.006f;
constexpr int32_t TOUCH_MARKER_SIZE = 28;
constexpr uint32_t TOUCH_MARKER_COLORS[2] = {COLOR_PRIMARY, 0x0891B2};

enum class TouchGestureMode : uint8_t {
  None,
  Drag,
  Twist,
};

struct ImuInitTaskContext {
  uint32_t generation = 0;
};

struct ImuPageState {
  lv_obj_t *canvas = nullptr;
  lv_obj_t *loading_container = nullptr;
  lv_obj_t *loading_label = nullptr;
  lv_obj_t *touch_markers[2] = {};
  lv_obj_t *window_backdrop = nullptr;
  lv_timer_t *timer = nullptr;
  uint8_t *canvas_buffer = nullptr;
  uint32_t canvas_stride = 0;

  CoreP4ImuQuaternion latest_relative = {};
  CoreP4ImuQuaternion view_offset = {};
  uint32_t pose_sequence = 0;
  uint32_t rendered_pose_sequence = 0;
  uint32_t view_sequence = 0;
  uint32_t rendered_view_sequence = 0;
  bool pose_ready = false;
  bool recenter_requested = false;
  bool calibration_status_visible = false;
  TaskHandle_t fusion_task_handle = nullptr;
  volatile bool fusion_stop_requested = false;

  TouchGestureMode touch_mode = TouchGestureMode::None;
  int32_t last_touch_x = 0;
  int32_t last_touch_y = 0;
  float last_twist_angle = 0.0f;

  volatile bool page_alive = false;
  volatile bool task_running = false;
  volatile bool init_failed = false;
  volatile esp_err_t init_result = ESP_OK;
  uint32_t generation = 0;
  bool init_failure_countdown_active = false;
  int init_failure_close_ms = 0;
};

ImuPageState s_imu_page;
volatile uint32_t s_imu_page_generation = 0;
portMUX_TYPE s_pose_lock = portMUX_INITIALIZER_UNLOCKED;

void render_device(CoreP4ImuQuaternion relative = {}) {
  if (!s_imu_page.canvas || !s_imu_page.canvas_buffer ||
      s_imu_page.canvas_stride == 0) {
    return;
  }
  corep4_imu_render_device(s_imu_page.canvas_buffer, s_imu_page.canvas_stride,
                           VIEW_WIDTH, VIEW_HEIGHT, relative);
  lv_obj_invalidate(s_imu_page.canvas);
}

void canvas_buffer_delete_event_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_DELETE) {
    return;
  }
  void *buffer = lv_event_get_user_data(event);
  if (buffer) {
    heap_caps_free(buffer);
  }
}

bool create_canvas(lv_obj_t *parent) {
  s_imu_page.canvas = lv_canvas_create(parent);
  lv_obj_remove_flag(s_imu_page.canvas, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_imu_page.canvas, VIEW_WIDTH, VIEW_HEIGHT);
  lv_obj_set_pos(s_imu_page.canvas, 0, 0);

  const lv_color_format_t format = LV_COLOR_FORMAT_RGB565;
  const uint32_t stride = lv_draw_buf_width_to_stride(VIEW_WIDTH, format);
  const size_t size = static_cast<size_t>(stride) * VIEW_HEIGHT;
  void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
    buffer = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
  }
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate IMU canvas: %u bytes",
             static_cast<unsigned>(size));
    lv_obj_delete(s_imu_page.canvas);
    s_imu_page.canvas = nullptr;
    return false;
  }

  s_imu_page.canvas_buffer = static_cast<uint8_t *>(buffer);
  lv_canvas_set_buffer(s_imu_page.canvas, buffer, VIEW_WIDTH, VIEW_HEIGHT,
                       format);
  lv_draw_buf_t *draw_buffer = lv_canvas_get_draw_buf(s_imu_page.canvas);
  s_imu_page.canvas_stride = draw_buffer ? draw_buffer->header.stride : stride;
  lv_obj_add_event_cb(s_imu_page.canvas, canvas_buffer_delete_event_cb,
                      LV_EVENT_DELETE, buffer);
  render_device();
  return true;
}

void create_loading_overlay(lv_obj_t *parent) {
  s_imu_page.loading_container = lv_obj_create(parent);
  lv_obj_remove_flag(s_imu_page.loading_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_imu_page.loading_container, VIEW_WIDTH, VIEW_HEIGHT);
  lv_obj_set_pos(s_imu_page.loading_container, 0, 0);
  lv_obj_set_style_radius(s_imu_page.loading_container, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_imu_page.loading_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_imu_page.loading_container, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_imu_page.loading_container,
                            lv_color_hex(0xF8FAFC), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_imu_page.loading_container, LV_OPA_COVER,
                          LV_PART_MAIN);

  s_imu_page.loading_label = lv_label_create(s_imu_page.loading_container);
  lv_label_set_text(s_imu_page.loading_label, "Initializing IMU...");
  lv_obj_set_width(s_imu_page.loading_label, 390);
  lv_label_set_long_mode(s_imu_page.loading_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(s_imu_page.loading_label, &lv_font_montserrat_20,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(s_imu_page.loading_label,
                              lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
  lv_obj_set_style_text_align(s_imu_page.loading_label, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_center(s_imu_page.loading_label);
}

void show_imu_unavailable_text(esp_err_t error, int remaining_ms) {
  if (!s_imu_page.loading_container || !s_imu_page.loading_label) {
    return;
  }
  char message[160] = {};
  const int remaining_seconds = (remaining_ms + 999) / 1000;
  std::snprintf(message, sizeof(message),
                "IMU unavailable\nInit failed: %s\nReturning in %ds",
                esp_err_to_name(error),
                remaining_seconds > 0 ? remaining_seconds : 0);
  lv_obj_clear_flag(s_imu_page.loading_container, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(s_imu_page.loading_label, message);
  lv_obj_set_style_text_color(s_imu_page.loading_label,
                              lv_color_hex(COLOR_ERROR), LV_PART_MAIN);
  lv_obj_center(s_imu_page.loading_label);
}

void update_imu_failure_countdown(esp_err_t error) {
  if (!s_imu_page.init_failure_countdown_active) {
    s_imu_page.init_failure_countdown_active = true;
    s_imu_page.init_failure_close_ms = IMU_INIT_FAILURE_CLOSE_MS;
  }
  show_imu_unavailable_text(error, s_imu_page.init_failure_close_ms);
  s_imu_page.init_failure_close_ms -= IMU_UPDATE_PERIOD_MS;
  if (s_imu_page.init_failure_close_ms <= 0 && s_imu_page.window_backdrop) {
    s_imu_page.page_alive = false;
    lv_obj_delete_async(s_imu_page.window_backdrop);
  }
}

void show_calibration_status() {
  if (!s_imu_page.loading_container || !s_imu_page.loading_label ||
      s_imu_page.calibration_status_visible) {
    return;
  }
  s_imu_page.calibration_status_visible = true;
  lv_label_set_text(s_imu_page.loading_label, "Calibrating IMU...");
  lv_obj_set_style_text_color(s_imu_page.loading_label,
                              lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
  lv_obj_center(s_imu_page.loading_label);
}

void reveal_canvas() {
  if (s_imu_page.loading_container) {
    lv_obj_add_flag(s_imu_page.loading_container, LV_OBJ_FLAG_HIDDEN);
  }
}

void create_touch_markers(lv_obj_t *parent) {
  for (size_t i = 0; i < 2; ++i) {
    lv_obj_t *marker = lv_obj_create(parent);
    s_imu_page.touch_markers[i] = marker;
    lv_obj_remove_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(marker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(marker, TOUCH_MARKER_SIZE, TOUCH_MARKER_SIZE);
    lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(marker, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(marker, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        marker, lv_color_hex(TOUCH_MARKER_COLORS[i]), LV_PART_MAIN);
    lv_obj_set_style_border_opa(marker, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_pad_all(marker, 0, LV_PART_MAIN);
    lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
  }
}

void hide_touch_markers() {
  for (lv_obj_t *marker : s_imu_page.touch_markers) {
    if (marker) {
      lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void position_touch_marker(size_t index, int32_t screen_x, int32_t screen_y,
                           const lv_area_t &canvas_area) {
  lv_obj_t *marker = s_imu_page.touch_markers[index];
  if (!marker) {
    return;
  }
  lv_obj_set_pos(marker, screen_x - canvas_area.x1 - TOUCH_MARKER_SIZE / 2,
                 screen_y - canvas_area.y1 - TOUCH_MARKER_SIZE / 2);
  lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(marker);
}

bool point_in_area(const esp_lcd_touch_point_data_t &point,
                   const lv_area_t &area) {
  return point.x >= area.x1 && point.x <= area.x2 && point.y >= area.y1 &&
         point.y <= area.y2;
}

float normalize_twist_delta(float delta) {
  constexpr float PI = 3.14159265358979323846f;
  constexpr float HALF_PI = PI * 0.5f;
  while (delta > HALF_PI) {
    delta -= PI;
  }
  while (delta < -HALF_PI) {
    delta += PI;
  }
  return delta;
}

void apply_view_rotation(CoreP4ImuQuaternion delta) {
  s_imu_page.view_offset = corep4_imu_quaternion_normalize(
      corep4_imu_quaternion_multiply(delta, s_imu_page.view_offset));
  ++s_imu_page.view_sequence;
}

void update_touch_gesture() {
  auto *tool = corep4_ui_sync_get_tool();
  esp_lcd_touch_handle_t touch = tool ? tool->get_touch_handle() : nullptr;
  if (!touch || !s_imu_page.canvas || !s_imu_page.pose_ready) {
    s_imu_page.touch_mode = TouchGestureMode::None;
    hide_touch_markers();
    return;
  }

  esp_lcd_touch_point_data_t points[2] = {};
  uint8_t point_count = 0;
  if (esp_lcd_touch_get_data(touch, points, &point_count, 2) != ESP_OK) {
    point_count = 0;
  }

  lv_area_t canvas_area = {};
  lv_obj_get_coords(s_imu_page.canvas, &canvas_area);
  if (point_count == 0 || !point_in_area(points[0], canvas_area) ||
      (point_count > 1 && !point_in_area(points[1], canvas_area))) {
    s_imu_page.touch_mode = TouchGestureMode::None;
    hide_touch_markers();
    return;
  }

  position_touch_marker(0, points[0].x, points[0].y, canvas_area);
  if (point_count == 1) {
    if (s_imu_page.touch_markers[1]) {
      lv_obj_add_flag(s_imu_page.touch_markers[1], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_imu_page.touch_mode == TouchGestureMode::Drag) {
      const float dx = static_cast<float>(points[0].x - s_imu_page.last_touch_x);
      const float dy = static_cast<float>(points[0].y - s_imu_page.last_touch_y);
      if (dx != 0.0f || dy != 0.0f) {
        apply_view_rotation(corep4_imu_quaternion_from_euler(
            -dy * TOUCH_DRAG_RADIANS_PER_PIXEL,
            -dx * TOUCH_DRAG_RADIANS_PER_PIXEL, 0.0f));
      }
    }
    s_imu_page.touch_mode = TouchGestureMode::Drag;
    s_imu_page.last_touch_x = points[0].x;
    s_imu_page.last_touch_y = points[0].y;
    return;
  }

  position_touch_marker(1, points[1].x, points[1].y, canvas_area);
  const float twist_angle =
      std::atan2(static_cast<float>(points[1].y - points[0].y),
                 static_cast<float>(points[1].x - points[0].x));
  if (s_imu_page.touch_mode == TouchGestureMode::Twist) {
    const float delta =
        normalize_twist_delta(twist_angle - s_imu_page.last_twist_angle);
    if (std::fabs(delta) < 0.35f) {
      apply_view_rotation(corep4_imu_quaternion_from_euler(0.0f, 0.0f, -delta));
    }
  }
  s_imu_page.touch_mode = TouchGestureMode::Twist;
  s_imu_page.last_twist_angle = twist_angle;
}

void publish_pose(CoreP4ImuQuaternion relative) {
  portENTER_CRITICAL(&s_pose_lock);
  s_imu_page.latest_relative = relative;
  ++s_imu_page.pose_sequence;
  s_imu_page.pose_ready = true;
  portEXIT_CRITICAL(&s_pose_lock);
}

bool take_recenter_request() {
  portENTER_CRITICAL(&s_pose_lock);
  const bool requested = s_imu_page.recenter_requested;
  s_imu_page.recenter_requested = false;
  portEXIT_CRITICAL(&s_pose_lock);
  return requested;
}

void imu_fusion_task(void *parameter) {
  const uint32_t generation =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(parameter));
  auto *tool = corep4_ui_sync_get_tool();
  CoreP4ImuAttitude attitude;
  CoreP4ImuQuaternion reference = {};
  bool reference_valid = false;
  uint32_t last_sensor_time = 0;
  bool last_sensor_time_valid = false;
  float acc_x = 0.0f;
  float acc_y = 0.0f;
  float acc_z = 1.0f;
  float gyr_x = 0.0f;
  float gyr_y = 0.0f;
  float gyr_z = 0.0f;
  bool acc_valid = false;
  bool gyr_valid = false;
  attitude.reset();

  while (tool && s_imu_page.page_alive &&
         generation == s_imu_page.generation &&
         !s_imu_page.fusion_stop_requested) {
    bmi270_tools::sensor_data_t data = {};
    const esp_err_t result = tool->imu_read_sensor_data(data, false);
    if (result == ESP_OK && (data.acc_valid || data.gyr_valid) &&
        (!last_sensor_time_valid || data.sensor_time != last_sensor_time)) {
      float delta_seconds = IMU_SAMPLE_PERIOD_MS / 1000.0f;
      if (last_sensor_time_valid) {
        const uint32_t elapsed_ms = data.sensor_time - last_sensor_time;
        if (elapsed_ms > 0 && elapsed_ms <= 100) {
          delta_seconds = elapsed_ms / 1000.0f;
        }
      }
      last_sensor_time = data.sensor_time;
      last_sensor_time_valid = true;

      if (data.acc_valid) {
        acc_x = data.acc_x;
        acc_y = data.acc_y;
        acc_z = data.acc_z;
        acc_valid = true;
      }
      if (data.gyr_valid) {
        gyr_x = data.gyr_x;
        gyr_y = data.gyr_y;
        gyr_z = data.gyr_z;
        gyr_valid = true;
      }

      // The BMI270 screen Y axis points down, while the model Y axis points up.
      // Angular velocity is a pseudovector, so X and Z also change sign.
      if (attitude.update(acc_x, -acc_y, acc_z, data.acc_valid && acc_valid,
                          -gyr_x, gyr_y, -gyr_z,
                          data.gyr_valid && gyr_valid, delta_seconds)) {
        if (take_recenter_request() && acc_valid &&
            attitude.recenter_from_acceleration(acc_x, -acc_y, acc_z)) {
          reference = attitude.orientation();
          reference_valid = true;
          publish_pose({});
        } else if (!reference_valid && attitude.calibrated()) {
          attitude.clear_correction();
          reference = attitude.orientation();
          reference_valid = true;
          publish_pose({});
        } else if (reference_valid) {
          const CoreP4ImuQuaternion relative =
              corep4_imu_quaternion_normalize(corep4_imu_quaternion_multiply(
                  corep4_imu_quaternion_conjugate(reference),
                  attitude.orientation()));
          publish_pose(relative);
        }
      }
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IMU_SAMPLE_PERIOD_MS));
  }

  if (generation == s_imu_page.generation) {
    s_imu_page.fusion_task_handle = nullptr;
  }
  vTaskDelete(nullptr);
}

esp_err_t start_fusion_task(uint32_t generation) {
  if (s_imu_page.fusion_task_handle) {
    return ESP_OK;
  }
  s_imu_page.fusion_stop_requested = false;
  const BaseType_t task_result = xTaskCreate(
      imu_fusion_task, "imu_fusion", 4096,
      reinterpret_cast<void *>(static_cast<uintptr_t>(generation)),
      configMAX_PRIORITIES - 4, &s_imu_page.fusion_task_handle);
  if (task_result != pdPASS) {
    s_imu_page.fusion_task_handle = nullptr;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void stop_fusion_task() {
  TaskHandle_t task = s_imu_page.fusion_task_handle;
  if (!task) {
    return;
  }
  s_imu_page.fusion_stop_requested = true;
  xTaskNotifyGive(task);
  const TickType_t start_ticks = xTaskGetTickCount();
  while (s_imu_page.fusion_task_handle &&
         xTaskGetTickCount() - start_ticks < pdMS_TO_TICKS(250)) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (s_imu_page.fusion_task_handle) {
    ESP_LOGW(TAG, "IMU fusion task did not exit in time, deleting it");
    vTaskDelete(task);
    s_imu_page.fusion_task_handle = nullptr;
  }
}

void imu_timer_cb(lv_timer_t *) {
  if (!s_imu_page.page_alive) {
    return;
  }
  auto *tool = corep4_ui_sync_get_tool();
  if (!tool) {
    update_imu_failure_countdown(ESP_ERR_INVALID_STATE);
    return;
  }
  if (s_imu_page.task_running) {
    return;
  }
  if (s_imu_page.init_failed) {
    update_imu_failure_countdown(s_imu_page.init_result);
    return;
  }
  if (!tool->imu_is_initialized()) {
    return;
  }

  s_imu_page.init_failed = false;
  s_imu_page.init_failure_countdown_active = false;
  s_imu_page.init_failure_close_ms = 0;
  show_calibration_status();
  update_touch_gesture();

  CoreP4ImuQuaternion relative = {};
  uint32_t sequence = 0;
  bool ready = false;
  portENTER_CRITICAL(&s_pose_lock);
  relative = s_imu_page.latest_relative;
  sequence = s_imu_page.pose_sequence;
  ready = s_imu_page.pose_ready;
  portEXIT_CRITICAL(&s_pose_lock);
  if (!ready) {
    return;
  }
  reveal_canvas();
  if (sequence == s_imu_page.rendered_pose_sequence &&
      s_imu_page.view_sequence == s_imu_page.rendered_view_sequence) {
    return;
  }
  s_imu_page.rendered_pose_sequence = sequence;
  s_imu_page.rendered_view_sequence = s_imu_page.view_sequence;
  const CoreP4ImuQuaternion displayed = corep4_imu_quaternion_normalize(
      corep4_imu_quaternion_multiply(s_imu_page.view_offset, relative));
  render_device(displayed);
}

void imu_init_task(void *parameter) {
  auto *context = static_cast<ImuInitTaskContext *>(parameter);
  const uint32_t generation = context ? context->generation : 0;
  delete context;

  auto *tool = corep4_ui_sync_get_tool();
  const esp_err_t result = tool ? tool->imu_init(false, bmi270_tools::MODE_BASE)
                                : ESP_ERR_INVALID_STATE;
  esp_err_t start_result = result;
  if (result == ESP_OK && tool &&
      (!s_imu_page.page_alive || generation != s_imu_page.generation)) {
    tool->imu_stop_service();
  } else if (result == ESP_OK && tool) {
    tool->imu_stop_service();
    start_result = start_fusion_task(generation);
  }

  if (generation == s_imu_page.generation) {
    s_imu_page.init_result = start_result;
    s_imu_page.init_failed = start_result != ESP_OK;
    s_imu_page.task_running = false;
  }
  ESP_LOGI(TAG, "IMU init task completed: %s", esp_err_to_name(start_result));
  vTaskDelete(nullptr);
}

void recenter_event_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
    return;
  }
  s_imu_page.view_offset = {};
  ++s_imu_page.view_sequence;
  s_imu_page.touch_mode = TouchGestureMode::None;
  portENTER_CRITICAL(&s_pose_lock);
  s_imu_page.recenter_requested = true;
  portEXIT_CRITICAL(&s_pose_lock);
}

void imu_content_delete_event_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_DELETE) {
    return;
  }
  s_imu_page.page_alive = false;
  if (s_imu_page.timer) {
    lv_timer_delete(s_imu_page.timer);
    s_imu_page.timer = nullptr;
  }
  stop_fusion_task();
  auto *tool = corep4_ui_sync_get_tool();
  if (tool) {
    tool->imu_stop_service();
  }
  s_imu_page = {};
}

void start_imu(lv_obj_t *parent, const CoreP4FunctionWindowConfig &config) {
  auto *tool = corep4_ui_sync_get_tool();
  if (tool) {
    tool->set_status(StatusParam::POWER_PYB_L3B, true);
    if (tool->imu_is_initialized()) {
      s_imu_page.init_result = ESP_OK;
      tool->imu_stop_service();
      const esp_err_t start_result = start_fusion_task(s_imu_page.generation);
      if (start_result != ESP_OK) {
        s_imu_page.init_result = start_result;
        s_imu_page.init_failed = true;
        ESP_LOGW(TAG, "Failed to start IMU fusion task: %s",
                 esp_err_to_name(start_result));
      }
      return;
    }

    s_imu_page.task_running = true;
    auto *context = new (std::nothrow) ImuInitTaskContext{
        s_imu_page.generation,
    };
    if (context) {
      const BaseType_t task_result =
          xTaskCreate(imu_init_task, "func_imu_init", 4096, context,
                      configMAX_PRIORITIES - 4, nullptr);
      if (task_result == pdPASS) {
        return;
      }
      delete context;
    }
    s_imu_page.task_running = false;
    s_imu_page.init_result = ESP_ERR_NO_MEM;
    s_imu_page.init_failed = true;
    ESP_LOGE(TAG, "Failed to create IMU init task");
    return;
  }

  (void)parent;
  (void)config;
  s_imu_page.init_result = ESP_ERR_INVALID_STATE;
  s_imu_page.init_failed = true;
}

void build_content(lv_obj_t *parent, const CoreP4FunctionWindowConfig &config) {
  s_imu_page = {};
  uint32_t generation = static_cast<uint32_t>(s_imu_page_generation) + 1;
  if (generation == 0) {
    generation = 1;
  }
  s_imu_page_generation = generation;
  s_imu_page.generation = generation;
  s_imu_page.page_alive = true;
  s_imu_page.window_backdrop = lv_obj_get_parent(parent);

  prepare_content(parent);
  lv_obj_set_layout(parent, LV_LAYOUT_NONE);
  lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(parent, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(parent, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(parent, imu_content_delete_event_cb, LV_EVENT_DELETE,
                      nullptr);

  if (!create_canvas(parent)) {
    s_imu_page.init_result = ESP_ERR_NO_MEM;
    s_imu_page.init_failed = true;
  }
  create_loading_overlay(parent);
  create_touch_markers(parent);
  start_imu(parent, config);
  s_imu_page.timer =
      lv_timer_create(imu_timer_cb, IMU_UPDATE_PERIOD_MS, nullptr);
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "IMU",
    "click on ui_func1imu1con",
    build_content,
    LV_SYMBOL_REFRESH,
    recenter_event_cb,
    nullptr,
    COLOR_BUTTON_BG,
    COLOR_TEXT,
};

} // namespace

extern "C" void func2imu2clicked2func(lv_event_t *event) {
  corep4_ui_open_function_window(event, WINDOW_CONFIG);
}
