#include <iris/iris.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <limits>

namespace {
iris_backend backend = IRIS_BACKEND_SCALAR;
#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition))                                                                                                  \
      throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #condition);                     \
  } while (0)
struct Plan {
  iris_plan* p = nullptr;
  iris_context* c = nullptr;
  ~Plan() {
    iris_context_destroy(c);
    iris_plan_destroy(p);
  }
};
iris_compile_options_v1 options(uint32_t bits, int optimize) {
  iris_compile_options_v1 o{};
  o.struct_size = sizeof(o);
  o.width = o.height = 1;
  o.input_count = 1;
  o.inputs[0] = {bits == 8 ? IRIS_U8 : bits == 32 ? IRIS_F32 : IRIS_U16, bits};
  o.output = {IRIS_F32, 32};
  o.backend = backend;
  o.optimize = optimize;
  return o;
}
iris_expr_options_v1 environment(int chroma = 0, iris_scale_inputs mode = IRIS_SCALE_NONE) {
  iris_expr_options_v1 e{};
  e.struct_size = sizeof(e);
  e.frame_count = 1;
  e.chroma = chroma;
  e.scale_inputs = mode;
  return e;
}
float evaluate(const char* expression, uint32_t bits, const iris_expr_options_v1& e, int optimize = 1,
               float sample = 0) {
  auto o = options(bits, optimize);
  Plan plan;
  iris_diagnostic diagnostic{};
  if (iris_compile_expr_v1(expression, &o, &e, &plan.p, &diagnostic) != IRIS_OK)
    throw std::runtime_error(std::string(expression) + ": " + diagnostic.message);
  CHECK(iris_context_create(plan.p, &plan.c, nullptr) == IRIS_OK);
  uint8_t u8 = bits == 8 ? uint8_t(sample) : 0;
  uint16_t u16 = bits < 32 ? uint16_t(sample) : 0;
  float output = 0;
  iris_execute_args_v1 a{};
  a.struct_size = sizeof(a);
  a.input_count = 1;
  a.inputs[0] = {bits == 8 ? static_cast<const void*>(&u8) : bits == 32 ? static_cast<const void*>(&sample) : &u16, 4};
  a.output = {&output, 4};
  CHECK(iris_execute_v1(plan.p, plan.c, &a, nullptr) == IRIS_OK);
  return output;
}
void near(float actual, float expected, float absolute_tolerance = 0) {
  if (!std::isfinite(actual) ||
      std::fabs(actual - expected) > std::max(absolute_tolerance, 2e-6f * std::max(1.0f, std::fabs(expected))))
    throw std::runtime_error("actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
}
void constants() {
  struct Case {
    const char* expression;
    float u8, u10, u16, f32, uv32;
  };
  const Case cases[] = {{"bitdepth", 8, 10, 16, 32, 32},
                        {"sbitdepth", 8, 8, 8, 8, 8},
                        {"ymin", 16, 64, 4096, 16.0f / 255, 16.0f / 255},
                        {"ymax", 235, 940, 60160, 235.0f / 255, 235.0f / 255},
                        {"cmin", 16, 64, 4096, -112.0f / 255, -112.0f / 255},
                        {"cmax", 240, 960, 61440, 112.0f / 255, 112.0f / 255},
                        {"range_size", 256, 1024, 65536, 1, 1},
                        {"range_min", 0, 0, 0, 0, -0.5f},
                        {"range_max", 255, 1023, 65535, 1, 0.5f},
                        {"range_half", 128, 512, 32768, 0.5f, 0},
                        {"yrange_min", 0, 0, 0, 0, 0},
                        {"yrange_max", 255, 1023, 65535, 1, 1},
                        {"yrange_half", 128, 512, 32768, 0.5f, 0.5f}};
  for (int optimize : {0, 1})
    for (const auto& item : cases) {
      CHECK(evaluate(item.expression, 8, environment(), optimize) == item.u8);
      CHECK(evaluate(item.expression, 10, environment(), optimize) == item.u10);
      CHECK(evaluate(item.expression, 16, environment(), optimize) == item.u16);
      CHECK(evaluate(item.expression, 32, environment(), optimize) == item.f32);
      CHECK(evaluate(item.expression, 32, environment(1), optimize) == item.uv32);
    }
  for (uint32_t bits : {8u, 10u, 12u, 14u, 16u}) {
    float normal = evaluate("cmin 2 *", bits, environment());
    CHECK(evaluate("cmin 2 *", bits, environment(0, IRIS_SCALE_FLOAT_UV)) == normal);
    CHECK(evaluate("cmin 2 *", bits, environment(1, IRIS_SCALE_FLOAT_UV)) == normal);
  }
  auto o = options(8, 1);
  o.input_count = 26;
  for (auto& f : o.inputs)
    f = {IRIS_U8, 8};
  o.inputs[25] = {IRIS_U16, 12};
  auto e = environment();
  Plan p;
  CHECK(iris_compile_expr_v1("range_max_w bitdepth_a +", &o, &e, &p.p, nullptr) == IRIS_OK);
  CHECK(iris_context_create(p.p, &p.c, nullptr) == IRIS_OK);
  iris_plan_info info{};
  CHECK(iris_plan_get_info(p.p, &info, nullptr) == IRIS_OK && info.input_mask == 0);
  float result = 0;
  iris_execute_args_v1 a{};
  a.struct_size = sizeof(a);
  a.input_count = 26;
  a.output = {&result, 4};
  CHECK(iris_execute_v1(p.p, p.c, &a, nullptr) == IRIS_OK && result == 4103);
}
void scaling() {
  for (int optimize : {0, 1}) {
    auto e = environment();
    CHECK(evaluate("32 scaleb i10", 8, e, optimize) == 32);
    CHECK(evaluate("i10 32 scaleb", 8, e, optimize) == 8);
    CHECK(evaluate("sbitdepth i16", 8, e, optimize) == 8);
    CHECK(evaluate("i16 sbitdepth", 8, e, optimize) == 16);
    CHECK(evaluate("i10 1 scaleb i8 1 scaleb +", 8, e, optimize) == 1.25f);
    CHECK(evaluate("128 scaleb", 16, e, optimize) == 32768);
    CHECK(evaluate("255 scalef", 16, e, optimize) == 65535);
    CHECK(evaluate("128 scaleb", 32, environment(1), optimize) == 0);
    near(evaluate("16 yscaleb", 32, environment(1), optimize), 16.0f / 255);
    near(evaluate("128 yscalef", 32, environment(1), optimize), 128.0f / 255);
    for (auto mode : {IRIS_SCALE_ALL, IRIS_SCALE_ALL_FULL, IRIS_SCALE_INT, IRIS_SCALE_INT_FULL, IRIS_SCALE_FLOAT,
                      IRIS_SCALE_FLOAT_FULL, IRIS_SCALE_FLOAT_UV, IRIS_SCALE_NONE})
      for (uint32_t bits : {8u, 10u, 12u, 14u, 16u, 32u})
        for (int chroma : {0, 1}) {
          auto env = environment(chroma, mode);
          float sample = bits == 32 ? 0.25f : 100.0f;
          // Centering 16-bit chroma and restoring its bias can lose low float bits.
          // Two ULPs at the largest 16-bit magnitude remain far below half an integer sample.
          float bound = bits == 32 ? 0.0f : 2 * std::numeric_limits<float>::epsilon() * float(1u << bits);
          near(evaluate("x", bits, env, optimize, sample), sample, bound);
          near(evaluate("x[0,0]", bits, env, optimize, sample), sample, bound);
        }
    auto shifted = environment(1, IRIS_SCALE_FLOAT_UV);
    CHECK(evaluate("range_min", 32, shifted, optimize) == -0.5f);
    CHECK(evaluate("range_max", 32, shifted, optimize) == 0.5f);
    CHECK(evaluate("range_half", 32, shifted, optimize) == 0);
    CHECK(evaluate("x 2 *", 32, shifted, optimize, 0.25f) == 1.0f);
    auto clamped = environment(1);
    clamped.clamp_float = 1;
    CHECK(evaluate("x", 32, clamped, optimize, -2) == -0.5f);
    CHECK(evaluate("x", 32, clamped, optimize, 2) == 0.5f);
    clamped.clamp_float_uv = 1;
    CHECK(evaluate("x", 32, clamped, optimize, -2) == 0);
    CHECK(evaluate("x", 32, clamped, optimize, 2) == 1);
    clamped.chroma = 0;
    CHECK(evaluate("x", 32, clamped, optimize, 2) == 1);
  }
}
void time_and_errors() {
  for (int optimize : {0, 1})
    for (uint64_t frames : {uint64_t(1), uint64_t(3), uint64_t(16777220), uint64_t(1) << 54}) {
      auto o = options(8, optimize);
      auto e = environment();
      e.frame_count = frames;
      Plan p;
      CHECK(iris_compile_expr_v1("time", &o, &e, &p.p, nullptr) == IRIS_OK);
      CHECK(iris_context_create(p.p, &p.c, nullptr) == IRIS_OK);
      iris_plan_info info{};
      CHECK(iris_plan_get_info(p.p, &info, nullptr) == IRIS_OK && info.metadata_mask == IRIS_DEP_TIME);
      float result = -1;
      iris_execute_args_v1 a{};
      a.struct_size = sizeof(a);
      a.input_count = 1;
      a.output = {&result, 4};
      for (auto frame : {frames - 1, uint64_t(0), frames / 2}) {
        a.frameno = frame;
        CHECK(iris_execute_v1(p.p, p.c, &a, nullptr) == IRIS_OK);
        CHECK(result == (frames == 1 ? 0.0f : float(double(frame) / double(frames - 1))));
      }
      a.frameno = frames;
      CHECK(iris_execute_v1(p.p, p.c, &a, nullptr) == IRIS_INVALID_ARGUMENT);
    }
  auto o = options(8, 1);
  auto e = environment();
  Plan p;
  for (const char* expression : {"bitdepth_w", "range_max_!", "scaleb", "1 time@", "1 cmin@"})
    CHECK(iris_compile_expr_v1(expression, &o, &e, &p.p, nullptr) == IRIS_PARSE_ERROR && !p.p);
  CHECK(iris_compile_expr_v1("time", &o, nullptr, &p.p, nullptr) == IRIS_INVALID_ARGUMENT && !p.p);
  e.struct_size--;
  CHECK(iris_compile_expr_v1("time", &o, &e, &p.p, nullptr) == IRIS_INVALID_ARGUMENT && !p.p);
  e = environment();
  e.frame_count = 0;
  CHECK(iris_compile_expr_v1("time", &o, &e, &p.p, nullptr) == IRIS_INVALID_ARGUMENT && !p.p);
  e = environment();
  e.chroma = 2;
  CHECK(iris_compile_expr_v1("time", &o, &e, &p.p, nullptr) == IRIS_INVALID_ARGUMENT && !p.p);
}
} // namespace
int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "llvm") == 0)
    backend = IRIS_BACKEND_LLVM;
  try {
    constants();
    scaling();
    time_and_errors();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  std::cout << "Expr context, ranges, scaling and time passed\n";
  return 0;
}
