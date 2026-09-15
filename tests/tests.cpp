#include <iris/iris.h>
#include "ir/ir.hpp"
#include "runtime/runtime.hpp"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
// Fault injection and a check that successful execution needs no operator new.
thread_local bool reject_allocation = false;
void* operator new(std::size_t n) {
  if (reject_allocation)
    throw std::bad_alloc();
  if (void* p = std::malloc(n ? n : 1))
    return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
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
#define CHECK(x)                                                                                                       \
  do {                                                                                                                 \
    if (!(x))                                                                                                          \
      throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #x);                      \
  } while (0)
iris_compile_options options(uint32_t w = 1, uint32_t h = 1, int optimize = 1) {
  iris_compile_options o{};
  o.width = w;
  o.height = h;
  o.input_count = 3;
  o.optimize = optimize;
  for (auto& f : o.inputs)
    f = {IRIS_F32, 32};
  o.output = {IRIS_F32, 32};
  return o;
}
iris_backend test_backend = IRIS_BACKEND_SCALAR;
struct Compiled {
  iris_plan* p = nullptr;
  iris_context* c = nullptr;
  Compiled(const std::string& s, iris_compile_options o = options(), int enable_lut = 0) {
    iris_diagnostic d{};
    auto status = iris_compile_ex(s.c_str(), &o, test_backend, enable_lut, &p, &d);
    if (status != IRIS_OK)
      throw std::runtime_error(s + ": " + d.message);
    CHECK(iris_context_create(p, &c, &d) == IRIS_OK);
    iris_backend backend;
    CHECK(iris_plan_get_backend(p, &backend, nullptr) == IRIS_OK);
    CHECK(backend == (info().strategy == IRIS_COMPUTE ? test_backend : IRIS_BACKEND_NONE));
  }
  ~Compiled() {
    iris_context_destroy(c);
    iris_plan_destroy(p);
  }
  iris_plan_info info() {
    iris_plan_info i{};
    CHECK(iris_plan_get_info(p, &i, nullptr) == IRIS_OK);
    return i;
  }
  float value(float x = 0, float y = 0, float z = 0) {
    iris_execute_args a{};
    a.inputs[0] = {&x, 4};
    a.inputs[1] = {&y, 4};
    a.inputs[2] = {&z, 4};
    float out = 0;
    a.output = {&out, 4};
    CHECK(iris_execute(p, c, &a, nullptr) == IRIS_OK);
    return out;
  }
};
bool same(float a, float b) {
  return (std::isnan(a) && std::isnan(b)) || (a == b && (a != 0 || std::signbit(a) == std::signbit(b)));
}
void numerical() {
  struct Case {
    const char* s;
    float want;
  };
  const Case cases[] = {{"+1.25e1 -.5 +", 12},
                        {"x y -", -1},
                        {"x y *", 6},
                        {"x y /", 2.0f / 3},
                        {"x y min", 2},
                        {"x y max", 3},
                        {"-4 abs sqrt", 2},
                        {"2 3 <", 1},
                        {"2 2 <=", 1},
                        {"2 2 =", 1},
                        {"2 2 ==", 1},
                        {"-1 2 &", 0},
                        {"-1 2 |", 1},
                        {"2 3 !=", 1},
                        {"3 2 >=", 1},
                        {"3 2 >", 1},
                        {"-1 2 and", 0},
                        {"-1 2 or", 1},
                        {"1 2 xor", 0},
                        {"-1 not", 1},
                        {"1 10 20 ?", 10},
                        {"0 10 20 ?", 20},
                        {"2 dup *", 4},
                        {"2 dup0 *", 4},
                        {"2 3 dup1 * +", 8},
                        {"2 3 5 dup2 + + +", 12},
                        {"2 3 swap1 -", 1},
                        {"2 3 5 swap2 - -", 4},
                        {"2 3 dup+1 * +", 8},
                        {"2 dup_name@ dup_name +", 4},
                        {"2 3 swap -", 1},
                        {"2 A@ A +", 4},
                        {"2 long_name^ long_name 3 +", 5},
                        {"2 A^ 3 A^ A", 3},
                        {"2 3 < 4 +", 5},
                        {"sx sy + width + height +", 2},
                        {"1 0 /", INFINITY},
                        {"-1 0 /", -INFINITY},
                        {"0 0 /", NAN},
                        {"-1 sqrt", 0.0f},
                        {"-1 0 / sqrt", 0.0f},
                        {"0 0 / sqrt", NAN},
                        {"0 0 / 1 min", NAN},
                        {"1 0 0 / max", NAN},
                        {"-0 0 min", 0.0f},
                        {"0 -0 min", 0.0f},
                        {"-0 0 max", 0.0f},
                        {"-0 -0 max", 0.0f},
                        {"-0 sqrt", 0.0f},
                        {"-0 1 min", 0.0f},
                        {"-0 -1 max", 0.0f},
                        {"1 -0 /", -INFINITY},
                        {"-0 abs", 0.0f},
                        {"5 1 9 clip", 5},
                        {"-1 1 9 clip", 1},
                        {"12 1 9 clip", 9},
                        {"5 9 1 clip", 9},
                        {"-0 -0 1 clip", 0.0f},
                        {"0 0 / 1 9 clip", NAN},
                        {"0 0 / not", 1},
                        {"0 0 / 0 !=", 1},
                        {"0 0 / 0 =", 0},
                        {"0 0 / 10 20 ?", 20},
                        {"1 2 0 0 / ?", 2}};
  for (int optimize = 0; optimize < 2; ++optimize)
    for (auto t : cases) {
      Compiled p(t.s, options(1, 1, optimize));
      if (!same(p.value(2, 3), t.want))
        throw std::runtime_error(t.s);
    }
  for (int optimize = 0; optimize < 2; ++optimize) {
    auto o = options(7, 1, optimize);
    o.output = {IRIS_U8, 8};
    Compiled p("x", o);
    float in[] = {NAN, -INFINITY, -1, .499f, .5f, 254.5f, INFINITY};
    uint8_t out[7]{};
    iris_execute_args a{};
    a.inputs[0] = {in, 28};
    a.output = {out, 7};
    CHECK(iris_execute(p.p, p.c, &a, nullptr) == IRIS_OK);
    const uint8_t want[] = {0, 0, 0, 0, 1, 255, 255};
    CHECK(std::memcmp(out, want, 7) == 0);
  }
}
void sqrt_boundaries() {
  const float input[] = {-4.0f,    -INFINITY, -std::numeric_limits<float>::denorm_min(), -0.0f, 0.0f, 1.0f, 4.0f,
                         INFINITY, NAN};
  const float expected[] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 2.0f, INFINITY, NAN};
  for (int optimize = 0; optimize < 2; ++optimize) {
    auto o = options(17, 3, optimize);
    Compiled p("x sqrt", o);
    float pixels[51], output[51];
    for (size_t i = 0; i < 51; ++i)
      pixels[i] = input[i % 9];
    iris_execute_args args{};
    args.inputs[0] = {pixels, 17 * sizeof(float)};
    args.output = {output, 17 * sizeof(float)};
    CHECK(iris_execute(p.p, p.c, &args, nullptr) == IRIS_OK);
    for (size_t i = 0; i < 51; ++i)
      CHECK(same(output[i], expected[i % 9]));
    o = options(256, 1, optimize);
    o.input_count = 1;
    o.inputs[0] = {IRIS_U8, 8};
    Compiled lut("x 128 - sqrt", o, 1);
    CHECK(lut.info().strategy == IRIS_LUT_U8);
    uint8_t indices[256];
    float table_output[256];
    for (int i = 0; i < 256; ++i)
      indices[i] = static_cast<uint8_t>(i);
    args.inputs[0] = {indices, sizeof indices};
    args.output = {table_output, sizeof table_output};
    CHECK(iris_execute(lut.p, lut.c, &args, nullptr) == IRIS_OK);
    for (int i = 0; i < 256; ++i)
      CHECK(same(table_output[i], i <= 128 ? 0.0f : std::sqrt(float(i - 128))));
  }
}
void zero_results() {
  for (int optimize = 0; optimize < 2; ++optimize) {
    for (const char* expression : {"x y min", "x y max"}) {
      Compiled p(expression, options(1, 1, optimize));
      for (float x : {-0.0f, 0.0f})
        for (float y : {-0.0f, 0.0f})
          CHECK(same(p.value(x, y), 0.0f));
      CHECK(std::isnan(p.value(NAN, -0.0f)));
      CHECK(std::isnan(p.value(-0.0f, NAN)));
    }
    CHECK(same(Compiled("x y min", options(1, 1, optimize)).value(-0.0f, 1), 0.0f));
    CHECK(same(Compiled("x y max", options(1, 1, optimize)).value(-0.0f, -1), 0.0f));
    CHECK(same(Compiled("x", options(1, 1, optimize)).value(-0.0f), -0.0f));
    CHECK(same(Compiled("1 x /", options(1, 1, optimize)).value(-0.0f), -INFINITY));
    auto o = options(256, 1, optimize);
    o.input_count = 1;
    o.inputs[0] = {IRIS_U8, 8};
    for (const char* expression : {"x -0 * 1 min", "x -0 * -1 max", "x -0 * sqrt"}) {
      Compiled lut(expression, o, 1);
      CHECK(lut.info().strategy == IRIS_LUT_U8);
      uint8_t input[256];
      float output[256];
      for (int i = 0; i < 256; ++i)
        input[i] = static_cast<uint8_t>(i);
      iris_execute_args args{};
      args.inputs[0] = {input, sizeof input};
      args.output = {output, sizeof output};
      CHECK(iris_execute(lut.p, lut.c, &args, nullptr) == IRIS_OK);
      for (float value : output)
        CHECK(same(value, 0.0f));
    }
  }
}
void unary_math() {
  struct Case {
    const char* op;
    float x, expected;
  };
  const Case cases[] = {
      {"neg", 2, -2},          {"neg", 0.0f, -0.0f},    {"neg", -0.0f, 0.0f},   {"sgn", -2, -1},
      {"sgn", 2, 1},           {"sgn", -0.0f, 0.0f},    {"sgn", NAN, 0},        {"round", 2.5f, 3},
      {"round", -2.5f, -3},    {"round", -.25f, -0.0f}, {"floor", -2.5f, -3},   {"floor", 2.5f, 2},
      {"floor", -0.0f, -0.0f}, {"ceil", -2.5f, -2},     {"ceil", 2.5f, 3},      {"ceil", -.25f, -0.0f},
      {"trunc", -2.5f, -2},    {"trunc", 2.5f, 2},      {"trunc", -.25f, -0.0f}};
  for (int optimize = 0; optimize < 2; ++optimize) {
    for (const auto& item : cases) {
      Compiled p(std::string("x ") + item.op, options(1, 1, optimize));
      CHECK(same(p.value(item.x), item.expected));
      // Literal spellings preserve -0; NaN is generated by an expression.
      std::string literal = std::isnan(item.x)                    ? "0 0 /"
                            : item.x == 0 && std::signbit(item.x) ? "-0"
                                                                  : std::to_string(item.x);
      CHECK(same(Compiled(literal + " " + item.op, options(1, 1, optimize)).value(), item.expected));
    }
    for (const char* op : {"round", "floor", "ceil", "trunc"}) {
      Compiled p(std::string("x ") + op, options(1, 1, optimize));
      CHECK(same(p.value(INFINITY), INFINITY));
      CHECK(same(p.value(-INFINITY), -INFINITY));
      CHECK(std::isnan(p.value(NAN)));
    }
  }
}
void libm_math() {
  for (int optimize = 0; optimize < 2; ++optimize) {
    Compiled clip("x y z clip", options(1, 1, optimize));
    CHECK(same(clip.value(5, 1, 9), 5));
    CHECK(same(clip.value(-1, 1, 9), 1));
    CHECK(same(clip.value(12, 1, 9), 9));
    CHECK(same(clip.value(5, 9, 1), 9));
    CHECK(same(clip.value(-0.0f, -0.0f, 1), 0.0f));
    CHECK(std::isnan(clip.value(NAN, 1, 9)));
    CHECK(std::isnan(clip.value(5, NAN, 9)));
    CHECK(std::isnan(clip.value(5, 1, NAN)));
  }
  struct Case {
    const char* literal;
    const char* runtime;
    float x, y, expected;
  };
  const Case cases[] = {{"0 exp", "x exp", 0, 0, 1},          {"1 log", "x log", 1, 0, 0},
                        {"0 log", "x log", 0, 0, -INFINITY},  {"-1 log", "x log", -1, 0, NAN},
                        {"0 sin", "x sin", 0, 0, 0},          {"0 cos", "x cos", 0, 0, 1},
                        {"-0 tan", "x tan", -0.0f, 0, -0.0f}, {"0 asin", "x asin", 0, 0, 0},
                        {"1 acos", "x acos", 1, 0, 0},        {"0 atan", "x atan", 0, 0, 0},
                        {"2 asin", "x asin", 2, 0, NAN},      {"-2 acos", "x acos", -2, 0, NAN},
                        {"-5 2 %", "x y %", -5, 2, -1},       {"5 -2 %", "x y %", 5, -2, 1},
                        {"2 0 %", "x y %", 2, 0, NAN},        {"-2 3 pow", "x y pow", -2, 3, -8},
                        {"2 3 ^", "x y ^", 2, 3, 8},          {"-2 0.5 pow", "x y pow", -2, .5f, NAN},
                        {"0 1 atan2", "x y atan2", 0, 1, 0},  {"-0 1 atan2", "x y atan2", -0.0f, 1, -0.0f}};
  for (int optimize = 0; optimize < 2; ++optimize)
    for (const auto& item : cases) {
      CHECK(same(Compiled(item.literal, options(1, 1, optimize)).value(), item.expected));
      CHECK(same(Compiled(item.runtime, options(1, 1, optimize)).value(item.x, item.y), item.expected));
    }
  CHECK(same(Compiled("pi").value(), 3.14159265358979323846f));
}
void errors() {
  for (const char* expression : {"dup0", "1 swap1", "1 dup1", "1 2 swap0", "1 dup-1", "1 dup+", "1 dup4294967296",
                                 "1 dup1tail", "1 2 swap-1", "1 2 swap4294967295"}) {
    auto config = options();
    iris_plan* invalid = nullptr;
    iris_diagnostic error{};
    CHECK(iris_compile(expression, &config, &invalid, &error) == IRIS_PARSE_ERROR);
    CHECK(!invalid && error.length > 0 && error.message[0]);
  }
  const char* bad[] = {"",
                       "+",
                       "1 2",
                       "x +",
                       "what",
                       "A",
                       "x@",
                       "1 sx@",
                       "x[1]",
                       "x[1,]",
                       "x[+-1,0]",
                       "x[1,--2]",
                       "x[2147483648,0]",
                       "x[-2147483648,0]",
                       "x.Bad!",
                       "1e99",
                       "0x10",
                       "nan",
                       "inf",
                       "x 1 sin",
                       "1e-99",
                       "1 swap"};
  auto o = options();
  iris_diagnostic d{};
  for (auto s : bad) {
    iris_plan* p = reinterpret_cast<iris_plan*>(uintptr_t(1));
    CHECK(iris_compile(s, &o, &p, &d) == IRIS_PARSE_ERROR);
    CHECK(p == nullptr);
    CHECK(d.message[0]);
    CHECK(d.offset <= std::strlen(s));
  }
  iris_plan* p = nullptr;
  o.input_count = 1;
  CHECK(iris_compile("y", &o, &p, &d) == IRIS_PARSE_ERROR);
  CHECK(iris_compile("x bad", &o, &p, &d) == IRIS_PARSE_ERROR);
  CHECK(d.offset == 2 && d.length == 3);
  std::string long_source(65537, ' ');
  CHECK(iris_compile(long_source.c_str(), &o, &p, &d) == IRIS_LIMIT_EXCEEDED);
  std::string deep = "1";
  for (int i = 0; i < 8192; ++i)
    deep += " 1 +";
  CHECK(iris_compile(deep.c_str(), &o, &p, &d) == IRIS_LIMIT_EXCEEDED);
  std::string stack = "1";
  for (int i = 0; i < 8192; ++i)
    stack += " dup";
  CHECK(iris_compile(stack.c_str(), &o, &p, &d) == IRIS_LIMIT_EXCEEDED);
  std::string variables = "1";
  for (int i = 0; i <= 8192; ++i)
    variables += " v" + std::to_string(i) + "@";
  CHECK(variables.size() < 65536);
  CHECK(iris_compile(variables.c_str(), &o, &p, &d) == IRIS_LIMIT_EXCEEDED);
  std::string boundary = "1" + std::string(65535, ' ');
  {
    Compiled okay(boundary);
    CHECK(okay.value() == 1);
  }
  o.width = 0;
  CHECK(iris_compile("1", &o, &p, &d) == IRIS_INVALID_ARGUMENT);
  o = options();
  o.inputs[0].bits = 8;
  CHECK(iris_compile("1", &o, &p, &d) == IRIS_INVALID_ARGUMENT);
  CHECK(iris_compile(nullptr, &o, &p, &d) == IRIS_INVALID_ARGUMENT);
  CHECK(iris_compile("1", &o, nullptr, &d) == IRIS_INVALID_ARGUMENT);
  iris_context* c = reinterpret_cast<iris_context*>(uintptr_t(1));
  CHECK(iris_context_create(nullptr, &c, &d) == IRIS_INVALID_ARGUMENT && c == nullptr);
  iris_plan_destroy(nullptr);
  iris_context_destroy(nullptr);
  CHECK(iris_plan_get_info(nullptr, nullptr, nullptr) == IRIS_INVALID_ARGUMENT);
  CHECK(iris_execute(nullptr, nullptr, nullptr, nullptr) == IRIS_INVALID_ARGUMENT);
  Compiled good("x"), other("x");
  float in = 2, out = 9;
  iris_execute_args a{};
  a.inputs[0] = {&in, 4};
  a.output = {&out, 4};
  CHECK(iris_execute(good.p, other.c, &a, &d) == IRIS_INVALID_ARGUMENT);
  CHECK(out == 9);
  CHECK(good.value(6) == 6);
  o = options();
  reject_allocation = true;
  auto status = iris_compile("x", &o, &p, &d);
  reject_allocation = false;
  CHECK(status == IRIS_OUT_OF_MEMORY && p == nullptr);
  reject_allocation = true;
  status = iris_context_create(good.p, &c, &d);
  reject_allocation = false;
  CHECK(status == IRIS_OUT_OF_MEMORY && c == nullptr);
  iris::IR invalid;
  invalid.nodes.push_back({});
  invalid.nodes[0].op = iris::Op::Add;
  bool rejected = false;
  try {
    iris::verify(invalid);
  } catch (const iris::Error&) {
    rejected = true;
  }
  CHECK(rejected);
  invalid.nodes[0].op = iris::Op::Constant;
  invalid.nodes.push_back({});
  invalid.nodes[1].op = iris::Op::Not;
  invalid.nodes[1].type = iris::Type::Bool;
  rejected = false;
  try {
    iris::verify(invalid);
  } catch (const iris::Error&) {
    rejected = true;
  }
  CHECK(rejected);
}
void strategies_and_allocation() {
  for (auto type : {IRIS_U8, IRIS_U16, IRIS_F32})
    for (const char* expr : {"x", "42", "x 1 +"}) {
      auto o = options(3, 2);
      o.inputs[0] = o.output = {type, type == IRIS_U8 ? 8u : type == IRIS_U16 ? 16u : 32u};
      Compiled p(expr, o);
      const size_t size = type == IRIS_U8 ? 1 : type == IRIS_U16 ? 2 : 4;
      std::vector<uint8_t> in(40, 0xa5), out(40, 0xcd);
      const ptrdiff_t pitch = ptrdiff_t(3 * size + 3);
      auto* src = in.data() + 1 + pitch;
      auto* dst = out.data() + 1 + pitch;
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) {
          float f = float(10 * y + x);
          uint16_t u = uint16_t(f);
          auto* at = src - y * pitch + x * size;
          if (type == IRIS_F32)
            std::memcpy(at, &f, 4);
          else if (type == IRIS_U16)
            std::memcpy(at, &u, 2);
          else
            *at = uint8_t(u);
        }
      const auto saved = in;
      iris_execute_args a{};
      a.inputs[0] = {src, -pitch};
      a.output = {dst, -pitch};
      reject_allocation = true;
      auto status = iris_execute(p.p, p.c, &a, nullptr);
      reject_allocation = false;
      CHECK(status == IRIS_OK);
      CHECK(in == saved);
      std::vector<bool> written(out.size());
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) {
          const auto* at = dst - y * pitch + x * size;
          float v = 0;
          if (type == IRIS_F32)
            std::memcpy(&v, at, 4);
          else if (type == IRIS_U16) {
            uint16_t u;
            std::memcpy(&u, at, 2);
            v = u;
          } else
            v = *at;
          CHECK(v == (std::strcmp(expr, "42") == 0 ? 42 : 10 * y + x + (std::strcmp(expr, "x") == 0 ? 0 : 1)));
          for (size_t b = 0; b < size; ++b)
            written[size_t(at - out.data()) + b] = true;
        }
      for (size_t i = 0; i < out.size(); ++i)
        if (!written[i])
          CHECK(out[i] == 0xcd);
    }
  Compiled p("x");
  uint32_t payload = 0x7fc12345, out = 0;
  iris_execute_args a{};
  a.inputs[0] = {&payload, 4};
  a.output = {&out, 4};
  CHECK(iris_execute(p.p, p.c, &a, nullptr) == IRIS_OK && out == payload);
  Compiled edge("x[-2147483647,+2147483647]");
  CHECK(edge.value(7) == 7);
}
void layout() {
  for (bool negative : {false, true})
    for (uint32_t width : {1u, 3u, 7u, 15u, 16u, 17u, 31u}) {
      const uint32_t h = 3;
      const ptrdiff_t pitch = ptrdiff_t(width * 2 + 5), opitch = ptrdiff_t(width * 4 + 7);
      std::vector<uint8_t> in(size_t(pitch) * h + 8, 0xa5), out(size_t(opitch) * h + 8, 0xcd);
      auto* base = in.data() + 1 + (negative ? (h - 1) * pitch : 0);
      auto* dest = out.data() + 1 + (negative ? (h - 1) * opitch : 0);
      ptrdiff_t stride = negative ? -pitch : pitch, ostride = negative ? -opitch : opitch;
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < width; ++x) {
          uint16_t v = uint16_t(10 * y + x);
          std::memcpy(base + y * stride + x * 2, &v, 2);
        }
      const auto original = in;
      auto o = options(width, h);
      o.inputs[0] = {IRIS_U16, 10};
      Compiled p("x[1,0] x[-1,0] - x[0,-1] + sy +", o);
      iris_execute_args a{};
      a.inputs[0] = {base, stride};
      a.output = {dest, ostride};
      CHECK(iris_execute(p.p, p.c, &a, nullptr) == IRIS_OK);
      CHECK(in == original);
      std::vector<bool> written(out.size());
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < width; ++x) {
          float v;
          auto* address = dest + y * ostride + x * 4;
          std::memcpy(&v, address, 4);
          float want = float(std::min(x + 1, width - 1) - (x ? x - 1 : 0) + (y ? y - 1 : 0) * 10 + x + y);
          CHECK(v == want);
          for (int b = 0; b < 4; ++b)
            written[size_t(address - out.data()) + b] = true;
        }
      for (size_t i = 0; i < out.size(); ++i)
        if (!written[i])
          CHECK(out[i] == 0xcd);
      a.output.stride = 1;
      CHECK(iris_execute(p.p, p.c, &a, nullptr) == IRIS_INVALID_ARGUMENT);
      a.output.stride = PTRDIFF_MIN;
      CHECK(iris_execute(p.p, p.c, &a, nullptr) == IRIS_INVALID_ARGUMENT);
    }
  auto o = options();
  o.inputs[0] = {IRIS_U8, 8};
  o.inputs[1] = {IRIS_U16, 12};
  o.output = {IRIS_U16, 10};
  Compiled p("x y + z +", o);
  uint8_t x = 2;
  uint16_t y = 1000, result = 0;
  float z = 30;
  iris_execute_args a{};
  a.inputs[0] = {&x, 1};
  a.inputs[1] = {&y, 2};
  a.inputs[2] = {&z, 4};
  a.output = {&result, 2};
  CHECK(iris_execute(p.p, p.c, &a, nullptr) == IRIS_OK && result == 1023);
  for (unsigned bits = 9; bits <= 16; ++bits) {
    o.output = {IRIS_U16, bits};
    Compiled q("1 0 /", o);
    CHECK(iris_execute(q.p, q.c, &a, nullptr) == IRIS_OK);
    CHECK(result == ((1u << bits) - 1));
  }
  o = options(2, 2);
  Compiled q("x", o);
  float buf[4]{};
  a = {};
  a.output = {buf, 8};
  a.inputs[0] = {reinterpret_cast<void*>(UINTPTR_MAX - 1), 8};
  CHECK(iris_execute(q.p, q.c, &a, nullptr) == IRIS_INVALID_ARGUMENT);
  a.inputs[0] = {reinterpret_cast<void*>(uintptr_t(1)), -8};
  CHECK(iris_execute(q.p, q.c, &a, nullptr) == IRIS_INVALID_ARGUMENT);
}
void dependencies() {
  for (int optimize = 0; optimize < 2; ++optimize) {
    Compiled p("x.Unused A^ y[1,0] B^ frameno C^ x.Gain x.Gain + frameno +", options(1, 1, optimize));
    auto info = p.info();
    CHECK(info.property_count == size_t(optimize ? 1 : 2));
    CHECK(info.input_mask == unsigned(optimize ? 0 : 2));
    CHECK(info.has_relative_access == !optimize);
    CHECK(info.metadata_mask == IRIS_DEP_FRAMENO);
    iris_execute_args a{};
    float out = 0, y = 9;
    a.inputs[1] = {&y, 4};
    a.output = {&out, 4};
    std::vector<float> props(info.property_count);
    for (size_t i = 0; i < props.size(); ++i) {
      iris_property_dependency dep{};
      CHECK(iris_plan_get_property(p.p, i, &dep, nullptr) == IRIS_OK);
      CHECK(dep.input == 0);
      props[i] = std::strcmp(dep.name, "Gain") == 0 ? 3 : 99;
    }
    a.properties = props.data();
    a.property_count = props.size();
    CHECK(iris_execute(p.p, p.c, &a, nullptr) == IRIS_OK && out == 6);
    a.frameno = 10;
    CHECK(iris_execute(p.p, p.c, &a, nullptr) == IRIS_OK && out == 16);
    a.property_count = 0;
    CHECK(iris_execute(p.p, p.c, &a, nullptr) == IRIS_INVALID_ARGUMENT);
    size_t needed = 0;
    CHECK(iris_plan_dump(p.p, nullptr, 0, &needed, nullptr) == IRIS_OK);
    std::vector<char> text(needed);
    CHECK(iris_plan_dump(p.p, text.data(), needed - 1, &needed, nullptr) == IRIS_INVALID_ARGUMENT);
    CHECK(iris_plan_dump(p.p, text.data(), text.size(), &needed, nullptr) == IRIS_OK);
    CHECK(std::strstr(text.data(), "return %"));
    CHECK(iris_plan_get_property(p.p, props.size(), nullptr, nullptr) == IRIS_INVALID_ARGUMENT);
  }
  Compiled fill("2 3 +"), copy("x"), scalar("x", options(1, 1, 0));
  CHECK(fill.info().strategy == IRIS_FILL && copy.info().strategy == IRIS_COPY &&
        scalar.info().strategy == IRIS_COMPUTE);
  CHECK(copy.info().input_mask == 1);
  CHECK(same(copy.value(-0.0f), -0.0f));
  auto o = options();
  o.input_count = 0;
  Compiled no_input("42", o);
  CHECK(no_input.value() == 42);
  std::string source = "x 2 +";
  Compiled owned(source);
  source.assign(1000, '!');
  CHECK(owned.value(4) == 6);
}
std::string expression(std::mt19937& rng, int depth) {
  if (!depth) {
    const char* leaves[] = {"x", "y", "z", "0", "-0", "1", "-1", "0.5", "1e20"};
    return leaves[rng() % 9];
  }
  unsigned op = rng() % 4;
  if (op == 0) {
    const char* unary[] = {" abs", " sqrt", " not", " neg", " sgn", " round", " floor", " ceil", " trunc",
                           " exp", " log",  " sin", " cos", " tan", " asin",  " acos",  " atan"};
    return expression(rng, depth - 1) + unary[rng() % (sizeof unary / sizeof *unary)];
  }
  if (op == 1) {
    auto a = expression(rng, depth - 1), b = expression(rng, depth - 1), c = expression(rng, depth - 1);
    return a + " " + b + " " + c + " ?";
  }
  const char* binary[] = {"+",  "-",  "*", "/",   "min", "max", "<", "<=",  "=",
                          "!=", ">=", ">", "and", "or",  "xor", "%", "pow", "atan2"};
  auto a = expression(rng, depth - 1), b = expression(rng, depth - 1);
  return a + " " + b + " " + binary[rng() % (sizeof binary / sizeof *binary)];
}
void differential() {
  std::mt19937 rng(0x1a15);
  const float values[] = {-INFINITY, -10, -0.0f, 0, .5f, 2, 1e20f, INFINITY, NAN};
  for (int i = 0; i < 250; ++i) {
    auto s = expression(rng, 4);
    Compiled a(s), b(s, options(1, 1, 0));
    auto backend = test_backend;
    test_backend = IRIS_BACKEND_SCALAR;
    Compiled reference(s, options(1, 1, 0));
    test_backend = backend;
    for (int j = 0; j < 15; ++j) {
      float x = values[rng() % 9], y = values[rng() % 9], z = values[rng() % 9];
      CHECK(same(a.value(x, y, z), b.value(x, y, z)));
      CHECK(same(a.value(x, y, z), reference.value(x, y, z)));
    }
  }
}
void concurrency() {
  Compiled shared("x 2 * frameno +");
  std::atomic<bool> okay{true};
  std::vector<std::thread> workers;
  std::vector<float> serial(8 * 200);
  for (int t = 0; t < 8; ++t)
    for (int j = 0; j < 200; ++j) {
      float in = float(j);
      iris_execute_args a{};
      a.inputs[0] = {&in, 4};
      a.output = {&serial[t * 200 + j], 4};
      a.frameno = uint64_t(t);
      CHECK(iris_execute(shared.p, shared.c, &a, nullptr) == IRIS_OK);
    }
  for (int thread = 0; thread < 8; ++thread)
    workers.emplace_back([&, thread] {
      iris_context* c = nullptr;
      if (iris_context_create(shared.p, &c, nullptr) != IRIS_OK) {
        okay = false;
        return;
      }
      for (int j = 0; j < 200; ++j) {
        float in = float(j), out = 0;
        iris_execute_args a{};
        a.inputs[0] = {&in, 4};
        a.output = {&out, 4};
        a.frameno = uint64_t(thread);
        if (iris_execute(shared.p, c, &a, nullptr) != IRIS_OK || out != serial[thread * 200 + j] ||
            out != j * 2 + thread || in != j)
          okay = false;
      }
      iris_context_destroy(c);
    });
  for (auto& t : workers)
    t.join();
  CHECK(okay);
  for (int i = 0; i < 200; ++i) {
    Compiled p("x 2 *");
    CHECK(p.value(float(i)) == i * 2);
  }
  iris_plan_destroy(shared.p);
  shared.p = nullptr; // context can be destroyed after its plan handle
}
void frontend_independence() {
  iris::Program p;
  p.options = options();
  iris::Node input;
  input.op = iris::Op::Input;
  iris::Node constant;
  constant.value = 2;
  iris::Node multiply;
  multiply.op = iris::Op::Mul;
  multiply.args = {0, 1, 0};
  p.ir.nodes = {input, constant, multiply};
  p.ir.result = 2;
  iris::verify(p.ir);
  iris::optimize(p.ir);
  iris::verify(p.ir);
  iris::describe(p);
  std::vector<float> scratch(p.ir.nodes.size());
  float in = 7, out = 0;
  iris_execute_args a{};
  a.inputs[0] = {&in, 4};
  a.output = {&out, 4};
  iris::validate_execution(p, a);
  iris::run(p, scratch, a);
  CHECK(out == 14);
}
void lut_tables() {
  for (auto format : {iris_format{IRIS_U8, 8}, iris_format{IRIS_U16, 10}, iris_format{IRIS_F32, 32}}) {
    auto o = options(257, 2);
    o.input_count = 1;
    o.inputs[0] = {IRIS_U8, 8};
    o.output = format;
    const size_t bytes = format.type == IRIS_U8 ? 1 : format.type == IRIS_U16 ? 2 : 4;
    const ptrdiff_t pitch = ptrdiff_t(257 * bytes + 5), input_pitch = 262;
    std::vector<uint8_t> input(size_t(input_pitch) * 2 + 2, 0xa5), output(size_t(pitch) * 2 + 2, 0xcd), ref = output;
    auto* src = input.data() + 1 + input_pitch;
    auto* dst = output.data() + 1 + pitch;
    auto* expected = ref.data() + 1 + pitch;
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 257; ++x)
        src[-y * input_pitch + x] = uint8_t(x);
    const auto original = input;
    const char* s = "x 2 * 0.5 +";
    iris_plan* p = nullptr;
    iris_context* c = nullptr;
    iris_diagnostic d{};
    CHECK(iris_compile_ex(s, &o, test_backend, 1, &p, &d) == IRIS_OK);
    CHECK(iris_context_create(p, &c, &d) == IRIS_OK);
    iris_plan_info info{};
    iris_backend backend;
    CHECK(iris_plan_get_info(p, &info, &d) == IRIS_OK && info.strategy == IRIS_LUT_U8);
    CHECK(iris_plan_get_backend(p, &backend, &d) == IRIS_OK && backend == IRIS_BACKEND_NONE);
    iris_execute_args a{};
    a.inputs[0] = {src, -input_pitch};
    a.output = {dst, -pitch};
    reject_allocation = true;
    auto status = iris_execute(p, c, &a, &d);
    reject_allocation = false;
    CHECK(status == IRIS_OK);
    Compiled compute(s, o);
    a.output.data = expected;
    CHECK(iris_execute(compute.p, compute.c, &a, &d) == IRIS_OK);
    CHECK(output == ref && input == original); // includes padding sentinels
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 257; ++x) {
        auto* at = dst - y * pitch + x * bytes;
        float v;
        if (format.type == IRIS_U8)
          v = *at;
        else if (format.type == IRIS_U16) {
          uint16_t u;
          std::memcpy(&u, at, 2);
          v = u;
        } else
          std::memcpy(&v, at, 4);
        float want = format.type == IRIS_F32 ? float(uint8_t(x) * 2) + 0.5f : float(uint8_t(x) * 2 + 1);
        if (format.type == IRIS_U8)
          want = std::min(255.0f, want);
        CHECK(v == want);
      }
    iris_plan_destroy(p);
    iris_context_destroy(c);
  }
  for (const char* s : {"x sx +", "x sy +", "x width +", "x height +", "x frameno +", "x x.Gain +", "x[1,0] 2 *"}) {
    auto o = options();
    o.input_count = 1;
    o.inputs[0] = {IRIS_U8, 8};
    iris_plan* p = nullptr;
    iris_plan_info info{};
    CHECK(iris_compile_ex(s, &o, test_backend, 1, &p, nullptr) == IRIS_OK);
    CHECK(iris_plan_get_info(p, &info, nullptr) == IRIS_OK && info.strategy == IRIS_COMPUTE);
    iris_plan_destroy(p);
  }
  for (const char* s : {"42", "x"}) {
    auto o = options();
    o.input_count = 1;
    o.inputs[0] = o.output = {IRIS_U8, 8};
    iris_plan* p = nullptr;
    iris_plan_info info{};
    CHECK(iris_compile_ex(s, &o, test_backend, 1, &p, nullptr) == IRIS_OK);
    CHECK(iris_plan_get_info(p, &info, nullptr) == IRIS_OK && info.strategy == (s[0] == 'x' ? IRIS_COPY : IRIS_FILL));
    iris_plan_destroy(p);
  }
  // Table generation must preserve NaN/Inf, not just ordinary integer outputs.
  auto o = options(256, 1);
  o.input_count = 1;
  o.inputs[0] = {IRIS_U8, 8};
  iris_plan* p = nullptr;
  iris_context* c = nullptr;
  uint8_t input[256];
  float out[256];
  for (int i = 0; i < 256; ++i)
    input[i] = uint8_t(i);
  CHECK(iris_compile_ex("x 0 /", &o, test_backend, 1, &p, nullptr) == IRIS_OK);
  CHECK(iris_context_create(p, &c, nullptr) == IRIS_OK);
  iris_execute_args a{};
  a.inputs[0] = {input, 256};
  a.output = {out, 1024};
  CHECK(iris_execute(p, c, &a, nullptr) == IRIS_OK);
  CHECK(std::isnan(out[0]));
  for (int i = 1; i < 256; ++i)
    CHECK(out[i] == INFINITY);
  iris_context_destroy(c);
  iris_plan_destroy(p);
  o = options();
  o.input_count = 1;
  o.inputs[0] = {IRIS_U8, 8};
  CHECK(iris_compile_ex("x 2 * 0.5 +", &o, test_backend, 1, &p, nullptr) == IRIS_OK);
  std::atomic<bool> okay{true};
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t)
    workers.emplace_back([&] {
      iris_context* context = nullptr;
      if (iris_context_create(p, &context, nullptr) != IRIS_OK) {
        okay = false;
        return;
      }
      for (int i = 0; i < 256; ++i) {
        uint8_t v = uint8_t(i);
        float result;
        iris_execute_args args{};
        args.inputs[0] = {&v, 1};
        args.output = {&result, 4};
        if (iris_execute(p, context, &args, nullptr) != IRIS_OK || result != i * 2 + .5f)
          okay = false;
      }
      iris_context_destroy(context);
    });
  for (auto& worker : workers)
    worker.join();
  CHECK(okay);
  iris_plan_destroy(p);
}
void backend_contract() {
  auto o = options();
  iris_plan* p = nullptr;
  iris_diagnostic d{};
  CHECK(iris_compile_ex("x", &o, IRIS_BACKEND_NONE, 0, &p, &d) == IRIS_INVALID_ARGUMENT && !p);
  CHECK(iris_compile_ex("x", &o, IRIS_BACKEND_SCALAR, 2, &p, &d) == IRIS_INVALID_ARGUMENT && !p);
  if (!iris::llvm_available()) {
    CHECK(iris_compile_ex("x", &o, IRIS_BACKEND_LLVM, 0, &p, &d) == IRIS_BACKEND_UNAVAILABLE && !p);
  }
  std::atomic<bool> okay{true};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&] {
      try {
        for (int j = 0; j < 10; ++j) {
          Compiled c("x 2 * 1 +");
          if (c.value(float(j)) != j * 2 + 1)
            okay = false;
        }
      } catch (...) {
        okay = false;
      }
    });
  for (auto& t : threads)
    t.join();
  CHECK(okay);
}
void guard_pages() {
#ifdef _WIN32
  struct Guarded {
    unsigned char *allocation = nullptr, *data = nullptr;
    Guarded(size_t bytes, bool at_end) {
      SYSTEM_INFO system;
      GetSystemInfo(&system);
      const size_t page = system.dwPageSize;
      CHECK(bytes <= page);
      allocation = static_cast<unsigned char*>(VirtualAlloc(nullptr, 3 * page, MEM_RESERVE, PAGE_NOACCESS));
      CHECK(allocation);
      if (!VirtualAlloc(allocation + page, page, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        allocation = nullptr;
        CHECK(false);
      }
      data = allocation + page + (at_end ? page - bytes : 0);
    }
    ~Guarded() {
      if (allocation)
        VirtualFree(allocation, 0, MEM_RELEASE);
    }
  };
  for (bool end : {false, true})
    for (unsigned w : {1u, 3u, 7u, 15u, 16u, 17u, 31u})
      for (auto format : {iris_format{IRIS_U8, 8}, iris_format{IRIS_U16, 16}, iris_format{IRIS_F32, 32}}) {
        size_t bytes = iris::sample_bytes(format);
        Guarded in(w * bytes, end), out(w * 4, end);
        for (unsigned x = 0; x < w; ++x) {
          float f = float(x);
          uint16_t u = uint16_t(x);
          if (bytes == 1)
            in.data[x] = uint8_t(x);
          else if (bytes == 2)
            std::memcpy(in.data + x * 2, &u, 2);
          else
            std::memcpy(in.data + x * 4, &f, 4);
        }
        auto o = options(w, 1);
        o.inputs[0] = format;
        Compiled p("x[-1,0] x[1,0] +", o);
        iris_execute_args a{};
        a.inputs[0] = {in.data, ptrdiff_t(w * bytes)};
        a.output = {out.data, ptrdiff_t(w * 4)};
        CHECK(iris_execute(p.p, p.c, &a, nullptr) == IRIS_OK);
        for (unsigned x = 0; x < w; ++x) {
          float value;
          std::memcpy(&value, out.data + x * 4, 4);
          CHECK(value == float((x ? x - 1 : 0) + std::min(x + 1, w - 1)));
        }
      }
#endif
}
void normalized_coordinates() {
  for (int optimize : {0, 1})
    for (auto dimensions : {std::pair{1u, 1u}, {1u, 17u}, {17u, 1u}, {7u, 3u}, {1920u, 2u}})
      for (bool horizontal : {false, true}) {
        auto [width, height] = dimensions;
        auto o = options(width, height, optimize);
        o.input_count = 1;
        o.inputs[0] = {IRIS_U8, 8};
        Compiled p(horizontal ? "sxr" : "syr", o, 1);
        CHECK(p.info().strategy == IRIS_COMPUTE);
        CHECK(p.info().input_mask == 0);
        CHECK(p.info().metadata_mask == (horizontal ? IRIS_DEP_SX | IRIS_DEP_WIDTH : IRIS_DEP_SY | IRIS_DEP_HEIGHT));
        std::vector<float> output(size_t(width) * height);
        iris_execute_args args{};
        args.output = {output.data(), ptrdiff_t(width) * 4};
        CHECK(iris_execute(p.p, p.c, &args, nullptr) == IRIS_OK);
        auto dimension = horizontal ? width : height;
        for (unsigned y = 0; y < height; ++y)
          for (unsigned x = 0; x < width; ++x) {
            auto coordinate = horizontal ? x : y;
            float expected = dimension == 1 ? 0.0f : float(coordinate) / float(dimension - 1);
            CHECK(same(output[size_t(y) * width + x], expected));
          }
      }
  for (const char* expression : {"1 sxr@", "1 syr^"}) {
    auto o = options();
    iris_plan* p = nullptr;
    iris_diagnostic d{};
    CHECK(iris_compile(expression, &o, &p, &d) == IRIS_PARSE_ERROR);
    CHECK(p == nullptr && d.offset == 2 && d.length == 4);
  }
  std::string oversized;
  for (unsigned i = 0; i < 1400; ++i)
    oversized += "sxr V^ ";
  oversized += "0";
  auto o = options();
  iris_plan* p = nullptr;
  CHECK(iris_compile(oversized.c_str(), &o, &p, nullptr) == IRIS_LIMIT_EXCEEDED && p == nullptr);
}
void extended_execution() {
  iris_compile_options_v1 o{};
  o.struct_size = sizeof(o);
  o.width = 3;
  o.height = 2;
  o.input_count = 26;
  o.backend = test_backend;
  o.output = {IRIS_F32, 32};
  for (auto& format : o.inputs)
    format = {IRIS_U8, 8};
  o.inputs[25] = {IRIS_U16, 10};
  iris_plan* p = nullptr;
  iris_expr_options_v1 expr{};
  expr.struct_size = sizeof(expr);
  expr.frame_count = 8;
  CHECK(iris_compile_expr_v1("time SavedTime^ w a + frameno +", &o, &expr, &p, nullptr) == IRIS_OK);
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (unsigned thread = 0; thread < 8; ++thread)
    workers.emplace_back([&, thread] {
      iris_context* c = nullptr;
      if (iris_context_create(p, &c, nullptr) != IRIS_OK) {
        ++failures;
        return;
      }
      uint16_t w[2][3] = {{100, 200, 300}, {400, 500, 600}};
      uint8_t a[2][3] = {{1, 2, 3}, {4, 5, 6}};
      float output[2][3]{};
      iris_execute_args_v1 args{};
      args.struct_size = sizeof(args);
      args.input_count = 26;
      args.inputs[3] = {a, 3};
      args.inputs[25] = {w, 6};
      args.output = {output, 12};
      args.frameno = thread;
      reject_allocation = true;
      for (unsigned call = 0; call < 100; ++call) {
        if (iris_execute_v1(p, c, &args, nullptr) != IRIS_OK)
          ++failures;
        for (unsigned y = 0; y < 2; ++y)
          for (unsigned x = 0; x < 3; ++x)
            if (output[y][x] != float((y * 3 + x + 1) * 101 + thread))
              ++failures;
      }
      reject_allocation = false;
      iris_context_destroy(c);
    });
  for (auto& worker : workers)
    worker.join();
  iris_plan_destroy(p);
  CHECK(failures == 0);
}
int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "llvm") == 0)
    test_backend = IRIS_BACKEND_LLVM;
  try {
    backend_contract();
    numerical();
    sqrt_boundaries();
    zero_results();
    unary_math();
    libm_math();
    errors();
    layout();
    strategies_and_allocation();
    dependencies();
    differential();
    concurrency();
    frontend_independence();
    lut_tables();
    guard_pages();
    extended_execution();
    normalized_coordinates();
    std::cout << "All Iris checks passed (250 expressions x 15 differential inputs; 8 threads x 200 calls; guard "
                 "pages), backend="
              << test_backend << ".\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
