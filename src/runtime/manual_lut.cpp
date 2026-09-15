#include "manual_lut.hpp"
#include <cstring>
#include <limits>
#include <string_view>

namespace iris {
uint64_t lut_storage_bytes(uint64_t entries, uint64_t bytes) {
  if (!bytes || entries > uint64_t(PTRDIFF_MAX) / bytes || entries > uint64_t(SIZE_MAX) / bytes)
    throw Error(IRIS_LIMIT_EXCEEDED, "LUT storage size exceeds addressable capacity");
  return entries * bytes;
}
uint64_t lut_add_bytes(uint64_t total, uint64_t bytes) {
  if (bytes > UINT64_MAX - total)
    throw Error(IRIS_LIMIT_EXCEEDED, "aggregate LUT storage size overflow");
  return total + bytes;
}
void check_lut_budget(uint64_t bytes, int64_t max_mib) {
  constexpr uint64_t mib = 1024 * 1024;
  if (max_mib == 0 || max_mib < -1)
    fail("lut_max_mb must be a positive integer or -1");
  if (max_mib == -1)
    return;
  if (uint64_t(max_mib) > UINT64_MAX / mib)
    fail("lut_max_mb is too large");
  if (bytes > uint64_t(max_mib) * mib) {
    auto required = bytes / mib + (bytes % mib != 0);
    throw Error(IRIS_LIMIT_EXCEEDED, "LUT tables require " + std::to_string(bytes) + " bytes (" +
                                         std::to_string(required) + " MiB rounded up), exceeding lut_max_mb=" +
                                         std::to_string(max_mib) + ". Set lut_max_mb=" + std::to_string(required) +
                                         " or higher, or lut_max_mb=-1 to disable the LUT memory limit.");
  }
}
void validate_lut_source(const std::string& source) {
  // Match the frontend's ASCII whitespace. Check before optimization so dead
  // dynamic expressions and zero-offset relative syntax are rejected as well.
  size_t at = 0;
  while ((at = source.find_first_not_of(" \t\n\r\f\v", at)) != std::string::npos) {
    auto end = source.find_first_of(" \t\n\r\f\v", at);
    if (end == std::string::npos)
      end = source.size();
    std::string_view token(source.data() + at, end - at);
    if (token == "sx" || token == "sy" || token == "sxr" || token == "syr" || token == "frameno" || token == "time" ||
        token.find('[') != std::string_view::npos)
      throw Error(IRIS_INVALID_ARGUMENT, "manual LUT does not support coordinates, time or relative input access", at,
                  end - at);
    at = end;
  }
}
ManualLut::ManualLut(const iris_plan& plan, iris_backend backend) : backend_(backend) {
  const auto& source = *plan.program;
  if (source.options.input_count < 1 || source.options.input_count > 2)
    fail("manual LUT requires one or two inputs");
  if (backend != IRIS_BACKEND_SCALAR && backend != IRIS_BACKEND_LLVM)
    fail("invalid LUT generation backend");
  if (backend == IRIS_BACKEND_LLVM && !llvm_available())
    throw Error(IRIS_BACKEND_UNAVAILABLE, "LLVM was disabled at build time");
  evaluation_.options = source.options;
  evaluation_.options.width = chunk;
  evaluation_.options.height = 1;
  evaluation_.ir = source.ir;
  for (auto& node : evaluation_.ir.nodes) {
    if (node.op == Op::Sx || node.op == Op::Sy || node.op == Op::Frame || node.op == Op::Time ||
        (node.op == Op::Input && (node.dx || node.dy)))
      fail("manual LUT contains a dynamic dependency");
    if (node.op == Op::Width || node.op == Op::Height) {
      node.value = float(node.op == Op::Width ? source.options.width : source.options.height);
      node.op = Op::Constant;
    }
  }
  verify(evaluation_.ir);
  describe(evaluation_);
  sample_bytes_ = sample_bytes(source.options.output);
  uint64_t entries = 1;
  for (uint32_t i = 0; i < source.options.input_count; ++i) {
    if (source.options.inputs[i].type == IRIS_F32)
      fail("manual LUT requires integer inputs; use lut=0 for floating-point inputs");
    domains_[i] = 1u << source.options.inputs[i].bits;
    if (input_mask() & (1u << i)) {
      axes_[i] = size_t(entries);
      entries *= domains_[i];
    }
  }
  storage_bytes_ = lut_storage_bytes(entries, sample_bytes_);
  entries_ = size_t(entries);
}
void ManualLut::build(const std::vector<float>& properties) {
  if (table_)
    fail("LUT has already been built");
  if (properties.size() != evaluation_.ir.properties.size())
    fail("missing LUT snapshot properties");
  try {
    // No value initialization: each byte is written by the shared output path.
    auto table = std::unique_ptr<unsigned char[]>(new unsigned char[size_t(storage_bytes_)]);
    if (backend_ == IRIS_BACKEND_LLVM && evaluation_.info.strategy == IRIS_COMPUTE)
      evaluation_.jit = compile_llvm(evaluation_);
    std::vector<float> scratch(evaluation_.ir.nodes.size());
    std::array<std::array<unsigned char, chunk * 2>, 2> input{};
    std::array<unsigned char, chunk * 4> output{};
    ExecuteArgs args;
    args.properties = properties.data();
    args.property_count = properties.size();
    args.output = {output.data(), ptrdiff_t(output.size())};
    for (size_t i = 0; i < 2; ++i)
      args.inputs[i] = {input[i].data(), ptrdiff_t(input[i].size())};
    for (size_t base = 0; base < entries_; base += chunk) {
      for (size_t i = 0; i < 2; ++i) {
        if (!axes_[i])
          continue;
        for (size_t x = 0; x < chunk; ++x) {
          auto value = uint16_t(((base + x) / axes_[i]) % domains_[i]);
          if (evaluation_.options.inputs[i].type == IRIS_U8)
            input[i][x] = static_cast<unsigned char>(value);
          else
            std::memcpy(input[i].data() + x * 2, &value, 2);
        }
      }
      execute_program(evaluation_, scratch, args);
      std::memcpy(table.get() + base * sample_bytes_, output.data(),
                  std::min(size_t(chunk), entries_ - base) * sample_bytes_);
    }
    table_ = std::move(table);
    // Lookup needs only domains, axes and final bytes, not generated code.
    evaluation_.jit.reset();
  } catch (const std::bad_alloc&) {
    throw Error(IRIS_OUT_OF_MEMORY, "LUT allocation failed while building " + std::to_string(storage_bytes_) +
                                        " bytes; available memory is insufficient");
  }
}
void ManualLut::apply(const iris_input_plane* inputs, iris_output_plane output, uint32_t width,
                      uint32_t height) const noexcept {
  for (uint32_t y = 0; y < height; ++y) {
    auto* dst = static_cast<unsigned char*>(output.data) + ptrdiff_t(y) * output.stride;
    std::array<const unsigned char*, 2> rows{};
    for (size_t i = 0; i < 2; ++i)
      if (axes_[i])
        rows[i] = static_cast<const unsigned char*>(inputs[i].data) + ptrdiff_t(y) * inputs[i].stride;
    for (uint32_t x = 0; x < width; ++x) {
      size_t index = 0;
      for (size_t i = 0; i < 2; ++i) {
        if (!axes_[i])
          continue;
        uint16_t value = 0;
        if (evaluation_.options.inputs[i].type == IRIS_U8)
          value = rows[i][x];
        else
          std::memcpy(&value, rows[i] + size_t(x) * 2, 2);
        index += size_t(std::min(uint32_t(value), domains_[i] - 1)) * axes_[i];
      }
      std::memcpy(dst + size_t(x) * sample_bytes_, table_.get() + index * sample_bytes_, sample_bytes_);
    }
  }
}
} // namespace iris
