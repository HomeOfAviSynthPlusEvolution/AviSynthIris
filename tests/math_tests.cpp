#include "runtime/manual_lut.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#define CHECK(x)                                                                                                       \
  do {                                                                                                                 \
    if (!(x))                                                                                                          \
      throw std::runtime_error(#x);                                                                                    \
  } while (false)
namespace {
using namespace iris;
struct Plan {
  iris_plan* p = nullptr;
  iris_context* c = nullptr;
  ~Plan() {
    iris_context_destroy(c);
    iris_plan_destroy(p);
  }
};
void check(iris_status s, const iris_diagnostic& d) {
  if (s != IRIS_OK)
    throw std::runtime_error(d.message);
}
void near(float actual, long double reference, double ulps) {
  float rounded = float(reference);
  if (std::isnan(rounded)) {
    CHECK(std::isnan(actual));
    return;
  }
  if (std::isinf(rounded)) {
    CHECK(actual == rounded);
    return;
  }
  CHECK(std::isfinite(actual));
  if (rounded == 0 && actual == 0)
    CHECK(std::signbit(actual) == std::signbit(rounded));
  int exponent = 0;
  std::frexp(reference, &exponent);
  long double unit = std::ldexp(1.L, std::max(-149, exponent - 24));
  CHECK(std::fabs(static_cast<long double>(actual) - reference) <= ulps * unit);
}
long double oracle(const std::string& name, float a, float b) {
  long double x = a, y = b;
  if (name == "sin")
    return std::sin(x);
  if (name == "cos")
    return std::cos(x);
  if (name == "tan")
    return std::tan(x);
  if (name == "asin")
    return std::asin(x);
  if (name == "acos")
    return std::acos(x);
  if (name == "atan")
    return std::atan(x);
  if (name == "exp")
    return std::exp(x);
  if (name == "log")
    return std::log(x);
  if (name == "pow")
    return std::pow(x, y);
  return std::atan2(x, y);
}
iris_status execute(const Plan& p, const iris_execute_args_v1& a, iris_backend executor, iris_diagnostic& d) {
  if (executor != IRIS_BACKEND_SCALAR)
    return iris_execute_v1(p.p, p.c, &a, &d);
  auto program = *p.p->program;
  program.jit.reset();
  std::vector<float> scratch(program.ir.nodes.size());
  iris::ExecuteArgs args;
  std::copy_n(a.inputs, 26, args.inputs);
  args.output = a.output;
  args.frameno = a.frameno;
  args.properties = a.properties;
  args.property_count = a.property_count;
  iris::execute_program(program, scratch, args);
  return IRIS_OK;
}
void run(iris_backend backend) {
  iris_compile_options_v1 o{};
  o.struct_size = sizeof(o);
  o.height = 2;
  o.input_count = 2;
  o.backend = backend;
  o.inputs[0] = o.inputs[1] = o.output = {IRIS_F32, 32};
  iris_diagnostic d{};
  CHECK(!iris_backend_available(99));
  o.width = 1;
  Plan invalid;
  o.backend = IRIS_BACKEND_NONE; // query-only value is invalid for compilation
  CHECK(iris_compile_v1("0", &o, &invalid.p, &d) == IRIS_INVALID_ARGUMENT);
  std::vector<unsigned char> accurate_sin;
  for (auto mode : {IRIS_MATH_NATIVE, IRIS_MATH_ACCURATE, IRIS_MATH_FAST}) {
    o.backend = mode == IRIS_MATH_NATIVE     ? backend
                : mode == IRIS_MATH_ACCURATE ? IRIS_BACKEND_SLEEF
                                             : IRIS_BACKEND_SLEEF_FAST;
    if (!iris_backend_available(o.backend)) {
      CHECK(iris_compile_v1("0", &o, &invalid.p, &d) == IRIS_BACKEND_UNAVAILABLE);
      CHECK(invalid.p == nullptr);
      continue;
    }
    bool saw_vector = false;
    for (const char* operation : {"sin", "cos", "tan", "asin", "acos", "atan", "exp", "log", "pow", "atan2"}) {
      std::string name = operation;
      bool binary = name == "pow" || name == "atan2";
      for (uint32_t width : {1u, 7u, 8u, 9u, 15u, 16u, 17u, 129u})
        for (int optimized : {0, 1}) {
          o.width = width;
          o.optimize = optimized;
          Plan p;
          std::string expr = binary ? "x y " + name : "x " + name;
          check(iris_compile_v1(expr.c_str(), &o, &p.p, &d), d);
          check(iris_context_create(p.p, &p.c, &d), d);
          iris_backend reported = IRIS_BACKEND_NONE;
          check(iris_plan_get_backend(p.p, &reported, &d), d);
          CHECK(reported == o.backend);
          if (mode != IRIS_MATH_NATIVE && width == 129 && name == "sin" && p.p->program->jit &&
              p.p->program->jit->vector_math_available) {
            CHECK(p.p->program->jit->vector_math_calls > 0);
            saw_vector = true;
          }
          size_t stride = width * 4 + 5;
          std::vector<unsigned char> a(stride * 2 + 2), b(a.size()), out(a.size(), 0xcd);
          iris_execute_args_v1 args{};
          args.struct_size = sizeof(args);
          args.input_count = 2;
          args.inputs[0] = {a.data() + 1 + stride, -ptrdiff_t(stride)};
          args.inputs[1] = {b.data() + 1 + stride, -ptrdiff_t(stride)};
          args.output = {out.data() + 1 + stride, -ptrdiff_t(stride)};
          for (size_t row = 0; row < 2; ++row)
            for (size_t x = 0; x < width; ++x) {
              float v = float(int((x + row * 13) % 97) - 48) / 64, w = 0.75f;
              if (name == "pow" || name == "log")
                v += 1;
              const float special[] = {0.0f,    -0.0f, INFINITY, -INFINITY, NAN, std::numeric_limits<float>::min(),
                                       1024.0f, -1.0f};
              if (x < 8)
                v = special[x];
              if (name == "sin" && x == 8) {
                uint32_t witness = 3218302676u;
                std::memcpy(&v, &witness, sizeof(v));
              }
              std::memcpy(a.data() + 1 + row * stride + x * 4, &v, 4);
              std::memcpy(b.data() + 1 + row * stride + x * 4, &w, 4);
            }
          check(execute(p, args, backend, d), d);
          double limit = mode == IRIS_MATH_FAST && name != "exp" && name != "pow" ? 3.5 : 1;
          if (name == "atan2" && mode == IRIS_MATH_ACCURATE)
            limit = 2;
          for (size_t row = 0; row < 2; ++row)
            for (size_t x = 0; x < width; ++x) {
              float v, w, r;
              std::memcpy(&v, a.data() + 1 + row * stride + x * 4, 4);
              std::memcpy(&w, b.data() + 1 + row * stride + x * 4, 4);
              std::memcpy(&r, out.data() + 1 + row * stride + x * 4, 4);
              try {
                near(r, oracle(name, v, w), limit);
              } catch (...) {
                std::cerr << name << " mode=" << mode << " x=" << v << " y=" << w << " actual=" << r
                          << " reference=" << oracle(name, v, w) << '\n';
                throw;
              }
            }
          if (name == "sin" && width == 129 && optimized) {
            if (mode == IRIS_MATH_ACCURATE)
              accurate_sin = out;
            else if (mode == IRIS_MATH_FAST)
              CHECK(out != accurate_sin);
          }
          CHECK(out.front() == 0xcd);
          CHECK(out.back() == 0xcd);
          // Constant folding must use the selected family as well.
          Plan folded;
          std::string constant = binary ? "0.75 0.75 " + name : "0.75 " + name;
          check(iris_compile_v1(constant.c_str(), &o, &folded.p, &d), d);
          check(iris_context_create(folded.p, &folded.c, &d), d);
          check(execute(folded, args, backend, d), d);
          float r;
          std::memcpy(&r, out.data() + 1, 4);
          near(r, oracle(name, .75f, .75f), limit);
        }
    }
    std::cout << "math mode " << mode << " vector calls observed=" << saw_vector << '\n';
    // A compound expression with clamped neighbours and a dynamic property.
    o.width = 129;
    o.height = 2;
    o.input_count = 1;
    o.inputs[0] = {IRIS_F32, 32};
    o.optimize = 1;
    {
      Plan p;
      check(iris_compile_v1("x[-1,0] sin x[1,0] cos + x.Gain + sy +", &o, &p.p, &d), d);
      check(iris_context_create(p.p, &p.c, &d), d);
      std::vector<float> input(258), output(258);
      for (size_t i = 0; i < input.size(); ++i)
        input[i] = float(i) * .01f;
      float gain = .25f;
      iris_execute_args_v1 a{};
      a.struct_size = sizeof(a);
      a.input_count = 1;
      a.inputs[0] = {input.data(), 129 * 4};
      a.output = {output.data(), 129 * 4};
      a.properties = &gain;
      a.property_count = 1;
      for (float property : {.25f, .75f}) {
        gain = property;
        check(execute(p, a, backend, d), d);
        for (size_t row = 0; row < 2; ++row)
          for (size_t x = 0; x < 129; ++x) {
            float expected = std::sin(input[row * 129 + (x ? x - 1 : 0)]) +
                             std::cos(input[row * 129 + std::min(x + 1, size_t(128))]) + gain + float(row);
            CHECK(std::abs(output[row * 129 + x] - expected) <= 1e-6f);
          }
      }
    }
    // Both automatic and explicit LUT construction inherit the mode.
    o.width = 256;
    o.height = 1;
    o.input_count = 1;
    o.inputs[0] = {IRIS_U8, 8};
    o.optimize = 1;
    std::vector<uint8_t> input(256);
    std::vector<float> out(256);
    for (size_t i = 0; i < 256; ++i)
      input[i] = uint8_t(i);
    for (int automatic : {0, 1}) {
      o.enable_lut = automatic;
      Plan p;
      check(iris_compile_v1("x sin", &o, &p.p, &d), d);
      check(iris_context_create(p.p, &p.c, &d), d);
      iris_execute_args_v1 a{};
      a.struct_size = sizeof(a);
      a.input_count = 1;
      a.inputs[0] = {input.data(), 256};
      a.output = {out.data(), 1024};
      check(execute(p, a, backend, d), d);
      for (size_t i = 0; i < 256; ++i)
        near(out[i], std::sin(static_cast<long double>(i)), mode == IRIS_MATH_FAST ? 3.5 : 1);
      iris::ManualLut lut(*p.p, backend);
      lut.build({});
      lut.apply(a.inputs, a.output, 256, 1);
      for (size_t i = 0; i < 256; ++i)
        near(out[i], std::sin(static_cast<long double>(i)), mode == IRIS_MATH_FAST ? 3.5 : 1);
    }
    o.enable_lut = 0;
    o.height = 2;
    o.input_count = 2;
    o.inputs[0] = o.inputs[1] = {IRIS_F32, 32};
  }
}
} // namespace
int main(int argc, char**) {
  try {
    run(argc > 1 ? IRIS_BACKEND_LLVM : IRIS_BACKEND_SCALAR);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
