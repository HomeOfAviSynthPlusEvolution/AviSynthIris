#pragma once
#include "runtime.hpp"
#include <array>

namespace iris {
// Internal host facility, separate from the optional automatic U8 strategy.
// Construction plans storage; build is called only after the host checks its
// aggregate budget and captures frame-zero properties.
uint64_t lut_storage_bytes(uint64_t entries, uint64_t bytes);
uint64_t lut_add_bytes(uint64_t total, uint64_t bytes);
void check_lut_budget(uint64_t bytes, int64_t max_mib);
void validate_lut_source(const std::string& source);
class ManualLut {
public:
  ManualLut(const iris_plan& plan, iris_backend backend);
  uint64_t storage_bytes() const { return storage_bytes_; }
  uint32_t input_mask() const { return evaluation_.info.input_mask; }
  void build(const std::vector<float>& properties);
  // The host supplies valid, nonoverlapping planes with the original geometry.
  // Built tables are immutable; lookup neither allocates nor evaluates IR.
  void apply(const iris_input_plane* inputs, iris_output_plane output, uint32_t width, uint32_t height) const noexcept;

private:
  static constexpr uint32_t chunk = 256;
  Program evaluation_;
  iris_backend backend_;
  std::array<uint32_t, 2> domains_{};
  std::array<size_t, 2> axes_{};
  size_t entries_ = 1, sample_bytes_ = 0;
  uint64_t storage_bytes_ = 0;
  std::unique_ptr<unsigned char[]> table_;
};
} // namespace iris
