#include "runtime/manual_lut.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

namespace {
iris_backend selected_backend = IRIS_BACKEND_LLVM;
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct PlanDelete {
  void operator()(iris_plan* p) const { iris_plan_destroy(p); }
};
struct ContextDelete {
  void operator()(iris_context* p) const { iris_context_destroy(p); }
};
struct Timing {
  double minimum, median, maximum;
  size_t repetitions;
};
template <class F>
Timing measure(F run, double target_ms) {
  run();
  std::vector<double> samples;
  size_t repetitions = 0;
  for (int sample = 0; sample < 5; ++sample) {
    auto start = Clock::now();
    size_t count = 0;
    do {
      run();
      ++count;
    } while (elapsed(start) < target_ms && count < 10000);
    samples.push_back(elapsed(start) / double(count));
    repetitions += count;
  }
  std::sort(samples.begin(), samples.end());
  return {samples.front(), samples[2], samples.back(), repetitions};
}
uint32_t random_value(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}
uint64_t checksum(const std::vector<float>& output) {
  uint64_t hash = 14695981039346656037ull;
  for (float value : output) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    hash = (hash ^ bits) * 1099511628211ull;
  }
  return hash;
}
void benchmark(uint32_t bits, bool complex, bool random, bool quick) {
  const uint32_t width = quick ? 257 : 1280, height = quick ? 17 : 720;
  const size_t pixels = size_t(width) * height;
  const uint32_t mask = (1u << bits) - 1;
  // Identical planar storage for each measured strategy; float output makes
  // arithmetic differences visible instead of hiding them with integer clipping.
  std::vector<unsigned char> x(pixels * (bits == 8 ? 1 : 2)), y(x.size());
  uint32_t seed = 0x12345678;
  for (size_t i = 0; i < pixels; ++i) {
    uint16_t a = uint16_t((random ? random_value(seed) : uint32_t(i % width)) & mask);
    uint16_t b = uint16_t((random ? random_value(seed) : uint32_t(i / width)) & mask);
    if (bits == 8) {
      x[i] = static_cast<unsigned char>(a);
      y[i] = static_cast<unsigned char>(b);
    } else {
      std::memcpy(x.data() + i * 2, &a, 2);
      std::memcpy(y.data() + i * 2, &b, 2);
    }
  }
  std::vector<float> output(pixels), reference;
  const char* expression = complex ? "x 0.001 * sin abs y 0.001 * cos abs + sqrt" : "x y + 0.5 *";
  for (int strategy = 0; strategy < 3; ++strategy) {
    if (strategy == 1 && !iris::llvm_available())
      continue;
    iris_backend backend = strategy == 0 || !iris::llvm_available() ? IRIS_BACKEND_SCALAR : selected_backend;
    iris_compile_options_v1 o{};
    o.struct_size = sizeof(o);
    o.width = width;
    o.height = height;
    o.input_count = 2;
    o.inputs[0] = o.inputs[1] = {bits == 8 ? IRIS_U8 : IRIS_U16, bits};
    o.output = {IRIS_F32, 32};
    o.optimize = 1;
    o.backend = backend;
    iris_expr_options_v1 e{sizeof(e), 1, 0, IRIS_SCALE_NONE, 0, 0};
    std::unique_ptr<iris_plan, PlanDelete> plan;
    double first_compile = 0;
    std::vector<double> compilation;
    for (int trial = 0; trial < 3; ++trial) {
      iris_plan* raw = nullptr;
      iris_diagnostic diagnostic{};
      auto start = Clock::now();
      auto status = iris_compile_expr_v1(expression, &o, &e, &raw, &diagnostic);
      double ms = elapsed(start);
      if (status != IRIS_OK)
        throw std::runtime_error(diagnostic.message);
      if (!trial)
        first_compile = ms;
      compilation.push_back(ms);
      plan.reset(raw);
    }
    std::sort(compilation.begin(), compilation.end());
    iris_context* raw_context = nullptr;
    if (iris_context_create(plan.get(), &raw_context, nullptr) != IRIS_OK)
      throw std::runtime_error("benchmark context creation failed");
    std::unique_ptr<iris_context, ContextDelete> context(raw_context);
    iris_execute_args_v1 args{};
    args.struct_size = sizeof(args);
    args.input_count = 2;
    args.inputs[0] = {x.data(), ptrdiff_t(width * (bits == 8 ? 1 : 2))};
    args.inputs[1] = {y.data(), args.inputs[0].stride};
    args.output = {output.data(), ptrdiff_t(width * 4)};
    std::unique_ptr<iris::ManualLut> lut;
    double build_ms = 0;
    if (strategy == 2) {
      auto start = Clock::now();
      lut = std::make_unique<iris::ManualLut>(*plan, backend);
      iris::check_lut_budget(lut->storage_bytes(), 256);
      lut->build({});
      build_ms = elapsed(start);
    }
    auto run = [&] {
      if (lut)
        lut->apply(args.inputs, args.output, width, height);
      else if (iris_execute_v1(plan.get(), context.get(), &args, nullptr) != IRIS_OK)
        throw std::runtime_error("benchmark execution failed");
    };
    run();
    if (strategy == 0) {
      reference = output;
      // Independent expected values at distributed pixels, before any timing.
      for (size_t i = 0; i < pixels; i += std::max(size_t(1), pixels / 1024)) {
        uint16_t a = 0, b = 0;
        size_t bytes = bits == 8 ? 1 : 2;
        std::memcpy(&a, x.data() + i * bytes, bytes);
        std::memcpy(&b, y.data() + i * bytes, bytes);
        float expected = complex
                             ? std::sqrt(std::abs(std::sin(float(a) * 0.001f)) + std::abs(std::cos(float(b) * 0.001f)))
                             : float(a + b) * 0.5f;
        if (std::abs(output[i] - expected) > (complex ? 1e-6f : 0.0f))
          throw std::runtime_error("benchmark independent oracle mismatch");
      }
    } else {
      for (size_t i = 0; i < pixels; ++i)
        if (!std::isfinite(output[i]) || std::abs(output[i] - reference[i]) > (complex ? 1e-6f : 0.0f))
          throw std::runtime_error("benchmark strategy result mismatch");
    }
    auto t = measure(run, quick ? 2 : 100);
    iris_plan_info info{};
    iris_plan_get_info(plan.get(), &info, nullptr);
    size_t dump_bytes = 0;
    iris_plan_dump(plan.get(), nullptr, 0, &dump_bytes, nullptr);
    std::cout << bits << ',' << (complex ? "trig_sqrt" : "average") << ',' << (random ? "random" : "spatial") << ','
              << (strategy == 0   ? "scalar"
                  : strategy == 1 ? "llvm"
                                  : "manual_lut")
              << ','
              << (backend == IRIS_BACKEND_SCALAR  ? "scalar"
                  : backend == IRIS_BACKEND_LLVM  ? "llvm"
                  : backend == IRIS_BACKEND_SLEEF ? "sleef"
                                                  : "sleef-fast")
              << ',' << width << ',' << height << ',' << first_compile << ',' << compilation[1] << ',' << build_ms
              << ',' << t.minimum << ',' << t.median << ',' << t.maximum << ',' << double(pixels) / (t.median * 1000)
              << ',' << t.repetitions << ',' << (lut ? lut->storage_bytes() : 0) << ',' << info.instruction_count << ','
              << dump_bytes << ',' << checksum(output) << '\n'
              << std::flush;
  }
}
} // namespace
int main(int argc, char** argv) {
  try {
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
      std::string option = argv[i];
      if (option == "--quick")
        quick = true;
      else if (option == "--sleef")
        selected_backend = IRIS_BACKEND_SLEEF;
      else if (option == "--sleef-fast")
        selected_backend = IRIS_BACKEND_SLEEF_FAST;
      else
        throw std::runtime_error("usage: iris_benchmark [--quick] [--sleef|--sleef-fast]");
    }
    if (selected_backend != IRIS_BACKEND_LLVM && !iris_backend_available(selected_backend))
      throw std::runtime_error("selected benchmark backend requires LLVM and SLEEF");
    std::cout << std::fixed << std::setprecision(6)
              << "bits,expression,pattern,strategy,generation_backend,width,height,first_compile_ms,compile_median_ms,"
                 "lut_build_ms,execute_min_ms,execute_median_ms,execute_max_ms,megapixels_per_second,repetitions,"
                 "table_bytes,ir_nodes,ir_dump_bytes,checksum\n";
    for (uint32_t bits : {8u, 10u, 12u}) {
      if (quick && bits == 12)
        continue;
      for (bool complex : {false, true})
        for (bool random : {false, true})
          benchmark(bits, complex, random, quick);
    }
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
