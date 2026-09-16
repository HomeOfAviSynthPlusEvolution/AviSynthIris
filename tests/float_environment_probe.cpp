#include <iris/iris.h>
#include <xmmintrin.h>
#include <array>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {
struct Environment {
  unsigned saved = _mm_getcsr();
  ~Environment() { _mm_setcsr(saved); }
};
uint32_t evaluate(const char* source, uint32_t input_bits, iris_backend backend, int optimize, unsigned compile_mode,
                  unsigned execute_mode) {
  Environment restore;
  // Mask exceptions, clear sticky flags, then select rounding and denormal modes.
  _mm_setcsr(0x1f80 | compile_mode);
  iris_compile_options o{1, 1, 1, {{IRIS_F32, 32}}, {IRIS_F32, 32}, optimize};
  iris_plan* plan = nullptr;
  iris_diagnostic diagnostic{};
  if (iris_compile_ex(source, &o, backend, 0, &plan, &diagnostic) != IRIS_OK)
    throw std::runtime_error(diagnostic.message);
  iris_context* context = nullptr;
  if (iris_context_create(plan, &context, &diagnostic) != IRIS_OK) {
    iris_plan_destroy(plan);
    throw std::runtime_error(diagnostic.message);
  }
  float input, output = 0;
  std::memcpy(&input, &input_bits, 4);
  iris_execute_args args{};
  args.inputs[0] = {&input, 4};
  args.output = {&output, 4};
  _mm_setcsr(0x1f80 | execute_mode);
  auto status = iris_execute(plan, context, &args, &diagnostic);
  uint32_t result = 0;
  std::memcpy(&result, &output, 4);
  iris_context_destroy(context);
  iris_plan_destroy(plan);
  if (status != IRIS_OK)
    throw std::runtime_error(diagnostic.message);
  return result;
}
} // namespace
int main() {
  try {
    struct Case {
      const char* name;
      const char* expression;
      uint32_t input;
    };
    const Case cases[] = {{"constant_half_min_normal", "1.1754943508222875e-38 2 /", 0x00800000},
                          {"runtime_half_min_normal", "x 2 /", 0x00800000},
                          {"runtime_twice_min_subnormal", "x 2 *", 0x00000001},
                          {"runtime_subnormal_positive", "x 0 >", 0x00000001},
                          {"constant_midpoint", "16777216 1 +", 0x4b800000},
                          {"runtime_midpoint", "x 1 +", 0x4b800000}};
    struct Mode {
      const char* name;
      unsigned bits;
    };
    const Mode modes[] = {
        {"nearest_gradual", 0}, {"daz", 0x40}, {"ftz", 0x8000}, {"ftz_daz", 0x8040}, {"upward", 0x4000}};
    std::cout << "case,compile_environment,execute_environment,backend,optimize,result_hex\n";
    for (const auto& item : cases)
      for (const auto& compile_mode : modes)
        for (const auto& execute_mode : modes)
          for (int optimize : {0, 1}) {
            for (auto backend : {IRIS_BACKEND_SCALAR, IRIS_BACKEND_LLVM}) {
#ifndef IRIS_TEST_LLVM
              if (backend == IRIS_BACKEND_LLVM)
                continue;
#endif
              auto result =
                  evaluate(item.expression, item.input, backend, optimize, compile_mode.bits, execute_mode.bits);
              std::cout << item.name << ',' << compile_mode.name << ',' << execute_mode.name << ','
                        << (backend == IRIS_BACKEND_SCALAR ? "scalar" : "llvm") << ',' << optimize << ',' << std::hex
                        << std::setw(8) << std::setfill('0') << result << std::dec << '\n';
            }
          }
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
