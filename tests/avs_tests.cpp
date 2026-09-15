#include "avs_host.hpp"
#include <algorithm>
#include <iostream>
#include <limits>

namespace {
constexpr int planes[] = {AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V};
AVS_VideoFrame* AVSC_CC fixture_frame(AVS_FilterInfo* fi, int n) {
  auto& api = *static_cast<AvsApi*>(fi->user_data);
  try {
    AvsFrame src(api, api.avs_get_frame(fi->child, n));
    if (!src.frame)
      throw std::runtime_error("fixture source failed");
    AvsFrame out(api, api.avs_new_video_frame_p(fi->env, &fi->vi, src.frame));
    if (!out.frame)
      throw std::runtime_error("fixture allocation failed");
    for (int p = 0; p < 3; ++p) {
      int width = api.avs_get_row_size_p(out.frame, planes[p]), height = api.avs_get_height_p(out.frame, planes[p]);
      auto* data = api.avs_get_write_ptr_p(out.frame, planes[p]);
      int pitch = api.avs_get_pitch_p(out.frame, planes[p]);
      for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
          data[y * pitch + x] = uint8_t(3 + p * 7 + x + y);
    }
    if (api.avs_prop_set_float(fi->env, api.avs_get_frame_props_rw(fi->env, out.frame), "Gain", n + 1.0, 0))
      throw std::runtime_error("fixture property failed");
    auto* map = api.avs_get_frame_props_rw(fi->env, out.frame);
    if (api.avs_prop_set_int(fi->env, map, "Integer", 7, 0) ||
        api.avs_prop_set_data(fi->env, map, "Text", "hello", 5, 0) ||
        api.avs_prop_set_float(fi->env, map, "Array", 3.0, 0) ||
        api.avs_prop_set_float(fi->env, map, "Array", 99.0, 1) ||
        api.avs_prop_set_float(fi->env, map, "Huge", 1e300, 0) ||
        api.avs_prop_set_float(fi->env, map, "NegativeHuge", -1e300, 0) ||
        api.avs_prop_set_float(fi->env, map, "Infinity", std::numeric_limits<double>::infinity(), 0) ||
        api.avs_prop_set_float(fi->env, map, "NotNumber", std::numeric_limits<double>::quiet_NaN(), 0))
      throw std::runtime_error("fixture property matrix failed");
    return out.release();
  } catch (const std::exception& e) {
    fi->error = api.avs_save_string(fi->env, e.what(), -1);
    return nullptr;
  }
}
AVS_Value AVSC_CC fixture_create(AVS_ScriptEnvironment* env, AVS_Value args, void* user) {
  auto& api = *static_cast<AvsApi*>(user);
  AVS_FilterInfo* fi = nullptr;
  AvsClip clip(api, api.avs_new_c_filter(env, &fi, avs_array_elt(args, 0), 1));
  if (!clip.clip)
    return make_avs_error("fixture create failed");
  fi->vi = *api.avs_get_video_info(fi->child);
  fi->user_data = user;
  fi->get_frame = fixture_frame;
  AVS_Value v{};
  api.avs_set_to_clip(&v, clip.clip);
  return v;
}
void test(AvsApi& api, AVS_ScriptEnvironment* env, const char* backend, bool lut) {
  std::string expression = lut ? "x 2 * 1 +" : "x x.Gain * sx + sy + width + height + frameno +";
  std::string script = "IrisPoC(IrisFixture(BlankClip(width=18, height=10, length=3, pixel_type=\"YV12\")), \"" +
                       expression + "\", \"" + backend + "\", " + (lut ? "true" : "false") + ")";
  AVS_Value result = api.avs_invoke(env, "Eval", make_avs_string(script.c_str()), nullptr);
  if (avs_is_error(result)) {
    std::string e = avs_as_error(result);
    api.avs_release_value(result);
    throw std::runtime_error(e);
  }
  AvsClip clip(api, api.avs_take_clip(result, env));
  api.avs_release_value(result);
  if (!clip.clip)
    throw std::runtime_error("script returned no clip");
  for (int n : {2, 0, 1, 2}) {
    AvsFrame frame(api, api.avs_get_frame(clip.clip, n));
    if (!frame.frame) {
      auto* e = api.avs_clip_get_error(clip.clip);
      throw std::runtime_error(e ? e : "frame failed");
    }
    int error = 0;
    auto gain = api.avs_prop_get_float(env, api.avs_get_frame_props_ro(env, frame.frame), "Gain", 0, &error);
    if (error || gain != n + 1)
      throw std::runtime_error("frame properties not preserved");
    for (int p = 0; p < 3; ++p) {
      int w = p ? 9 : 18, h = p ? 5 : 10;
      if (api.avs_get_row_size_p(frame.frame, planes[p]) != w || api.avs_get_height_p(frame.frame, planes[p]) != h)
        throw std::runtime_error("wrong chroma geometry");
      const auto* data = api.avs_get_read_ptr_p(frame.frame, planes[p]);
      auto stride = api.avs_get_pitch_p(frame.frame, planes[p]);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          int sample = 3 + p * 7 + x + y;
          int expected = lut ? sample * 2 + 1 : sample * (n + 1) + x + y + w + h + n;
          if (data[y * stride + x] != std::min(255, expected))
            throw std::runtime_error("AVS pixel mismatch");
        }
    }
  }
  std::cout << "AVS " << backend << " lut=" << lut
            << ": Y 18x10 / UV 9x5; dynamic properties and out-of-order frames passed\n";
}
void errors(AvsApi& api, AVS_ScriptEnvironment* env) {
  for (const char* script : {"IrisPoC(BlankClip(pixel_type=\"YV12\"), \"x +\", \"scalar\", false)",
                             "IrisPoC(BlankClip(pixel_type=\"RGB32\"), \"x\", \"scalar\", false)",
                             "IrisPoC(BlankClip(pixel_type=\"YV12\"), \"x\", \"unknown\", false)"}) {
    auto result = api.avs_invoke(env, "Eval", make_avs_string(script), nullptr);
    bool failed = avs_is_error(result);
    api.avs_release_value(result);
    if (!failed)
      throw std::runtime_error("AVS accepted invalid compile request");
  }
}
void property_contract(AvsApi& api, AVS_ScriptEnvironment* env, const char* backend) {
  struct Case {
    const char* expression;
    int expected;
  };
  const Case cases[] = {{"x.Missing", 0},
                        {"x.Text", 0},
                        {"x.Integer", 7},
                        {"x.Array", 3},
                        {"x.Huge 1e38 > x.Huge 1e38 / 4 < and", 1},
                        {"x.NegativeHuge -1e38 < x.NegativeHuge -1e38 / 4 < and", 1},
                        {"x.Infinity 1e38 / 4 <", 1},
                        {"x.NotNumber 0 !=", 1},
                        {"x.Gain frameno 1 + =", 1}};
  for (const auto& item : cases) {
    std::string source = "IrisFixture(BlankClip(width=18,height=10,length=3,pixel_type=\"YV12\"))";
    std::string script = std::string(backend ? "IrisPoC(" : "Expr(") + source + ",\"" + item.expression + "\"";
    script +=
        backend ? std::string(",\"") + backend + "\",false)" : ",optAvx2=false,optSSE2=false,optVectorC=false,lut=0)";
    auto result = api.avs_invoke(env, "Eval", make_avs_string(script.c_str()), nullptr);
    if (avs_is_error(result)) {
      std::string error = avs_as_error(result);
      api.avs_release_value(result);
      throw std::runtime_error(error);
    }
    AvsClip clip(api, api.avs_take_clip(result, env));
    api.avs_release_value(result);
    if (!clip.clip)
      throw std::runtime_error("property script returned no clip");
    for (int n : {2, 0, 1}) {
      AvsFrame output(api, api.avs_get_frame(clip.clip, n));
      if (!output.frame)
        throw std::runtime_error("property frame failed");
      for (int plane : planes) {
        const auto* data = api.avs_get_read_ptr_p(output.frame, plane);
        for (int y = 0; y < api.avs_get_height_p(output.frame, plane); ++y)
          for (int x = 0; x < api.avs_get_row_size_p(output.frame, plane); ++x)
            if (data[y * api.avs_get_pitch_p(output.frame, plane) + x] != item.expected)
              throw std::runtime_error(std::string("property mismatch: ") + item.expression);
      }
    }
  }
  std::cout << "Property contract passed: " << (backend ? backend : "Expr scalar request") << '\n';
}
} // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("expected absolute avisynth.dll path");
    AvsApi api(argv[1]);
    auto* env = api.avs_create_script_environment(8);
    if (!env)
      throw std::runtime_error("cannot create AVS environment v8");
    try {
      register_iris_avs(api, env);
      if (api.avs_add_function(env, "IrisFixture", "c", fixture_create, &api) != 0)
        throw std::runtime_error("register fixture failed");
      errors(api, env);
      property_contract(api, env, nullptr);
      property_contract(api, env, "scalar");
      test(api, env, "scalar", false);
      test(api, env, "scalar", true);
#ifdef IRIS_TEST_LLVM
      property_contract(api, env, "llvm");
      test(api, env, "llvm", false);
      test(api, env, "llvm", true);
#endif
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
