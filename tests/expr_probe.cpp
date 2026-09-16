#include "avs_host.hpp"
#include <iris/iris.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

namespace {
constexpr int width = 17;
const float nan = std::numeric_limits<float>::quiet_NaN();
const float inf = std::numeric_limits<float>::infinity();
const std::array<float, 10> xs = {-4, 4, -0.0f, 0.0f, nan, 1, -inf, inf, nan, -0.0f};
const std::array<float, 10> ys = {2, 9, 0.0f, -0.0f, 1, nan, inf, -inf, nan, -0.0f};
struct Fixture {
  AvsApi& api;
  const std::array<float, 10>& values;
};
uint32_t bits(float value) {
  uint32_t result;
  std::memcpy(&result, &value, sizeof result);
  return result;
}
bool equal(float a, float b) {
  return (std::isnan(a) && std::isnan(b)) || bits(a) == bits(b);
}
AVS_VideoFrame* AVSC_CC frame(AVS_FilterInfo* fi, int) {
  auto& fixture = *static_cast<Fixture*>(fi->user_data);
  auto& api = fixture.api;
  AvsFrame source(api, api.avs_get_frame(fi->child, 0));
  if (!source.frame) {
    fi->error = api.avs_save_string(fi->env, "probe source failed", -1);
    return nullptr;
  }
  AvsFrame out(api, api.avs_new_video_frame_p(fi->env, &fi->vi, source.frame));
  if (!out.frame) {
    fi->error = api.avs_save_string(fi->env, "probe allocation failed", -1);
    return nullptr;
  }
  auto* data = api.avs_get_write_ptr_p(out.frame, AVS_PLANAR_Y);
  int pitch = api.avs_get_pitch_p(out.frame, AVS_PLANAR_Y);
  for (size_t y = 0; y < xs.size(); ++y)
    for (int x = 0; x < width; ++x)
      std::memcpy(data + y * pitch + x * sizeof(float), &fixture.values[y], sizeof(float));
  return out.release();
}
AVS_Value AVSC_CC create(AVS_ScriptEnvironment* env, AVS_Value args, void* user) {
  auto& api = static_cast<Fixture*>(user)->api;
  AVS_FilterInfo* fi = nullptr;
  AvsClip clip(api, api.avs_new_c_filter(env, &fi, avs_array_elt(args, 0), 1));
  if (!clip.clip)
    return make_avs_error("probe fixture creation failed");
  fi->vi = *api.avs_get_video_info(fi->child);
  fi->user_data = user;
  fi->get_frame = frame;
  AVS_Value result{};
  api.avs_set_to_clip(&result, clip.clip);
  return result;
}
std::vector<float> legacy(AvsApi& api, AVS_ScriptEnvironment* env, const char* expression, const char* flags) {
  std::string source = "BlankClip(width=17,height=10,length=1,pixel_type=\"Y32\")";
  std::string script = "Expr(ProbeX(" + source + "),ProbeY(" + source + "),\"" + expression + "\"," + flags +
                       ",optSingleMode=false,scale_inputs=\"none\",clamp_float=false,clamp_float_UV=false,lut=0)";
  auto result = api.avs_invoke(env, "Eval", make_avs_string(script.c_str()), nullptr);
  if (avs_is_error(result)) {
    std::string error = avs_as_error(result);
    api.avs_release_value(result);
    throw std::runtime_error(error);
  }
  AvsClip clip(api, api.avs_take_clip(result, env));
  api.avs_release_value(result);
  if (!clip.clip)
    throw std::runtime_error("probe returned no clip");
  AvsFrame output(api, api.avs_get_frame(clip.clip, 0));
  if (!output.frame)
    throw std::runtime_error("probe GetFrame failed");
  std::vector<float> values(width * xs.size());
  auto* data = api.avs_get_read_ptr_p(output.frame, AVS_PLANAR_Y);
  for (size_t y = 0; y < xs.size(); ++y)
    std::memcpy(values.data() + y * width, data + y * api.avs_get_pitch_p(output.frame, AVS_PLANAR_Y),
                width * sizeof(float));
  return values;
}
std::vector<float> current(const char* expression, iris_backend backend, int optimize) {
  iris_compile_options options{};
  options.width = width;
  options.height = static_cast<uint32_t>(xs.size());
  options.input_count = 2;
  options.inputs[0] = options.inputs[1] = options.output = {IRIS_F32, 32};
  options.optimize = optimize;
  iris_plan* plan = nullptr;
  iris_context* context = nullptr;
  iris_diagnostic diagnostic{};
  if (iris_compile_ex(expression, &options, backend, 0, &plan, &diagnostic) != IRIS_OK)
    throw std::runtime_error(diagnostic.message);
  try {
    if (iris_context_create(plan, &context, &diagnostic) != IRIS_OK)
      throw std::runtime_error(diagnostic.message);
    std::vector<float> x(width * xs.size()), y(x.size()), output(x.size());
    for (size_t row = 0; row < xs.size(); ++row)
      for (int col = 0; col < width; ++col) {
        x[row * width + col] = xs[row];
        y[row * width + col] = ys[row];
      }
    iris_execute_args args{};
    args.inputs[0] = {x.data(), width * sizeof(float)};
    args.inputs[1] = {y.data(), width * sizeof(float)};
    args.output = {output.data(), width * sizeof(float)};
    if (iris_execute(plan, context, &args, &diagnostic) != IRIS_OK)
      throw std::runtime_error(diagnostic.message);
    iris_context_destroy(context);
    iris_plan_destroy(plan);
    return output;
  } catch (...) {
    iris_context_destroy(context);
    iris_plan_destroy(plan);
    throw;
  }
}
void report(const char* path, const char* expression, const std::vector<float>& values,
            const std::vector<float>& reference) {
  for (size_t row = 0; row < xs.size(); ++row) {
    // Every pixel is reported: vector body and scalar tail may differ in the old engine.
    for (int col = 0; col < width; ++col) {
      size_t index = row * width + col;
      float value = values[index];
      if (std::strncmp(path, "iris-", 5) == 0 && !equal(value, reference[index]))
        throw std::runtime_error("Iris backend/optimizer disagreement");
      if (row == 1) {
        float expected = std::strcmp(expression, "x sqrt") == 0    ? 2.0f
                         : std::strcmp(expression, "x y min") == 0 ? 4.0f
                         : std::strcmp(expression, "x y max") == 0 ? 9.0f
                                                                   : 0.0f;
        if (expression[0] == 'x' && !equal(value, expected))
          throw std::runtime_error("ordinary finite-value probe failed");
      }
      std::cout << path << '\t' << expression << '\t' << row << '\t' << col << '\t' << std::hex << std::setfill('0')
                << std::setw(8) << bits(xs[row]) << '\t' << std::setw(8) << bits(ys[row]) << '\t' << std::setw(8)
                << bits(value) << std::dec << '\t' << (equal(value, reference[index]) ? "same" : "different") << '\n';
    }
  }
}
// Parsing observations are deliberately separate from the special-value oracle.
void parsing_probe(AvsApi& api, AVS_ScriptEnvironment* env) {
  const char* expressions[] = {"1e-50",
                               "-1e-50",
                               "1e-46",
                               "7e-46",
                               "8e-46",
                               "1.401298464324817e-45",
                               "1.1754943508222875e-38",
                               "3.4028234663852886e38",
                               "3.402824e38",
                               "1e9999",
                               "0e-9999",
                               "1e+",
                               "1.5tail",
                               "0x1.8p0",
                               "0x10",
                               "0x-10",
                               "-0X1.8P+1",
                               "+0x.8p1",
                               "0x1p-149",
                               "0x1.fffffep127",
                               "-0x0p0",
                               "0x1p-150",
                               "nan",
                               "inf",
                               "+.5",
                               "1.",
                               "2 dup+0 +",
                               "2 dup-0 +",
                               "2 dup0tail +",
                               "2 dup_name@ dup_name +",
                               "2 3 swap+1 -",
                               "2 3 swap1tail -",
                               "2 3 swap0 -",
                               "2 A@ 3 A@ + A +",
                               "2 A^ A",
                               "2 sqrt@",
                               "2 x@",
                               "2 dup0@",
                               "x[+0,-0]",
                               "x[0,0]tail",
                               "x[17,0]",
                               "x[-2147483648,0]",
                               "1 2",
                               "1 +",
                               "1,5"};
  std::cout << "parser_path\texpression\tstatus\tfirst_result_bits\n";
  for (const char* expression : expressions) {
    auto observe = [&](const char* name, auto evaluate) {
      try {
        const auto values = evaluate();
        std::cout << name << '\t' << expression << "\tok\t" << std::hex << bits(values.front()) << std::dec << '\n';
      } catch (const std::exception&) {
        std::cout << name << '\t' << expression << "\trejected\t-\n";
      }
    };
    observe("avs-parser", [&] { return legacy(api, env, expression, "optAvx2=false,optSSE2=false,optVectorC=false"); });
    observe("iris-parser", [&] { return current(expression, IRIS_BACKEND_SCALAR, 0); });
  }
}
void run(AvsApi& api, AVS_ScriptEnvironment* env) {
  Fixture x{api, xs}, y{api, ys};
  if (api.avs_add_function(env, "ProbeX", "c", create, &x) || api.avs_add_function(env, "ProbeY", "c", create, &y))
    throw std::runtime_error("probe registration failed");
  parsing_probe(api, env);
  auto cpu = reinterpret_cast<avs_get_cpu_flags_func>(GetProcAddress(api.handle, "avs_get_cpu_flags"));
  if (!cpu)
    throw std::runtime_error("missing CPU capability query");
  struct Path {
    const char* name;
    const char* flags;
    int required;
  };
  const Path paths[] = {{"avs-scalar-request", "optAvx2=false,optSSE2=false,optVectorC=false", 0},
                        {"avs-vectorc-request", "optAvx2=false,optSSE2=false,optVectorC=true", 0},
                        {"avs-sse2-request", "optAvx2=false,optSSE2=true,optVectorC=false", AVS_CPU_SSE2},
                        {"avs-avx2-request", "optAvx2=true,optSSE2=true,optVectorC=false", AVS_CPUF_AVX2}};
  std::cout << "path\texpression\trow\tcolumn\tx_bits\ty_bits\tresult_bits\tvs_iris_scalar_opt0\n";
  // Finite expressions cover parser operations and their compositions in the
  // actual legacy engines. Existing unit tests provide independent oracles.
  const char* corpus[] = {
      "x y +",          "x y -",         "x y *",         "x y /",        "x y %",         "x y min",      "x y max",
      "x abs",          "x neg",         "x sgn",         "x sqrt",       "x exp",         "x log",        "x 2 pow",
      "x 2 ^",          "x sin",         "x cos",         "x tan",        "x 0.1 * asin",  "x 0.1 * acos", "x atan",
      "x y atan2",      "x 0.3 * round", "x 0.3 * floor", "x 0.3 * ceil", "x 0.3 * trunc", "x 1 3 clip",   "x y <",
      "x y <=",         "x y >",         "x y >=",        "x y =",        "x y ==",        "x y !=",       "x y and",
      "x y &",          "x y or",        "x y |",         "x y xor",      "x not",         "x y 2 ?",      "x dup *",
      "x y swap -",     "x y dup1 + +",  "x y swap1 -",   "x A@ y A * +", "x A^ y A +",    "sx sy +",      "sxr syr +",
      "width height +", "frameno pi +"};
  size_t comparisons = 0, legacy_differences = 0;
  for (const char* expression : corpus) {
    const auto reference = current(expression, IRIS_BACKEND_SCALAR, 0);
    auto verify = [&](const std::vector<float>& values, const char* path) {
      for (int col = 0; col < width; ++col) {
        const size_t index = width + col; // fixture row 1: x=4, y=9, all operations finite
        if (!std::isfinite(values[index]) ||
            std::abs(values[index] - reference[index]) > 1e-4f * std::max(1.0f, std::abs(reference[index]))) {
          if (std::strcmp(path, "iris") == 0)
            throw std::runtime_error(std::string("finite expression mismatch: ") + path + ": " + expression);
          ++legacy_differences;
          if (col == 0)
            std::cerr << "legacy difference: " << path << ": " << expression << ": actual=" << values[index]
                      << ", reference=" << reference[index] << '\n';
        }
        ++comparisons;
      }
    };
    for (auto backend : {IRIS_BACKEND_SCALAR, IRIS_BACKEND_LLVM, IRIS_BACKEND_SLEEF, IRIS_BACKEND_SLEEF_FAST})
      if (iris_backend_available(backend))
        for (int optimize : {0, 1})
          verify(current(expression, backend, optimize), "iris");
    for (const auto& path : paths)
      if ((cpu(env) & path.required) == path.required)
        verify(legacy(api, env, expression, path.flags), path.name);
  }
  std::cerr << "finite corpus: " << std::size(corpus) << " expressions, " << comparisons << " samples checked, "
            << legacy_differences << " legacy differences\n";
  for (const char* expression : {"x sqrt", "x y min", "x y max", "-1 sqrt", "-0 0 min", "0 -0 max"}) {
    auto reference = current(expression, IRIS_BACKEND_SCALAR, 0);
    report("iris-scalar-opt0", expression, reference, reference);
    report("iris-scalar-opt1", expression, current(expression, IRIS_BACKEND_SCALAR, 1), reference);
#ifdef IRIS_TEST_LLVM
    report("iris-llvm-opt0", expression, current(expression, IRIS_BACKEND_LLVM, 0), reference);
    report("iris-llvm-opt1", expression, current(expression, IRIS_BACKEND_LLVM, 1), reference);
#endif
    for (const auto& path : paths) {
      if ((cpu(env) & path.required) != path.required) {
        std::cerr << "SKIP " << path.name << ": CPU capability unavailable\n";
        continue;
      }
      report(path.name, expression, legacy(api, env, expression, path.flags), reference);
    }
  }
}
} // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("expected absolute avisynth.dll path; stdout is TSV observations");
    AvsApi api(argv[1]);
    auto* env = api.avs_create_script_environment(8);
    if (!env)
      throw std::runtime_error("cannot create AVS environment");
    try {
      run(api, env);
    } catch (...) {
      api.avs_delete_script_environment(env);
      throw;
    }
    api.avs_delete_script_environment(env);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
