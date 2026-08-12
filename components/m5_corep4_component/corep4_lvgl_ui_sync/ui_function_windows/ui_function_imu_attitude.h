#pragma once

struct CoreP4ImuQuaternion {
  float w = 1.0f;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

CoreP4ImuQuaternion corep4_imu_quaternion_normalize(CoreP4ImuQuaternion value);
CoreP4ImuQuaternion corep4_imu_quaternion_conjugate(CoreP4ImuQuaternion value);
CoreP4ImuQuaternion corep4_imu_quaternion_multiply(CoreP4ImuQuaternion lhs,
                                                   CoreP4ImuQuaternion rhs);
CoreP4ImuQuaternion corep4_imu_quaternion_from_euler(float roll_radians,
                                                     float pitch_radians,
                                                     float yaw_radians);

class CoreP4ImuAttitude {
public:
  void reset();
  void clear_correction();
  bool recenter_from_acceleration(float acc_x, float acc_y, float acc_z);

  bool update(float acc_x, float acc_y, float acc_z, bool acceleration_valid,
              float gyr_x_dps, float gyr_y_dps, float gyr_z_dps,
              bool gyroscope_valid, float delta_seconds);

  bool ready() const { return ready_; }
  bool calibrated() const;
  CoreP4ImuQuaternion orientation() const { return orientation_; }

private:
  CoreP4ImuQuaternion orientation_ = {};
  float integral_error_[3] = {};
  float gyro_bias_dps_[3] = {};
  float gravity_norm_ = 0.0f;
  unsigned gyro_bias_samples_ = 0;
  bool ready_ = false;
};
