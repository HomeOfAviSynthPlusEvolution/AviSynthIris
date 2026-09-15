#include "avs_host.hpp"
#include <iris/iris.h>
#include <cmath>
#include <limits>
#include <cstring>
#include <iostream>
#include <iomanip>

namespace {
void compare(AvsApi& api, AVS_ScriptEnvironment* env) {
  struct Mode {
    const char* name;
    iris_scale_inputs value;
  };
  const Mode modes[] = {{"none", IRIS_SCALE_NONE},         {"all", IRIS_SCALE_ALL},
                        {"allf", IRIS_SCALE_ALL_FULL},     {"int", IRIS_SCALE_INT},
                        {"intf", IRIS_SCALE_INT_FULL},     {"float", IRIS_SCALE_FLOAT},
                        {"floatf", IRIS_SCALE_FLOAT_FULL}, {"floatUV", IRIS_SCALE_FLOAT_UV}};
  size_t comparisons = 0;
  for (unsigned bits : {8u, 10u, 16u, 32u})
    for (const auto& mode : modes)
      for (const char* expression : {"x", "x 2 *", "16 scaleb", "i16 x i8 x +", "range_max", "i16 bitdepth"}) {
        std::string type = bits == 32 ? "YUV444PS" : "YUV444P" + std::to_string(bits);
        std::string source = "Expr(BlankClip(width=4,height=4,length=1,pixel_type=\"" + type + "\"),\"" +
                             (bits == 32 ? "0.25" : "100") + "\")";
        std::string script = "Expr(" + source + ",\"" + expression + "\",format=\"YUV444PS\",scale_inputs=\"" +
                             mode.name + "\",optAvx2=false,optSSE2=false,optVectorC=false,lut=0,clamp_float=false)";
        auto value = api.avs_invoke(env, "Eval", make_avs_string(script.c_str()), nullptr);
        if (avs_is_error(value)) {
          std::string error = avs_as_error(value);
          api.avs_release_value(value);
          throw std::runtime_error(error);
        }
        AvsClip clip(api, api.avs_take_clip(value, env));
        api.avs_release_value(value);
        if (!clip.clip)
          throw std::runtime_error("comparison clip failed");
        AvsFrame frame(api, api.avs_get_frame(clip.clip, 0));
        if (!frame.frame)
          throw std::runtime_error("comparison frame failed");
        for (int plane : {AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V}) {
          float expected = 0;
          std::memcpy(&expected, api.avs_get_read_ptr_p(frame.frame, plane), sizeof(expected));
          for (auto backend : {IRIS_BACKEND_SCALAR
#ifdef IRIS_TEST_LLVM
                               ,
                               IRIS_BACKEND_LLVM
#endif
               }) {
            iris_compile_options_v1 o{};
            o.struct_size = sizeof(o);
            o.width = o.height = 1;
            o.input_count = 1;
            o.optimize = 1;
            o.backend = backend;
            o.inputs[0] = {bits == 8 ? IRIS_U8 : bits == 32 ? IRIS_F32 : IRIS_U16, bits};
            o.output = {IRIS_F32, 32};
            iris_expr_options_v1 e{};
            e.struct_size = sizeof(e);
            e.frame_count = 1;
            e.chroma = plane != AVS_PLANAR_Y;
            e.scale_inputs = mode.value;
            iris_plan* plan = nullptr;
            iris_context* context = nullptr;
            iris_diagnostic d{};
            if (iris_compile_expr_v1(expression, &o, &e, &plan, &d) != IRIS_OK)
              throw std::runtime_error(d.message);
            if (iris_context_create(plan, &context, &d) != IRIS_OK) {
              iris_plan_destroy(plan);
              throw std::runtime_error(d.message);
            }
            uint8_t u8 = 100;
            uint16_t u16 = 100;
            float f32 = 0.25f, actual = 0;
            iris_execute_args_v1 a{};
            a.struct_size = sizeof(a);
            a.input_count = 1;
            a.output = {&actual, 4};
            a.inputs[0] = {bits == 8    ? static_cast<const void*>(&u8)
                           : bits == 32 ? static_cast<const void*>(&f32)
                                        : &u16,
                           4};
            auto status = iris_execute_v1(plan, context, &a, &d);
            iris_context_destroy(context);
            iris_plan_destroy(plan);
            if (status != IRIS_OK)
              throw std::runtime_error(d.message);
            float scale = bits == 32 ? 1.0f : float(1u << bits);
            float tolerance = 2 * std::numeric_limits<float>::epsilon() * std::max(scale, std::fabs(expected));
            if (!std::isfinite(actual) || std::fabs(actual - expected) > tolerance)
              throw std::runtime_error(std::string(expression) + " bits=" + std::to_string(bits) +
                                       " mode=" + mode.name + " plane=" + std::to_string(plane) +
                                       " actual=" + std::to_string(actual) + " legacy=" + std::to_string(expected));
            ++comparisons;
          }
        }
      }
  std::cerr << "Scale comparisons passed: " << comparisons << '\n';
}
void run(AvsApi& api, AVS_ScriptEnvironment* env) {
  struct Case {
    const char* expression;
    const char* mode;
  };
  const Case cases[] = {{"32 scaleb i10", "none"},
                        {"i10 32 scaleb", "none"},
                        {"sbitdepth i16", "none"},
                        {"i16 sbitdepth", "none"},
                        {"i10 1 scaleb i8 1 scaleb +", "none"},
                        {"cmin 2 *", "none"},
                        {"cmin 2 *", "floatUV"},
                        {"cmax 2 *", "floatUV"}};
  std::cout << "mode\texpression\tplane\tvalue\n";
  for (const auto& item : cases) {
    std::string script = "Expr(BlankClip(width=4,height=4,length=1,pixel_type=\"YUV444P8\"),\"" +
                         std::string(item.expression) + "\",format=\"YUV444PS\",scale_inputs=\"" + item.mode +
                         "\",optAvx2=false,optSSE2=false,optVectorC=false,lut=0,clamp_float=false)";
    auto result = api.avs_invoke(env, "Eval", make_avs_string(script.c_str()), nullptr);
    if (avs_is_error(result)) {
      std::string error = avs_as_error(result);
      api.avs_release_value(result);
      throw std::runtime_error(error);
    }
    AvsClip clip(api, api.avs_take_clip(result, env));
    api.avs_release_value(result);
    if (!clip.clip)
      throw std::runtime_error("scale probe returned no clip");
    AvsFrame frame(api, api.avs_get_frame(clip.clip, 0));
    if (!frame.frame)
      throw std::runtime_error("scale probe GetFrame failed");
    for (int plane : {AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V}) {
      float first = 0;
      const auto* data = api.avs_get_read_ptr_p(frame.frame, plane);
      auto pitch = api.avs_get_pitch_p(frame.frame, plane);
      std::memcpy(&first, data, sizeof(first));
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          float value = 0;
          std::memcpy(&value, data + y * pitch + x * sizeof(float), sizeof(value));
          if (value != first)
            throw std::runtime_error("nonuniform constant probe result");
        }
      std::cout << item.mode << '\t' << item.expression << '\t' << plane << '\t' << std::setprecision(9) << first
                << '\n';
    }
  }
}
} // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("expected absolute avisynth.dll path");
    AvsApi api(argv[1]);
    auto* env = api.avs_create_script_environment(8);
    if (!env)
      throw std::runtime_error("cannot create AVS environment");
    try {
      run(api, env);
      compare(api, env);
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
