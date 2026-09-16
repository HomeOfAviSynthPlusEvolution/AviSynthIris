#include "runtime/manual_lut.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>

thread_local bool reject_allocation = false;
thread_local bool fail_array = false;
void* operator new(std::size_t n) {
  if (reject_allocation)
    throw std::bad_alloc();
  if (auto* p = std::malloc(n ? n : 1))
    return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (fail_array) {
    fail_array = false;
    throw std::bad_alloc();
  }
  return ::operator new(n);
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete[](void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  std::free(p);
}

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
iris_backend backend = IRIS_BACKEND_SCALAR;
iris_format fmt(uint32_t bits) {
  return {bits == 8 ? IRIS_U8 : bits == 32 ? IRIS_F32 : IRIS_U16, bits};
}
struct Compiled {
  iris_plan* plan = nullptr;
  iris_context* context = nullptr;
  Compiled(const std::string& expression, const iris_compile_options_v1& o,
           iris_expr_options_v1 e = {sizeof(iris_expr_options_v1), 3, 0, IRIS_SCALE_NONE, 0, 0}) {
    iris::validate_lut_source(expression);
    iris_diagnostic d{};
    if (iris_compile_expr_v1(expression.c_str(), &o, &e, &plan, &d) != IRIS_OK)
      throw std::runtime_error(d.message);
    if (iris_context_create(plan, &context, &d) != IRIS_OK) {
      iris_plan_destroy(plan);
      throw std::runtime_error(d.message);
    }
  }
  ~Compiled() {
    iris_context_destroy(context);
    iris_plan_destroy(plan);
  }
};
iris_compile_options_v1 options(uint32_t bits, uint32_t second, uint32_t output, int optimize) {
  iris_compile_options_v1 o{};
  o.struct_size = sizeof(o);
  o.width = 1u << bits;
  o.height = second ? 1u << second : 1;
  o.input_count = second ? 2 : 1;
  o.inputs[0] = fmt(bits);
  o.inputs[1] = fmt(second ? second : bits);
  o.output = fmt(output);
  o.optimize = optimize;
  o.backend = backend;
  return o;
}
// Deliberately unaligned samples and odd, optionally negative pitches.
struct Buffer {
  std::vector<unsigned char> bytes;
  unsigned char* data;
  ptrdiff_t stride;
  Buffer(uint32_t width, uint32_t height, size_t sample_bytes, bool negative)
      : bytes((size_t(width) * sample_bytes + 3) * height + 2, 0xa5), data(bytes.data() + 1),
        stride(ptrdiff_t(size_t(width) * sample_bytes + 3)) {
    if (negative) {
      data += stride * (height - 1);
      stride = -stride;
    }
  }
};
void run_case(const std::string& expression, iris_compile_options_v1 o,
              const std::function<float(uint32_t, uint32_t)>& expected, bool negative = false,
              iris_expr_options_v1 e = {sizeof(iris_expr_options_v1), 3, 0, IRIS_SCALE_NONE, 0, 0}) {
  Compiled compiled(expression, o, e);
  iris::ManualLut lut(*compiled.plan, backend);
  iris::check_lut_budget(lut.storage_bytes(), 256);
  lut.build({});
  size_t out_bytes = iris::sample_bytes(o.output);
  Buffer x(o.width, o.height, iris::sample_bytes(o.inputs[0]), negative);
  Buffer y(o.width, o.height, iris::sample_bytes(o.inputs[1]), !negative);
  Buffer out(o.width, o.height, out_bytes, negative);
  Buffer direct(o.width, o.height, out_bytes, negative);
  for (uint32_t row = 0; row < o.height; ++row)
    for (uint32_t col = 0; col < o.width; ++col) {
      uint16_t xv = uint16_t(col), yv = uint16_t(row);
      std::memcpy(x.data + ptrdiff_t(row) * x.stride + size_t(col) * iris::sample_bytes(o.inputs[0]), &xv,
                  iris::sample_bytes(o.inputs[0]));
      std::memcpy(y.data + ptrdiff_t(row) * y.stride + size_t(col) * iris::sample_bytes(o.inputs[1]), &yv,
                  iris::sample_bytes(o.inputs[1]));
    }
  iris_execute_args_v1 args{};
  args.struct_size = sizeof(args);
  args.input_count = o.input_count;
  args.inputs[0] = {x.data, x.stride};
  args.inputs[1] = {y.data, y.stride};
  args.output = {direct.data, direct.stride};
  require(iris_execute_v1(compiled.plan, compiled.context, &args, nullptr) == IRIS_OK, "direct evaluation failed");
  reject_allocation = true;
  lut.apply(args.inputs, {out.data, out.stride}, o.width, o.height);
  reject_allocation = false;
  for (uint32_t row = 0; row < o.height; ++row) {
    auto* result = out.data + ptrdiff_t(row) * out.stride;
    auto* reference = direct.data + ptrdiff_t(row) * direct.stride;
    for (uint32_t col = 0; col < o.width; ++col) {
      auto* at = result + size_t(col) * out_bytes;
      float want = expected(col, row), actual = 0;
      if (o.output.type == IRIS_F32) {
        std::memcpy(&actual, at, 4);
        float computed = 0;
        std::memcpy(&computed, reference + size_t(col) * 4, 4);
        require((std::isnan(actual) && std::isnan(computed)) || std::memcmp(at, reference + size_t(col) * 4, 4) == 0,
                "LUT/direct float mismatch");
      } else {
        uint16_t value = 0;
        std::memcpy(&value, at, out_bytes);
        actual = value;
        want = !(want > 0) ? 0 : std::floor(std::min(want, float((1u << o.output.bits) - 1)) + 0.5f);
        require(std::memcmp(at, reference + size_t(col) * out_bytes, out_bytes) == 0, "LUT/direct integer mismatch");
      }
      require((std::isnan(actual) && std::isnan(want)) ||
                  (actual == want && (actual != 0 || std::signbit(actual) == std::signbit(want))),
              "independent LUT result mismatch");
    }
    for (size_t i = size_t(o.width) * out_bytes; i < size_t(o.width) * out_bytes + 3; ++i)
      require(result[i] == 0xa5, "LUT wrote row padding");
  }
  require(out.bytes.front() == 0xa5 && out.bytes.back() == 0xa5, "LUT wrote buffer guard");
}
template <class F>
void error_contains(F f, const char* text) {
  try {
    f();
  } catch (const iris::Error& e) {
    require(std::string(e.what()).find(text) != std::string::npos, "unexpected LUT diagnostic");
    return;
  }
  throw std::runtime_error("expected LUT error");
}
void budget() {
  constexpr uint64_t mib = 1024 * 1024;
  require(iris::lut_storage_bytes(uint64_t(1) << 20, 2) == 2 * mib, "10-bit LUT2 size");
  iris::check_lut_budget(256 * mib, 256);
  iris::check_lut_budget(512 * mib, 512);
  iris::check_lut_budget(20 * 1024 * mib, -1);
  for (const char* diagnostic : {"268435457 bytes", "lut_max_mb=256", "lut_max_mb=257", "lut_max_mb=-1"})
    error_contains([&] { iris::check_lut_budget(256 * mib + 1, 256); }, diagnostic);
  for (int invalid : {0, -2, INT32_MIN})
    error_contains([&] { iris::check_lut_budget(0, invalid); }, "positive integer or -1");
  error_contains([] { iris::lut_add_bytes(UINT64_MAX, 1); }, "overflow");
  error_contains([] { iris::lut_storage_bytes(UINT64_MAX, 4); }, "capacity");
  error_contains([] { iris::check_lut_budget(0, INT64_MAX); }, "too large");
  auto o = options(16, 16, 32, 1);
  Compiled compiled("x y +", o);
  iris::ManualLut big(*compiled.plan, backend);
  require(big.storage_bytes() == uint64_t(16) * 1024 * mib, "16-bit LUT2 size");
  auto aggregate = iris::lut_add_bytes(big.storage_bytes(), big.storage_bytes());
  iris::check_lut_budget(aggregate, -1); // 32 GiB, no allocation.
  error_contains([&] { iris::check_lut_budget(aggregate, 256); }, "lut_max_mb=32768");
  for (const char* expression : {"sx", "sy", "sxr", "syr", "frameno", "time", "sx unused^ 1", "x[0,0]"})
    error_contains([&] { iris::validate_lut_source(expression); }, "manual LUT does not support");
  iris::validate_lut_source("x.time x.sx + x.width +");
}
void snapshots_and_bounds() {
  auto o = options(10, 8, 32, 1);
  o.width = 3;
  o.height = 1;
  Compiled compiled("x y + x.Gain *", o);
  iris::ManualLut lut(*compiled.plan, backend);
  error_contains([&] { lut.build({}); }, "snapshot properties");
  fail_array = true;
  error_contains([&] { lut.build({2}); }, "LUT allocation failed");
  lut.build({2});
  error_contains([&] { lut.build({2}); }, "already been built");
  uint16_t x[] = {1023, 1024, 65535};
  unsigned char y[] = {0, 1, 255};
  float output[3]{};
  iris_input_plane inputs[] = {{x, 6}, {y, 3}};
  lut.apply(inputs, {output, 12}, 3, 1);
  require(output[0] == 2046 && output[1] == 2048 && output[2] == 2556, "index clamping or snapshot mismatch");
  Compiled constant("x.Gain width + height +", o);
  iris::ManualLut fill(*constant.plan, backend);
  require(fill.storage_bytes() == 4, "property-only table should contain one result");
  fill.build({2});
  fill.apply(nullptr, {output, 12}, 3, 1);
  require(output[0] == 6 && output[2] == 6, "property-only fill mismatch");
  for (uint32_t bits : {8u, 10u}) {
    for (uint32_t out_bits : {8u, 16u, 32u}) {
      auto second_options = options(10, bits, out_bits, 1);
      Compiled second("y", second_options);
      iris::ManualLut selected(*second.plan, backend);
      require(selected.input_mask() == 2, "unused first LUT axis retained");
      selected.build({});
      uint16_t sample = bits == 8 ? 200 : 65535;
      unsigned char result[4]{};
      iris_input_plane planes[] = {{nullptr, 0}, {&sample, 2}};
      selected.apply(planes, {result, 4}, 1, 1);
      float value = 0;
      if (out_bits == 32)
        std::memcpy(&value, result, 4);
      else {
        uint16_t integer = 0;
        std::memcpy(&integer, result, out_bits == 8 ? 1 : 2);
        value = integer;
      }
      require(value == (bits == 8 ? 200 : out_bits == 8 ? 255 : 1023), "second-only LUT clamping mismatch");
    }
  }
  auto copy_options = options(10, 0, 10, 1);
  Compiled copy("x", copy_options);
  iris::ManualLut copy_lut(*copy.plan, backend);
  copy_lut.build({});
  uint16_t illegal = 65535, clamped = 0;
  iris_input_plane plane{&illegal, 2};
  copy_lut.apply(&plane, {&clamped, 2}, 1, 1);
  require(clamped == 1023, "optimized x bypassed LUT index clamping");
}
} // namespace
int main(int argc, char**) {
  try {
    if (argc > 1)
      backend = IRIS_BACKEND_LLVM;
    budget();
    snapshots_and_bounds();
    for (int optimize : {0, 1}) {
      for (uint32_t bits : {8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u})
        for (uint32_t output : {8u, 16u, 32u})
          run_case(
              "x 3 * 0.5 +", options(bits, 0, output, optimize),
              [](uint32_t x, uint32_t) { return float(x * 3) + 0.5f; }, optimize != 0);
      for (const auto& bits : {std::array<uint32_t, 2>{8, 8}, {10, 10}, {8, 10}, {10, 8}})
        for (uint32_t output : {8u, 10u, 32u})
          run_case(
              "x 3 * y 5 * + 0.5 +", options(bits[0], bits[1], output, optimize),
              [](uint32_t x, uint32_t y) { return float(x * 3 + y * 5) + 0.5f; }, optimize != 0);
      auto small = options(8, 0, 32, optimize);
      run_case("x 128 - sqrt", small, [](uint32_t x, uint32_t) { return x <= 128 ? 0.0f : std::sqrt(float(x - 128)); });
      run_case("1 x 1 - /", small, [](uint32_t x, uint32_t) { return 1.0f / (float(x) - 1.0f); });
      run_case("x x - 0 /", small, [](uint32_t, uint32_t) { return std::numeric_limits<float>::quiet_NaN(); });
      run_case("-0 x *", small, [](uint32_t, uint32_t) { return -0.0f; });
      run_case("-0 x min", small, [](uint32_t, uint32_t) { return 0.0f; });
      small.width = 7;
      small.height = 3;
      run_case("x width + height +", small, [](uint32_t x, uint32_t) { return float(x + 10); }, true);
      small = options(10, 0, 10, optimize);
      run_case("x", small, [](uint32_t x, uint32_t) { return float(x); }, false,
               {sizeof(iris_expr_options_v1), 3, 1, IRIS_SCALE_ALL, 0, 0});
    }
    std::cout << "Manual LUT complete integer domains, mixed formats, snapshot, budgets and bounds passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
