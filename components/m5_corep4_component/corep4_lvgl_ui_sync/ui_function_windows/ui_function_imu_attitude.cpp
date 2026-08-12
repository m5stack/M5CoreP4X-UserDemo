#include "ui_function_imu_attitude.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float PI = 3.14159265358979323846f;
constexpr float DEG_TO_RAD = PI / 180.0f;
constexpr float MAHONY_KP = 2.8f;
constexpr float MAHONY_KI = 0.06f;
constexpr float MIN_VECTOR_NORM = 1.0e-5f;
constexpr float MAX_DELTA_SECONDS = 0.1f;
constexpr float MIN_DELTA_SECONDS = 0.001f;
constexpr float BOOTSTRAP_STATIONARY_GYRO_DPS = 1.5f;
constexpr float TRACKING_STATIONARY_GYRO_DPS = 0.25f;
constexpr float STATIONARY_ACCEL_FRACTION = 0.10f;
constexpr unsigned GYRO_BIAS_BOOTSTRAP_SAMPLES = 12;
constexpr float GYRO_BIAS_ALPHA = 0.003f;
constexpr float INTEGRAL_ERROR_LIMIT = 0.08f;
constexpr float INTEGRAL_ERROR_DECAY = 0.98f;

float clamp_f(float value, float minimum, float maximum) {
  return std::max(minimum, std::min(value, maximum));
}

} // namespace

CoreP4ImuQuaternion corep4_imu_quaternion_normalize(CoreP4ImuQuaternion value) {
  const float norm = std::sqrt(value.w * value.w + value.x * value.x +
                               value.y * value.y + value.z * value.z);
  if (norm < MIN_VECTOR_NORM) {
    return {};
  }
  const float inverse = 1.0f / norm;
  value.w *= inverse;
  value.x *= inverse;
  value.y *= inverse;
  value.z *= inverse;
  return value;
}

CoreP4ImuQuaternion corep4_imu_quaternion_conjugate(CoreP4ImuQuaternion value) {
  value.x = -value.x;
  value.y = -value.y;
  value.z = -value.z;
  return value;
}

CoreP4ImuQuaternion corep4_imu_quaternion_multiply(CoreP4ImuQuaternion lhs,
                                                   CoreP4ImuQuaternion rhs) {
  return {
      lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
      lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
      lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
      lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
  };
}

CoreP4ImuQuaternion corep4_imu_quaternion_from_euler(float roll_radians,
                                                     float pitch_radians,
                                                     float yaw_radians) {
  const float cr = std::cos(roll_radians * 0.5f);
  const float sr = std::sin(roll_radians * 0.5f);
  const float cp = std::cos(pitch_radians * 0.5f);
  const float sp = std::sin(pitch_radians * 0.5f);
  const float cy = std::cos(yaw_radians * 0.5f);
  const float sy = std::sin(yaw_radians * 0.5f);
  return corep4_imu_quaternion_normalize({
      cr * cp * cy + sr * sp * sy,
      sr * cp * cy - cr * sp * sy,
      cr * sp * cy + sr * cp * sy,
      cr * cp * sy - sr * sp * cy,
  });
}

void CoreP4ImuAttitude::reset() {
  orientation_ = {};
  clear_correction();
  gyro_bias_dps_[0] = 0.0f;
  gyro_bias_dps_[1] = 0.0f;
  gyro_bias_dps_[2] = 0.0f;
  gravity_norm_ = 0.0f;
  gyro_bias_samples_ = 0;
  ready_ = false;
}

void CoreP4ImuAttitude::clear_correction() {
  integral_error_[0] = 0.0f;
  integral_error_[1] = 0.0f;
  integral_error_[2] = 0.0f;
}

bool CoreP4ImuAttitude::recenter_from_acceleration(float acc_x, float acc_y,
                                                   float acc_z) {
  const float norm = std::sqrt(acc_x * acc_x + acc_y * acc_y + acc_z * acc_z);
  if (norm < MIN_VECTOR_NORM) {
    return false;
  }
  const float roll = std::atan2(acc_y, acc_z);
  const float pitch =
      std::atan2(-acc_x, std::sqrt(acc_y * acc_y + acc_z * acc_z));
  orientation_ = corep4_imu_quaternion_from_euler(roll, pitch, 0.0f);
  gravity_norm_ = norm;
  clear_correction();
  ready_ = true;
  return true;
}

bool CoreP4ImuAttitude::calibrated() const {
  return gyro_bias_samples_ >= GYRO_BIAS_BOOTSTRAP_SAMPLES;
}

bool CoreP4ImuAttitude::update(float acc_x, float acc_y, float acc_z,
                               bool acceleration_valid, float gyr_x_dps,
                               float gyr_y_dps, float gyr_z_dps,
                               bool gyroscope_valid, float delta_seconds) {
  const float acc_norm =
      acceleration_valid
          ? std::sqrt(acc_x * acc_x + acc_y * acc_y + acc_z * acc_z)
          : 0.0f;
  acceleration_valid = acceleration_valid && acc_norm > MIN_VECTOR_NORM;

  if (!ready_) {
    if (!acceleration_valid ||
        !recenter_from_acceleration(acc_x, acc_y, acc_z)) {
      return false;
    }
  }

  if (!gyroscope_valid) {
    return ready_;
  }

  const float dt = clamp_f(delta_seconds, MIN_DELTA_SECONDS, MAX_DELTA_SECONDS);
  if (acceleration_valid) {
    gravity_norm_ += (acc_norm - gravity_norm_) * 0.02f;
  }

  const float raw_gyro_norm = std::sqrt(
      gyr_x_dps * gyr_x_dps + gyr_y_dps * gyr_y_dps + gyr_z_dps * gyr_z_dps);
  const float corrected_gyr_x = gyr_x_dps - gyro_bias_dps_[0];
  const float corrected_gyr_y = gyr_y_dps - gyro_bias_dps_[1];
  const float corrected_gyr_z = gyr_z_dps - gyro_bias_dps_[2];
  const float corrected_gyro_norm =
      std::sqrt(corrected_gyr_x * corrected_gyr_x +
                corrected_gyr_y * corrected_gyr_y +
                corrected_gyr_z * corrected_gyr_z);
  const bool acceleration_stationary =
      acceleration_valid &&
      std::fabs(acc_norm - gravity_norm_) <=
          std::max(gravity_norm_ * STATIONARY_ACCEL_FRACTION, 0.01f);
  const bool bootstrap_stationary =
      acceleration_stationary &&
      raw_gyro_norm < BOOTSTRAP_STATIONARY_GYRO_DPS;
  const bool tracking_stationary =
      acceleration_stationary &&
      corrected_gyro_norm < TRACKING_STATIONARY_GYRO_DPS;
  if (!calibrated()) {
    if (bootstrap_stationary) {
      ++gyro_bias_samples_;
      const float sample_weight = 1.0f / gyro_bias_samples_;
      gyro_bias_dps_[0] += (gyr_x_dps - gyro_bias_dps_[0]) * sample_weight;
      gyro_bias_dps_[1] += (gyr_y_dps - gyro_bias_dps_[1]) * sample_weight;
      gyro_bias_dps_[2] += (gyr_z_dps - gyro_bias_dps_[2]) * sample_weight;
    } else {
      gyro_bias_samples_ = 0;
      gyro_bias_dps_[0] = 0.0f;
      gyro_bias_dps_[1] = 0.0f;
      gyro_bias_dps_[2] = 0.0f;
    }
  } else if (tracking_stationary) {
    gyro_bias_dps_[0] += (gyr_x_dps - gyro_bias_dps_[0]) * GYRO_BIAS_ALPHA;
    gyro_bias_dps_[1] += (gyr_y_dps - gyro_bias_dps_[1]) * GYRO_BIAS_ALPHA;
    gyro_bias_dps_[2] += (gyr_z_dps - gyro_bias_dps_[2]) * GYRO_BIAS_ALPHA;
  }

  float gx = (gyr_x_dps - gyro_bias_dps_[0]) * DEG_TO_RAD;
  float gy = (gyr_y_dps - gyro_bias_dps_[1]) * DEG_TO_RAD;
  float gz = (gyr_z_dps - gyro_bias_dps_[2]) * DEG_TO_RAD;

  if (acceleration_valid &&
      std::fabs(acc_norm - gravity_norm_) <= gravity_norm_ * 0.20f) {
    const float inverse_acc = 1.0f / acc_norm;
    const float ax = acc_x * inverse_acc;
    const float ay = acc_y * inverse_acc;
    const float az = acc_z * inverse_acc;
    const CoreP4ImuQuaternion &q = orientation_;

    const float vx = 2.0f * (q.x * q.z - q.w * q.y);
    const float vy = 2.0f * (q.w * q.x + q.y * q.z);
    const float vz = q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z;
    const float error_x = ay * vz - az * vy;
    const float error_y = az * vx - ax * vz;
    const float error_z = ax * vy - ay * vx;

    integral_error_[0] = clamp_f(integral_error_[0] + MAHONY_KI * error_x * dt,
                                 -INTEGRAL_ERROR_LIMIT, INTEGRAL_ERROR_LIMIT);
    integral_error_[1] = clamp_f(integral_error_[1] + MAHONY_KI * error_y * dt,
                                 -INTEGRAL_ERROR_LIMIT, INTEGRAL_ERROR_LIMIT);
    integral_error_[2] = clamp_f(integral_error_[2] + MAHONY_KI * error_z * dt,
                                 -INTEGRAL_ERROR_LIMIT, INTEGRAL_ERROR_LIMIT);
    gx += MAHONY_KP * error_x + integral_error_[0];
    gy += MAHONY_KP * error_y + integral_error_[1];
    gz += MAHONY_KP * error_z + integral_error_[2];
  } else {
    integral_error_[0] *= INTEGRAL_ERROR_DECAY;
    integral_error_[1] *= INTEGRAL_ERROR_DECAY;
    integral_error_[2] *= INTEGRAL_ERROR_DECAY;
  }

  const CoreP4ImuQuaternion q = orientation_;
  const float half_dt = 0.5f * dt;
  orientation_.w += (-q.x * gx - q.y * gy - q.z * gz) * half_dt;
  orientation_.x += (q.w * gx + q.y * gz - q.z * gy) * half_dt;
  orientation_.y += (q.w * gy - q.x * gz + q.z * gx) * half_dt;
  orientation_.z += (q.w * gz + q.x * gy - q.y * gx) * half_dt;
  orientation_ = corep4_imu_quaternion_normalize(orientation_);
  return true;
}
