#include "avs_host.hpp"
#include <cstring>
#include <iostream>
#include <iomanip>

namespace {
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
