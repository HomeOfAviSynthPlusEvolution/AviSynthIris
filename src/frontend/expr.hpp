#pragma once
#include "iris/iris.h"
#include <string_view>
#include <initializer_list>

namespace iris {
// Expr range arithmetic, expressed independently of the AviSynth host.
struct ExprScaling {
  iris_scale_inputs mode;
  bool converts(uint32_t bits) const {
    return mode == IRIS_SCALE_ALL || mode == IRIS_SCALE_ALL_FULL ||
           (bits == 32 ? mode == IRIS_SCALE_FLOAT || mode == IRIS_SCALE_FLOAT_FULL
                       : mode == IRIS_SCALE_INT || mode == IRIS_SCALE_INT_FULL);
  }
  bool full() const {
    return mode == IRIS_SCALE_ALL_FULL || mode == IRIS_SCALE_INT_FULL || mode == IRIS_SCALE_FLOAT_FULL;
  }
  bool shift() const { return mode == IRIS_SCALE_FLOAT_UV; }
};
struct ExprRange {
  float origin, span;
};
inline ExprRange expr_range(uint32_t bits, bool chroma, bool full) {
  if (bits == 32) {
    if (chroma)
      return {0, full ? 0.5f : 112.0f / 255.0f};
    return {full ? 0.0f : 16.0f / 255.0f, full ? 1.0f : 219.0f / 255.0f};
  }
  float unit = float(1u << (bits - 8));
  float maximum = float((1u << bits) - 1);
  if (chroma)
    return {float(1u << (bits - 1)), full ? maximum / 2.0f : 112.0f * unit};
  return {full ? 0.0f : 16.0f * unit, full ? maximum : 219.0f * unit};
}
inline bool expr_constant_name(std::string_view name) {
  for (auto keyword : {"bitdepth", "ymin", "ymax", "cmin", "cmax", "range_size", "range_min", "range_max", "range_half",
                       "yrange_min", "yrange_max", "yrange_half"})
    if (name == keyword)
      return true;
  return false;
}
inline bool expr_reserved(std::string_view token) {
  auto suffix = token.size() > 2 && token[token.size() - 2] == '_' ? token.substr(0, token.size() - 2) : token;
  return expr_constant_name(suffix) || token == "time" || token == "sbitdepth" || token == "scaleb" ||
         token == "scalef" || token == "yscaleb" || token == "yscalef" || token == "i8" || token == "i10" ||
         token == "i12" || token == "i14" || token == "i16" || token == "f32";
}
inline float expr_constant(std::string_view name, uint32_t bits, bool chroma, bool shift) {
  if (name == "bitdepth")
    return float(bits);
  if (name == "ymin" || name == "ymax") {
    float value = name == "ymin" ? 16.0f : 235.0f;
    return bits == 32 ? value / 255.0f : value * float(1u << (bits - 8));
  }
  if (name == "cmin" || name == "cmax") {
    float value = name == "cmin" ? 16.0f : 240.0f;
    return bits == 32 ? (value - 128.0f) / 255.0f + (shift ? 0.5f : 0.0f) : value * float(1u << (bits - 8));
  }
  if (name == "range_size")
    return bits == 32 ? 1.0f : float(1u << bits);
  bool use_chroma = chroma && name.substr(0, 6) != "yrange";
  if (name == "range_min" || name == "yrange_min")
    return bits == 32 && use_chroma && !shift ? -0.5f : 0.0f;
  if (name == "range_max" || name == "yrange_max")
    return bits == 32 ? (use_chroma && !shift ? 0.5f : 1.0f) : float((1u << bits) - 1);
  return bits == 32 ? (use_chroma && !shift ? 0.0f : 0.5f) : float(1u << (bits - 1));
}
} // namespace iris
