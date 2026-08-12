#include "ui_function_imu_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

extern "C" {
extern const uint8_t texture_front[] asm("_binary_corep4x_front_rgb565_start");
extern const uint8_t texture_back[] asm("_binary_corep4x_back_rgb565_start");
extern const uint8_t texture_top[] asm("_binary_corep4x_top_rgb565_start");
extern const uint8_t
    texture_bottom[] asm("_binary_corep4x_bottom_rgb565_start");
extern const uint8_t texture_left[] asm("_binary_corep4x_left_rgb565_start");
extern const uint8_t texture_right[] asm("_binary_corep4x_right_rgb565_start");
}

constexpr float PI = 3.14159265358979323846f;
constexpr float DEVICE_HALF_WIDTH = 27.0f;
constexpr float DEVICE_HALF_DEPTH = 11.8f;
constexpr float DEVICE_CORNER_RADIUS = 5.2f;
constexpr int CORNER_SEGMENTS = 4;
constexpr int PERIMETER_POINTS = 4 * (CORNER_SEGMENTS + 1);
constexpr int MAX_RENDER_TRIANGLES = PERIMETER_POINTS * 4;
constexpr float CAMERA_DISTANCE = 108.0f;
constexpr float FOCAL_LENGTH = 575.0f;
constexpr uint16_t BEVEL_COLOR = 0xBDF7;

struct Vec3 {
  float x;
  float y;
  float z;
};

struct Texture {
  const uint8_t *data;
  int width;
  int height;
};

enum class SideFace : uint8_t {
  Top,
  Bottom,
  Left,
  Right,
};

struct ModelVertex {
  Vec3 position;
  float u;
  float v;
};

struct ScreenVertex {
  float x;
  float y;
  float inverse_depth;
  float u_over_depth;
  float v_over_depth;
};

struct RenderTriangle {
  ScreenVertex vertices[3];
  const Texture *texture;
  uint16_t solid_color;
  float shade;
  float depth;
};

constexpr Texture FRONT_TEXTURE = {texture_front, 256, 256};
constexpr Texture BACK_TEXTURE = {texture_back, 256, 256};
constexpr Texture TOP_TEXTURE = {texture_top, 256, 112};
constexpr Texture BOTTOM_TEXTURE = {texture_bottom, 256, 112};
constexpr Texture LEFT_TEXTURE = {texture_left, 256, 112};
constexpr Texture RIGHT_TEXTURE = {texture_right, 256, 112};

const Texture *side_texture(SideFace face) {
  switch (face) {
  case SideFace::Top:
    return &TOP_TEXTURE;
  case SideFace::Bottom:
    return &BOTTOM_TEXTURE;
  case SideFace::Left:
    return &LEFT_TEXTURE;
  case SideFace::Right:
    return &RIGHT_TEXTURE;
  }
  return nullptr;
}

float side_texture_u(SideFace face, float perimeter_distance,
                     float side_start, float side_length,
                     float perimeter_length) {
  switch (face) {
  case SideFace::Top:
    return (side_start + side_length - perimeter_distance) / side_length;
  case SideFace::Left:
    return (perimeter_distance - side_start - side_length) / side_length;
  case SideFace::Bottom:
    return (perimeter_distance - side_start - 2.0f * side_length) /
           side_length;
  case SideFace::Right:
    if (perimeter_distance <= side_start) {
      perimeter_distance += perimeter_length;
    }
    return (side_start + 4.0f * side_length - perimeter_distance) /
           side_length;
  }
  return 0.0f;
}

Vec3 subtract(Vec3 lhs, Vec3 rhs) {
  return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Vec3 cross(Vec3 lhs, Vec3 rhs) {
  return {
      lhs.y * rhs.z - lhs.z * rhs.y,
      lhs.z * rhs.x - lhs.x * rhs.z,
      lhs.x * rhs.y - lhs.y * rhs.x,
  };
}

float dot(Vec3 lhs, Vec3 rhs) {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Vec3 normalized(Vec3 value) {
  const float length = std::sqrt(dot(value, value));
  if (length < 1.0e-6f) {
    return {0.0f, 0.0f, 1.0f};
  }
  const float inverse = 1.0f / length;
  return {value.x * inverse, value.y * inverse, value.z * inverse};
}

Vec3 rotate(CoreP4ImuQuaternion q, Vec3 value) {
  const Vec3 qv = {q.x, q.y, q.z};
  const Vec3 first = cross(qv, value);
  const Vec3 second = cross(qv, first);
  return {
      value.x + 2.0f * (q.w * first.x + second.x),
      value.y + 2.0f * (q.w * first.y + second.y),
      value.z + 2.0f * (q.w * first.z + second.z),
  };
}

uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return static_cast<uint16_t>(((red & 0xF8U) << 8U) | ((green & 0xFCU) << 3U) |
                               (blue >> 3U));
}

void write_pixel(uint8_t *buffer, uint32_t stride, int32_t x, int32_t y,
                 uint16_t color) {
  std::memcpy(buffer + static_cast<uint32_t>(y) * stride +
                  static_cast<uint32_t>(x) * 2U,
              &color, sizeof(color));
}

uint16_t read_pixel(const uint8_t *buffer, uint32_t stride, int32_t x,
                    int32_t y) {
  uint16_t color = 0;
  std::memcpy(&color,
              buffer + static_cast<uint32_t>(y) * stride +
                  static_cast<uint32_t>(x) * 2U,
              sizeof(color));
  return color;
}

uint16_t mix_rgb565(uint16_t source, uint16_t target, float amount) {
  amount = std::max(0.0f, std::min(amount, 1.0f));
  const int source_r = (source >> 11) & 0x1F;
  const int source_g = (source >> 5) & 0x3F;
  const int source_b = source & 0x1F;
  const int target_r = (target >> 11) & 0x1F;
  const int target_g = (target >> 5) & 0x3F;
  const int target_b = target & 0x1F;
  const int red = static_cast<int>(source_r + (target_r - source_r) * amount);
  const int green = static_cast<int>(source_g + (target_g - source_g) * amount);
  const int blue = static_cast<int>(source_b + (target_b - source_b) * amount);
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

uint16_t shade_rgb565(uint16_t color, float shade) {
  shade = std::max(0.0f, std::min(shade, 1.0f));
  const int red = static_cast<int>(((color >> 11) & 0x1F) * shade);
  const int green = static_cast<int>(((color >> 5) & 0x3F) * shade);
  const int blue = static_cast<int>((color & 0x1F) * shade);
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

void fill_background(uint8_t *buffer, uint32_t stride, int32_t width,
                     int32_t height) {
  const uint16_t top = rgb565(248, 250, 252);
  const uint16_t bottom = rgb565(224, 232, 236);
  for (int32_t y = 0; y < height; ++y) {
    const float amount =
        height > 1 ? static_cast<float>(y) / (height - 1) : 0.0f;
    const uint16_t color = mix_rgb565(top, bottom, amount);
    auto *row = reinterpret_cast<uint16_t *>(
        buffer + static_cast<uint32_t>(y) * stride);
    std::fill_n(row, width, color);
  }

  const float center_x = width * 0.50f;
  const float center_y = height * 0.73f;
  const float radius_x = width * 0.31f;
  const float radius_y = height * 0.085f;
  const uint16_t shadow = rgb565(64, 75, 82);
  const int32_t x0 =
      std::max<int32_t>(0, static_cast<int32_t>(center_x - radius_x));
  const int32_t x1 =
      std::min(width - 1, static_cast<int32_t>(center_x + radius_x));
  const int32_t y0 =
      std::max<int32_t>(0, static_cast<int32_t>(center_y - radius_y));
  const int32_t y1 =
      std::min(height - 1, static_cast<int32_t>(center_y + radius_y));
  for (int32_t y = y0; y <= y1; ++y) {
    for (int32_t x = x0; x <= x1; ++x) {
      const float dx = (x - center_x) / radius_x;
      const float dy = (y - center_y) / radius_y;
      const float distance = dx * dx + dy * dy;
      if (distance >= 1.0f) {
        continue;
      }
      const float alpha = (1.0f - distance) * (1.0f - distance) * 0.18f;
      write_pixel(buffer, stride, x, y,
                  mix_rgb565(read_pixel(buffer, stride, x, y), shadow, alpha));
    }
  }
}

float edge(const ScreenVertex &a, const ScreenVertex &b, float x, float y) {
  return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

uint16_t sample_texture(const Texture &texture, float u, float v) {
  u = std::max(0.0f, std::min(u, 0.99999f));
  v = std::max(0.0f, std::min(v, 0.99999f));
  const int x = static_cast<int>(u * texture.width);
  const int y = static_cast<int>(v * texture.height);
  uint16_t color = 0;
  const size_t offset = static_cast<size_t>(y * texture.width + x) * 2U;
  std::memcpy(&color, texture.data + offset, sizeof(color));
  return color;
}

void rasterize_triangle(uint8_t *buffer, uint32_t stride, int32_t width,
                        int32_t height, const RenderTriangle &triangle) {
  const ScreenVertex &a = triangle.vertices[0];
  const ScreenVertex &b = triangle.vertices[1];
  const ScreenVertex &c = triangle.vertices[2];
  const float area = edge(a, b, c.x, c.y);
  if (std::fabs(area) < 0.01f) {
    return;
  }

  const int32_t min_x = std::max<int32_t>(
      0, static_cast<int32_t>(std::floor(std::min(a.x, std::min(b.x, c.x)))));
  const int32_t max_x = std::min(
      width - 1,
      static_cast<int32_t>(std::ceil(std::max(a.x, std::max(b.x, c.x)))));
  const int32_t min_y = std::max<int32_t>(
      0, static_cast<int32_t>(std::floor(std::min(a.y, std::min(b.y, c.y)))));
  const int32_t max_y = std::min(
      height - 1,
      static_cast<int32_t>(std::ceil(std::max(a.y, std::max(b.y, c.y)))));

  for (int32_t y = min_y; y <= max_y; ++y) {
    for (int32_t x = min_x; x <= max_x; ++x) {
      const float px = x + 0.5f;
      const float py = y + 0.5f;
      const float weight_a = edge(b, c, px, py) / area;
      const float weight_b = edge(c, a, px, py) / area;
      const float weight_c = 1.0f - weight_a - weight_b;
      if (weight_a < -0.0001f || weight_b < -0.0001f || weight_c < -0.0001f) {
        continue;
      }

      uint16_t color = triangle.solid_color;
      if (triangle.texture) {
        const float inverse_depth = weight_a * a.inverse_depth +
                                    weight_b * b.inverse_depth +
                                    weight_c * c.inverse_depth;
        if (inverse_depth <= 0.0f) {
          continue;
        }
        const float u = (weight_a * a.u_over_depth + weight_b * b.u_over_depth +
                         weight_c * c.u_over_depth) /
                        inverse_depth;
        const float v = (weight_a * a.v_over_depth + weight_b * b.v_over_depth +
                         weight_c * c.v_over_depth) /
                        inverse_depth;
        color = sample_texture(*triangle.texture, u, v);
      }
      write_pixel(buffer, stride, x, y, shade_rgb565(color, triangle.shade));
    }
  }
}

class TriangleBuilder {
public:
  TriangleBuilder(std::array<RenderTriangle, MAX_RENDER_TRIANGLES> &triangles,
                  int32_t width, int32_t height, CoreP4ImuQuaternion rotation)
      : triangles_(triangles), width_(width), height_(height),
        rotation_(rotation) {}

  void add(ModelVertex a, ModelVertex b, ModelVertex c, const Texture *texture,
           uint16_t solid_color = BEVEL_COLOR) {
    if (count_ >= triangles_.size()) {
      return;
    }
    const Vec3 positions[3] = {
        rotate(rotation_, a.position),
        rotate(rotation_, b.position),
        rotate(rotation_, c.position),
    };
    const Vec3 normal_raw = cross(subtract(positions[1], positions[0]),
                                  subtract(positions[2], positions[0]));
    const Vec3 center = {
        (positions[0].x + positions[1].x + positions[2].x) / 3.0f,
        (positions[0].y + positions[1].y + positions[2].y) / 3.0f,
        (positions[0].z + positions[1].z + positions[2].z) / 3.0f,
    };
    if (dot(normal_raw, {-center.x, -center.y, CAMERA_DISTANCE - center.z}) <=
        0.0f) {
      return;
    }

    const Vec3 normal = normalized(normal_raw);
    const Vec3 light = normalized({-0.35f, 0.55f, 0.76f});
    RenderTriangle &output = triangles_[count_++];
    output.texture = texture;
    output.solid_color = solid_color;
    output.shade = 0.58f + 0.42f * std::max(0.0f, dot(normal, light));
    output.depth = center.z;

    const ModelVertex input[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
      const float depth = std::max(8.0f, CAMERA_DISTANCE - positions[i].z);
      const float inverse_depth = 1.0f / depth;
      output.vertices[i] = {
          width_ * 0.5f + FOCAL_LENGTH * positions[i].x * inverse_depth,
          height_ * 0.48f - FOCAL_LENGTH * positions[i].y * inverse_depth,
          inverse_depth,
          input[i].u * inverse_depth,
          input[i].v * inverse_depth,
      };
    }
  }

  size_t count() const { return count_; }

private:
  std::array<RenderTriangle, MAX_RENDER_TRIANGLES> &triangles_;
  int32_t width_;
  int32_t height_;
  CoreP4ImuQuaternion rotation_;
  size_t count_ = 0;
};

std::array<Vec3, PERIMETER_POINTS> rounded_perimeter(float z) {
  std::array<Vec3, PERIMETER_POINTS> points = {};
  size_t index = 0;
  for (int corner = 0; corner < 4; ++corner) {
    const float center_x = (corner == 0 || corner == 3)
                               ? DEVICE_HALF_WIDTH - DEVICE_CORNER_RADIUS
                               : -DEVICE_HALF_WIDTH + DEVICE_CORNER_RADIUS;
    const float center_y = (corner == 0 || corner == 1)
                               ? DEVICE_HALF_WIDTH - DEVICE_CORNER_RADIUS
                               : -DEVICE_HALF_WIDTH + DEVICE_CORNER_RADIUS;
    const float start_angle = corner * 90.0f;
    for (int segment = 0; segment <= CORNER_SEGMENTS; ++segment) {
      const float angle =
          (start_angle + 90.0f * segment / CORNER_SEGMENTS) * PI / 180.0f;
      points[index++] = {
          center_x + DEVICE_CORNER_RADIUS * std::cos(angle),
          center_y + DEVICE_CORNER_RADIUS * std::sin(angle),
          z,
      };
    }
  }
  return points;
}

void add_device_mesh(TriangleBuilder &builder) {
  const auto front = rounded_perimeter(DEVICE_HALF_DEPTH);
  const auto back = rounded_perimeter(-DEVICE_HALF_DEPTH);
  std::array<float, PERIMETER_POINTS> perimeter_distances = {};
  for (int i = 1; i < PERIMETER_POINTS; ++i) {
    const Vec3 segment = subtract(front[i], front[i - 1]);
    perimeter_distances[i] =
        perimeter_distances[i - 1] + std::sqrt(dot(segment, segment));
  }
  const Vec3 closing_segment = subtract(front[0], front.back());
  const float perimeter_length =
      perimeter_distances.back() + std::sqrt(dot(closing_segment, closing_segment));
  const float side_start = perimeter_distances[CORNER_SEGMENTS / 2];
  const float side_length = perimeter_length * 0.25f;
  const ModelVertex front_center = {
      {0.0f, 0.0f, DEVICE_HALF_DEPTH}, 0.5f, 0.5f};
  const ModelVertex back_center = {
      {0.0f, 0.0f, -DEVICE_HALF_DEPTH}, 0.5f, 0.5f};

  for (int i = 0; i < PERIMETER_POINTS; ++i) {
    const int next = (i + 1) % PERIMETER_POINTS;
    const auto front_vertex = [](Vec3 point) {
      return ModelVertex{
          point,
          (point.x + DEVICE_HALF_WIDTH) / (2.0f * DEVICE_HALF_WIDTH),
          (DEVICE_HALF_WIDTH - point.y) / (2.0f * DEVICE_HALF_WIDTH),
      };
    };
    const auto back_vertex = [](Vec3 point) {
      return ModelVertex{
          point,
          (DEVICE_HALF_WIDTH - point.x) / (2.0f * DEVICE_HALF_WIDTH),
          (DEVICE_HALF_WIDTH - point.y) / (2.0f * DEVICE_HALF_WIDTH),
      };
    };
    builder.add(front_center, front_vertex(front[i]), front_vertex(front[next]),
                &FRONT_TEXTURE);
    builder.add(back_center, back_vertex(back[next]), back_vertex(back[i]),
                &BACK_TEXTURE);

    const float dx = front[next].x - front[i].x;
    const float dy = front[next].y - front[i].y;
    const float length = std::sqrt(dx * dx + dy * dy);
    const float outward_x = length > 0.0f ? dy / length : 0.0f;
    const float outward_y = length > 0.0f ? -dx / length : 0.0f;
    SideFace face = SideFace::Right;
    if (std::fabs(outward_y) > std::fabs(outward_x)) {
      face = outward_y > 0.0f ? SideFace::Top : SideFace::Bottom;
    } else {
      face = outward_x > 0.0f ? SideFace::Right : SideFace::Left;
    }
    const Texture *texture = side_texture(face);
    const float distance_a = perimeter_distances[i];
    const float distance_b = next == 0 ? perimeter_length
                                       : perimeter_distances[next];
    const float u_a = side_texture_u(face, distance_a, side_start, side_length,
                                     perimeter_length);
    const float u_b = side_texture_u(face, distance_b, side_start, side_length,
                                     perimeter_length);

    const ModelVertex a_front = {front[i], u_a, 0.0f};
    const ModelVertex a_back = {back[i], u_a, 1.0f};
    const ModelVertex b_back = {back[next], u_b, 1.0f};
    const ModelVertex b_front = {front[next], u_b, 0.0f};
    builder.add(a_front, a_back, b_back, texture);
    builder.add(a_front, b_back, b_front, texture);
  }
}

} // namespace

void corep4_imu_render_device(uint8_t *rgb565_buffer, uint32_t stride_bytes,
                              int32_t width, int32_t height,
                              CoreP4ImuQuaternion relative_orientation) {
  if (!rgb565_buffer || stride_bytes < static_cast<uint32_t>(width * 2) ||
      width <= 0 || height <= 0) {
    return;
  }

  fill_background(rgb565_buffer, stride_bytes, width, height);
  const CoreP4ImuQuaternion rotation =
      corep4_imu_quaternion_normalize(relative_orientation);

  std::array<RenderTriangle, MAX_RENDER_TRIANGLES> triangles = {};
  TriangleBuilder builder(triangles, width, height, rotation);
  add_device_mesh(builder);
  std::sort(triangles.begin(), triangles.begin() + builder.count(),
            [](const RenderTriangle &lhs, const RenderTriangle &rhs) {
              return lhs.depth < rhs.depth;
            });
  for (size_t i = 0; i < builder.count(); ++i) {
    rasterize_triangle(rgb565_buffer, stride_bytes, width, height,
                       triangles[i]);
  }
}
