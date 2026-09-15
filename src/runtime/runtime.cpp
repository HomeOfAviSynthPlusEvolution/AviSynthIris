#include "runtime.hpp"
#include <limits>
#include <cstring>
namespace iris {
namespace {
// Clang's function-type sanitizer probes metadata at function_address - 8.
// ORC code has no Clang UBSan prefix and may begin at an allocation boundary.
// Exclude only this known private-ABI call, keeping ASan and all other checks.
#if defined(__clang__)
__attribute__((no_sanitize("function"), noinline))
#endif
void call_jit_row(JitCode::Row row, const ExecuteArgs* args, uint32_t y) noexcept {
  row(args, y);
}
} // namespace
void execute_program(const Program& p, std::vector<float>& scratch, const ExecuteArgs& a) noexcept {
  if (p.info.strategy == IRIS_LUT_U8) {
    const auto& in = a.inputs[p.lut_input];
    const auto& out = a.output;
    size_t bytes = p.options.output.type == IRIS_U8 ? 1 : p.options.output.type == IRIS_U16 ? 2 : 4;
    for (uint32_t y = 0; y < p.options.height; ++y) {
      const auto* src = static_cast<const unsigned char*>(in.data) + ptrdiff_t(y) * in.stride;
      auto* dst = static_cast<unsigned char*>(out.data) + ptrdiff_t(y) * out.stride;
      for (uint32_t x = 0; x < p.options.width; ++x)
        std::memcpy(dst + size_t(x) * bytes, p.lut.data() + size_t(src[x]) * bytes, bytes);
    }
  } else if (p.jit) {
    for (uint32_t y = 0; y < p.options.height; ++y)
      call_jit_row(p.jit->row, &a, y);
  } else
    run(p, scratch, a);
}
void prepare_lut(Program& p) {
  if (p.info.strategy != IRIS_COMPUTE || p.options.input_count != 1 || p.options.inputs[0].type != IRIS_U8 ||
      p.info.input_mask != 1 || p.info.metadata_mask || p.info.property_count || p.info.has_relative_access)
    return;
  // No metadata, properties or neighborhood: changing evaluation geometry is safe.
  // Use the existing scalar pipeline including output conversion, not another evaluator.
  Program evaluation;
  evaluation.options = p.options;
  evaluation.options.width = 256;
  evaluation.options.height = 1;
  evaluation.ir = p.ir;
  evaluation.info = p.info;
  unsigned char inputs[256];
  for (size_t i = 0; i < 256; ++i)
    inputs[i] = static_cast<unsigned char>(i);
  std::vector<unsigned char> table(256 * sample_bytes(p.options.output));
  std::vector<float> scratch(p.ir.nodes.size());
  ExecuteArgs args{};
  args.inputs[0] = {inputs, 256};
  args.output = {table.data(), ptrdiff_t(table.size())};
  run(evaluation, scratch, args);
  p.lut = std::move(table);
  p.lut_input = 0;
  p.info.strategy = IRIS_LUT_U8;
}
#ifndef IRIS_WITH_LLVM
bool llvm_available() noexcept {
  return false;
}
std::shared_ptr<const JitCode> compile_llvm(const Program&) {
  throw Error(IRIS_BACKEND_UNAVAILABLE, "LLVM was disabled at build time");
}
#endif
size_t sample_bytes(iris_format f) {
  switch (f.type) {
    case IRIS_U8:
      if (f.bits == 8)
        return 1;
      break;
    case IRIS_U16:
      if (f.bits >= 9 && f.bits <= 16)
        return 2;
      break;
    case IRIS_F32:
      if (f.bits == 32)
        return 4;
      break;
  }
  fail("invalid sample format or bit depth");
}
void validate_options(const CompileOptions& o) {
  if (!o.width || !o.height || o.width > INT32_MAX || o.height > INT32_MAX)
    fail("dimensions must be in 1..INT32_MAX");
  if (o.input_count > (o.extended_inputs ? 26u : 3u) || (o.optimize != 0 && o.optimize != 1))
    fail("invalid input count or optimization flag");
  sample_bytes(o.output);
  for (uint32_t i = 0; i < o.input_count; ++i)
    sample_bytes(o.inputs[i]);
}
void describe(Program& p) {
  auto& info = p.info;
  info = {};
  info.width = p.options.width;
  info.height = p.options.height;
  info.instruction_count = p.ir.nodes.size();
  info.property_count = p.ir.properties.size();
  for (const auto& n : p.ir.nodes) {
    if (n.op == Op::Input) {
      info.input_mask |= 1u << n.input;
      if (n.dx || n.dy)
        info.has_relative_access = 1;
    }
    if (n.op >= Op::Sx && n.op <= Op::Time)
      info.metadata_mask |= 1u << (static_cast<unsigned>(n.op) - static_cast<unsigned>(Op::Sx));
  }
  const auto& r = p.ir.nodes[p.ir.result];
  if (p.options.optimize && r.op == Op::Constant)
    info.strategy = IRIS_FILL;
  if (p.options.optimize && r.op == Op::Input && !r.dx && !r.dy &&
      p.options.inputs[r.input].type == p.options.output.type &&
      p.options.inputs[r.input].bits == p.options.output.bits)
    info.strategy = IRIS_COPY;
  p.text = dump(p.ir);
}
void validate_execution(const Program& p, const ExecuteArgs& a) {
  if ((p.info.metadata_mask & IRIS_DEP_TIME) && a.frameno >= p.options.expr.frame_count)
    fail("frame index outside Expr frame count");
  auto plane = [&](const void* data, ptrdiff_t stride, iris_format f) {
    if (!data)
      fail("required plane data is null");
    const uint64_t row = uint64_t(p.options.width) * sample_bytes(f);
    const uint64_t pitch = stride < 0 ? uint64_t(-(stride + 1)) + 1 : uint64_t(stride);
    const uint64_t limit = static_cast<uint64_t>(PTRDIFF_MAX);
    if (row > limit || (p.options.height > 1 && (pitch < row || pitch > (limit - (row - 1)) / (p.options.height - 1))))
      fail("invalid stride or plane span overflow");
    const uint64_t span = pitch * (p.options.height - 1);
    const uintptr_t address = reinterpret_cast<uintptr_t>(data);
    if (stride < 0) {
      if (span > address || row - 1 > UINTPTR_MAX - address)
        fail("plane address overflow");
    } else if (span > UINTPTR_MAX - address || row - 1 > UINTPTR_MAX - address - span)
      fail("plane address overflow");
  };
  plane(a.output.data, a.output.stride, p.options.output);
  for (uint32_t i = 0; i < p.options.input_count; ++i)
    if (p.info.input_mask & (1u << i))
      plane(a.inputs[i].data, a.inputs[i].stride, p.options.inputs[i]);
  if (a.property_count < p.ir.properties.size() || (!p.ir.properties.empty() && !a.properties))
    fail("missing property values");
}
} // namespace iris
