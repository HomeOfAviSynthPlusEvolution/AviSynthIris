#include "avs_host.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>
#include <cstring>

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
AVS_VideoFrame* AVSC_CC forbidden_frame(AVS_FilterInfo* fi, int) {
  fi->error = "unused source was evaluated";
  return nullptr;
}
AVS_Value AVSC_CC forbidden_create(AVS_ScriptEnvironment* env, AVS_Value args, void* user) {
  auto& api = *static_cast<AvsApi*>(user);
  AVS_FilterInfo* fi = nullptr;
  AvsClip clip(api, api.avs_new_c_filter(env, &fi, avs_array_elt(args, 0), 1));
  if (!clip.clip)
    return make_avs_error("forbidden source creation failed");
  fi->vi = *api.avs_get_video_info(fi->child);
  fi->get_frame = forbidden_frame;
  AVS_Value result{};
  api.avs_set_to_clip(&result, clip.clip);
  return result;
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
std::unique_ptr<AvsClip> script_clip(AvsApi& api, AVS_ScriptEnvironment* env, const std::string& script) {
  auto value = api.avs_invoke(env, "Eval", make_avs_string(script.c_str()), nullptr);
  if (avs_is_error(value)) {
    std::string error = avs_as_error(value);
    api.avs_release_value(value);
    throw std::runtime_error(script + ": " + error);
  }
  auto clip = std::make_unique<AvsClip>(api, api.avs_take_clip(value, env));
  api.avs_release_value(value);
  if (!clip->clip)
    throw std::runtime_error("script returned no clip");
  return clip;
}
void check_uniform(AvsApi& api, AVS_Clip* clip, const std::vector<float>& expected, int n = 0) {
  auto* vi = api.avs_get_video_info(clip);
  int bits = api.avs_bits_per_component(vi), count = api.avs_num_components(vi);
  if (int(expected.size()) != count)
    throw std::runtime_error("unexpected plane count");
  AvsFrame frame(api, api.avs_get_frame(clip, n));
  if (!frame.frame)
    throw std::runtime_error("IrisExpr frame failed");
  const int yuv[] = {AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V, AVS_PLANAR_A};
  const int rgb[] = {AVS_PLANAR_R, AVS_PLANAR_G, AVS_PLANAR_B, AVS_PLANAR_A};
  for (int p = 0; p < count; ++p) {
    int plane = avs_is_rgb(vi) ? rgb[p] : yuv[p];
    int bytes = bits == 8 ? 1 : bits == 32 ? 4 : 2;
    auto* data = api.avs_get_read_ptr_p(frame.frame, plane);
    auto pitch = api.avs_get_pitch_p(frame.frame, plane);
    for (int y = 0; y < api.avs_get_height_p(frame.frame, plane); ++y)
      for (int x = 0; x < api.avs_get_row_size_p(frame.frame, plane) / bytes; ++x) {
        float sample = 0;
        auto* at = data + ptrdiff_t(y) * pitch + x * bytes;
        if (bits == 8)
          sample = *at;
        else if (bits == 32)
          std::memcpy(&sample, at, 4);
        else {
          uint16_t value = 0;
          std::memcpy(&value, at, 2);
          sample = value;
        }
        if (sample != expected[p])
          throw std::runtime_error("IrisExpr plane=" + std::to_string(p) + " actual=" + std::to_string(sample) +
                                   " expected=" + std::to_string(expected[p]));
      }
  }
}
void expr_adapter(AvsApi& api, AVS_ScriptEnvironment* env, const char* backend) {
  std::string suffix = std::string(",backend=\"") + backend + "\")";
  for (const char* type : {"Y8", "Y10", "Y16", "Y32", "YV12", "YUV422P12", "YUV444P16", "YUV444PS", "YUVA420P8",
                           "YUVA444P16", "RGBP", "RGBP16", "RGBPS", "RGBAP", "RGBAPS"}) {
    std::string blank = "BlankClip(width=18,height=10,length=3,pixel_type=\"" + std::string(type) + "\")";
    auto source = script_clip(api, env, blank);
    int count = api.avs_num_components(api.avs_get_video_info(source->clip));
    std::string values = "\"10\"";
    for (int p = 1; p < count; ++p)
      values += ",\"" + std::to_string((p + 1) * 10) + "\"";
    std::string input = "Expr(" + blank + "," + values + ")";
    auto clip = script_clip(api, env, "IrisExpr(" + input + ",\"x 1 +\"" + suffix);
    std::vector<float> expected;
    for (int p = 0; p < count; ++p)
      expected.push_back(float((p + 1) * 10 + (p == 3 ? 0 : 1)));
    for (int n : {2, 0, 1})
      check_uniform(api, clip->clip, expected, n);
    if (count >= 3) {
      auto two = script_clip(api, env, "IrisExpr(" + input + ",\"1\",\"2\"" + suffix);
      std::vector<float> want = {1, 2, 2};
      if (count == 4)
        want.push_back(40);
      check_uniform(api, two->clip, want);
    }
    auto copied = script_clip(api, env, "IrisExpr(" + input + ",\"\"" + suffix);
    for (int p = 0; p < count; ++p)
      expected[p] = float((p + 1) * 10);
    check_uniform(api, copied->clip, expected);
  }
  std::string clips;
  for (int i = 0; i < 26; ++i) {
    if (i)
      clips += ",";
    clips += "Expr(BlankClip(width=7,height=3,length=3,pixel_type=\"Y16\"),\"" + std::to_string(i + 1) + "\")";
  }
  auto multi = script_clip(
      api, env,
      "IrisExpr(" + clips +
          ",\"x y + z + a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q + r + s + t + u + v + w +\"" +
          suffix);
  check_uniform(api, multi->clip, {351});
  auto mixed = script_clip(api, env,
                           "IrisExpr(Expr(BlankClip(width=7,height=3,pixel_type=\"Y8\"),\"10\"),Expr(BlankClip(width=7,"
                           "height=3,pixel_type=\"Y16\"),\"1000\"),\"x y +\",format=\"Y32\"" +
                               suffix);
  check_uniform(api, mixed->clip, {1010});
  auto broadcast = script_clip(
      api, env, "IrisExpr(Expr(BlankClip(width=7,height=3,pixel_type=\"Y8\"),\"9\"),\"x\",format=\"RGBP\"" + suffix);
  check_uniform(api, broadcast->clip, {9, 9, 9});
  auto time =
      script_clip(api, env, "IrisExpr(BlankClip(width=7,height=3,length=3,pixel_type=\"Y32\"),\"time\"" + suffix);
  for (int n : {2, 0, 1})
    check_uniform(api, time->clip, {float(n) / 2}, n);
  auto corrected = script_clip(
      api, env,
      "IrisExpr(BlankClip(width=8,height=4,pixel_type=\"YUV444P8\"),\"cmin 2 *\",scale_inputs=\"floatUV\"" + suffix);
  check_uniform(api, corrected->clip, {32, 32, 32});
  std::string blank = "BlankClip(width=18,height=10,length=3,pixel_type=\"YV12\")";
  auto property_only =
      script_clip(api, env, "IrisExpr(IrisForbidden(" + blank + "),IrisFixture(" + blank + "),\"y.Gain\"" + suffix);
  for (int n : {2, 0, 1}) {
    check_uniform(api, property_only->clip, {float(n + 1), float(n + 1), float(n + 1)}, n);
    AvsFrame frame(api, api.avs_get_frame(property_only->clip, n));
    int error = 0;
    double gain = api.avs_prop_get_float(env, api.avs_get_frame_props_ro(env, frame.frame), "Gain", 0, &error);
    if (error || gain != n + 1)
      throw std::runtime_error("properties not inherited from first used input");
  }
  auto unused = script_clip(api, env, "IrisExpr(IrisForbidden(" + blank + "),\"42\"" + suffix);
  check_uniform(api, unused->clip, {42, 42, 42});
  auto failed = script_clip(api, env, "IrisExpr(IrisForbidden(" + blank + "),\"x\"" + suffix);
  AvsFrame failed_frame(api, api.avs_get_frame(failed->clip, 0));
  if (failed_frame.frame || !api.avs_clip_get_error(failed->clip))
    throw std::runtime_error("IrisExpr did not propagate a used input failure");
  auto audio_clip = script_clip(api, env,
                                "IrisExpr(BlankClip(width=7,height=3,length=9,pixel_type=\"Y8\",audio_rate=48000,"
                                "channels=2,sample_type=\"16bit\"),\"42\"" +
                                    suffix);
  const auto* audio_info = api.avs_get_video_info(audio_clip->clip);
  if (audio_info->audio_samples_per_second != 48000 || audio_info->nchannels != 2 || audio_info->num_frames != 9)
    throw std::runtime_error("IrisExpr changed source audio or frame metadata");
  int16_t audio[32];
  for (auto& sample : audio)
    sample = 123;
  if (api.avs_get_audio(audio_clip->clip, audio, 0, 16) != 0)
    throw std::runtime_error("IrisExpr audio forwarding failed");
  for (auto sample : audio)
    if (sample != 0)
      throw std::runtime_error("IrisExpr changed silent source audio");
  auto prefetched = script_clip(
      api, env, "IrisExpr(BlankClip(width=7,height=3,length=9,pixel_type=\"Y32\"),\"time\"" + suffix + ".Prefetch(4)");
  for (int n : {8, 0, 4, 1, 7, 2, 6, 3, 5})
    check_uniform(api, prefetched->clip, {float(n) / 8}, n);
  for (const char* script : {"IrisExpr(BlankClip(pixel_type=\"Y8\"),\"\",format=\"Y16\")",
                             "IrisExpr(BlankClip(pixel_type=\"RGBAP\"),\"x\",format=\"RGBAP16\")",
                             "IrisExpr(BlankClip(pixel_type=\"RGB32\"),\"x\")",
                             "IrisExpr(BlankClip(pixel_type=\"Y8\"),\"x\",format=\"YV12\")",
                             "IrisExpr(BlankClip(pixel_type=\"Y8\"),\"x\",\"y\")",
                             "IrisExpr(BlankClip(pixel_type=\"Y8\"),\"x\",scale_inputs=\"bad\")"}) {
    auto result = api.avs_invoke(env, "Eval", make_avs_string(script), nullptr);
    bool error = avs_is_error(result);
    api.avs_release_value(result);
    if (!error)
      throw std::runtime_error(std::string("accepted invalid IrisExpr script: ") + script);
  }
#ifndef IRIS_TEST_LLVM
  for (const char* expression : {"x", ""}) {
    std::string script = "IrisExpr(BlankClip(pixel_type=\"Y8\"),\"" + std::string(expression) + "\",backend=\"llvm\")";
    auto result = api.avs_invoke(env, "Eval", make_avs_string(script.c_str()), nullptr);
    bool error = avs_is_error(result);
    api.avs_release_value(result);
    if (!error)
      throw std::runtime_error("unavailable LLVM backend was silently accepted");
  }
#endif
  std::cout << "IrisExpr format, multi-input, expressions and context passed: " << backend << '\n';
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
void expect_script_error(AvsApi& api, AVS_ScriptEnvironment* env, const std::string& script,
                         const std::vector<std::string>& messages) {
  auto value = api.avs_invoke(env, "Eval", make_avs_string(script.c_str()), nullptr);
  std::string error = avs_is_error(value) ? avs_as_error(value) : "";
  api.avs_release_value(value);
  if (error.empty())
    throw std::runtime_error("expected script error: " + script);
  for (const auto& message : messages)
    if (error.find(message) == std::string::npos)
      throw std::runtime_error("missing diagnostic '" + message + "': " + error);
}
void lut_adapter(AvsApi& api, AVS_ScriptEnvironment* env, const char* backend) {
  std::string suffix = ",backend=\"" + std::string(backend) + "\")";
  std::string blank = "BlankClip(width=18,height=10,length=3,pixel_type=\"YV12\")";
  std::string fixture = "IrisFixture(" + blank + ")";
  auto snapshot = script_clip(api, env, "IrisExpr(" + fixture + ",\"x x.Gain * width + height +\",lut=1" + suffix);
  auto baseline = script_clip(api, env, "Expr(" + fixture + ",\"x x.Gain * width + height +\",lut=1)");
  for (int n : {2, 0, 1}) {
    AvsFrame frame(api, api.avs_get_frame(snapshot->clip, n));
    AvsFrame reference(api, api.avs_get_frame(baseline->clip, n));
    if (!frame.frame || !reference.frame)
      throw std::runtime_error("LUT snapshot frame failed");
    int error = 0;
    auto gain = api.avs_prop_get_float(env, api.avs_get_frame_props_ro(env, frame.frame), "Gain", 0, &error);
    if (error || gain != n + 1)
      throw std::runtime_error("LUT changed inherited frame properties");
    for (int p = 0; p < 3; ++p) {
      int w = p ? 9 : 18, h = p ? 5 : 10;
      auto* data = api.avs_get_read_ptr_p(frame.frame, planes[p]);
      auto* ref = api.avs_get_read_ptr_p(reference.frame, planes[p]);
      int pitch = api.avs_get_pitch_p(frame.frame, planes[p]);
      int ref_pitch = api.avs_get_pitch_p(reference.frame, planes[p]);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          if (data[y * pitch + x] != 3 + p * 7 + x + y + w + h || data[y * pitch + x] != ref[y * ref_pitch + x])
            throw std::runtime_error("LUT snapshot/geometry/baseline mismatch");
    }
  }
  auto property_only =
      script_clip(api, env, "IrisExpr(IrisForbidden(" + blank + ")," + fixture + ",\"y.Gain\",lut=2" + suffix);
  for (int n : {2, 1, 0})
    check_uniform(api, property_only->clip, {1, 1, 1}, n);
  auto constant = script_clip(api, env, "IrisExpr(IrisForbidden(" + blank + "),\"42\",lut=1" + suffix);
  check_uniform(api, constant->clip, {42, 42, 42});
  auto missing = script_clip(api, env, "IrisExpr(" + fixture + ",\"x.Text x.Missing + x.Integer +\",lut=1" + suffix);
  check_uniform(api, missing->clip, {7, 7, 7}, 2);
  for (const char* type : {"Y8", "Y10", "Y12", "Y14", "Y16", "RGBAP", "YUVA444P16"}) {
    auto source = "Expr(BlankClip(width=7,height=3,pixel_type=\"" + std::string(type) + "\"),\"100\")";
    auto clip = script_clip(api, env, "IrisExpr(" + source + ",\"x 2 *\",lut=1" + suffix);
    int count = api.avs_num_components(api.avs_get_video_info(clip->clip));
    std::vector<float> expected(size_t(count), 200);
    if (count == 4) {
      // Make the original alpha explicit, then verify Iris's omitted-alpha copy.
      source =
          "Expr(BlankClip(width=7,height=3,pixel_type=\"" + std::string(type) + "\"),\"100\",\"100\",\"100\",\"50\")";
      clip = script_clip(api, env, "IrisExpr(" + source + ",\"x 2 *\",lut=1" + suffix);
      expected[3] = 50;
    }
    check_uniform(api, clip->clip, expected);
    auto copied = script_clip(api, env, "IrisExpr(" + source + ",\"\",lut=1" + suffix);
    std::fill(expected.begin(), expected.end(), 100.0f);
    if (count == 4)
      expected[3] = 50;
    check_uniform(api, copied->clip, expected);
  }
  std::string ten = "Expr(BlankClip(width=7,height=3,pixel_type=\"YUV444P10\"),\"100\")";
  for (int budget : {6, -1}) {
    auto clip = script_clip(
        api, env, "IrisExpr(" + ten + "," + ten + ",\"x y +\",lut=2,lut_max_mb=" + std::to_string(budget) + suffix);
    check_uniform(api, clip->clip, {200, 200, 200});
  }
  expect_script_error(api, env, "IrisExpr(" + ten + "," + ten + ",\"x y +\",lut=2,lut_max_mb=5" + suffix,
                      {"6291456 bytes", "lut_max_mb=5", "lut_max_mb=6", "lut_max_mb=-1"});
  std::string large = "IrisForbidden(BlankClip(width=7,height=3,pixel_type=\"Y16\"))";
  expect_script_error(api, env, "IrisExpr(" + large + "," + large + ",\"x y + x.Gain +\",lut=2" + suffix,
                      {"8589934592 bytes", "lut_max_mb=256", "lut_max_mb=8192", "lut_max_mb=-1"});
  auto small_large = script_clip(api, env, "IrisExpr(" + large + "," + large + ",\"7\",lut=2,lut_max_mb=1" + suffix);
  check_uniform(api, small_large->clip, {7});
  std::string eight = "Expr(BlankClip(width=7,height=3,pixel_type=\"Y8\"),\"200\")";
  std::string twelve = "Expr(BlankClip(width=7,height=3,pixel_type=\"Y12\"),\"1000\")";
  auto mixed = script_clip(api, env, "IrisExpr(" + eight + "," + twelve + ",\"x y -\",format=\"Y32\",lut=2" + suffix);
  check_uniform(api, mixed->clip, {-800});
  for (const char* expression : {"sx", "sy", "sxr", "syr", "frameno", "time", "x[0,0]", "time unused^ x"})
    expect_script_error(api, env, "IrisExpr(" + blank + ",\"" + expression + "\",lut=1" + suffix,
                        {"manual LUT does not support"});
  expect_script_error(api, env, "IrisExpr(" + blank + ",\"x\",lut=2" + suffix, {"input clip count"});
  expect_script_error(api, env, "IrisExpr(" + blank + ",\"x\",lut=3" + suffix, {"lut must be"});
  for (int budget : {0, -2})
    expect_script_error(api, env, "IrisExpr(" + blank + ",\"x\",lut=1,lut_max_mb=" + std::to_string(budget) + suffix,
                        {"lut_max_mb must be"});
  expect_script_error(api, env, "IrisExpr(BlankClip(pixel_type=\"Y32\"),\"x\",lut=1" + suffix,
                      {"manual LUT requires integer inputs"});
  expect_script_error(api, env, "IrisExpr(IrisForbidden(" + blank + "),\"x.Gain\",lut=1" + suffix,
                      {"LUT frame 0 property snapshot failed"});
  auto corrected = script_clip(api, env, "IrisExpr(" + blank + ",\"cmin 2 *\",scale_inputs=\"floatUV\",lut=1" + suffix);
  check_uniform(api, corrected->clip, {32, 32, 32});
#ifndef IRIS_TEST_LLVM
  expect_script_error(api, env, "IrisExpr(" + blank + ",\"x\",lut=1,backend=\"llvm\")", {"LLVM was disabled"});
#endif
  auto prefetched = script_clip(api, env, "IrisExpr(" + fixture + ",\"x.Gain\",lut=1" + suffix + ".Prefetch(4)");
  for (int n : {2, 0, 1})
    check_uniform(api, prefetched->clip, {1, 1, 1}, n);
  std::cout << "Manual LUT plugin snapshots, formats, aggregate budgets and diagnostics passed: " << backend << '\n';
}
void expr_lifecycle(AvsApi& api, AVS_ScriptEnvironment* env, const char* backend) {
  std::string suffix = std::string(",backend=\"") + backend + "\")";
  std::string source = "Expr(BlankClip(width=18,height=10,length=64,pixel_type=\"YV12\"),\"7\")";
  auto survivor = script_clip(api, env, "IrisExpr(" + source + ",\"x 3 * frameno +\"" + suffix);
  for (int iteration = 0; iteration < 16; ++iteration) {
    // Fail after an earlier plane has compiled, then construct and destroy both
    // normal and LUT filters while an independent JIT filter remains alive.
    auto failed = api.avs_invoke(
        env, "Eval", make_avs_string(("IrisExpr(" + source + ",\"x 2 *\",\"x +\"" + suffix).c_str()), nullptr);
    bool rejected = avs_is_error(failed);
    api.avs_release_value(failed);
    if (!rejected)
      throw std::runtime_error("lifecycle malformed second plane was accepted");
    {
      auto compute = script_clip(api, env, "IrisExpr(" + source + ",\"x 2 * frameno +\"" + suffix);
      auto table = script_clip(api, env, "IrisExpr(" + source + "," + source + ",\"x y + 3 +\",lut=2" + suffix);
      for (int n : {iteration + 2, iteration, iteration + 1}) {
        check_uniform(api, compute->clip, {float(14 + n), float(14 + n), float(14 + n)}, n);
        check_uniform(api, table->clip, {17, 17, 17}, n);
      }
      // Release ordinary JIT state before the table, then use the table again.
      compute.reset();
      check_uniform(api, table->clip, {17, 17, 17}, iteration + 3);
    }
    check_uniform(api, survivor->clip, {float(21 + iteration), float(21 + iteration), float(21 + iteration)},
                  iteration);
  }
  std::cout << "AVS partial construction recovery and repeated JIT/LUT lifetime checks passed: " << backend << '\n';
}
} // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc != 2 && argc != 3)
      throw std::runtime_error("expected absolute avisynth.dll path and optional plugin path");
    AvsApi api(argv[1]);
    auto* env = api.avs_create_script_environment(8);
    if (!env)
      throw std::runtime_error("cannot create AVS environment v8");
    try {
      register_iris_avs(api, env);
      if (argc == 3) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, nullptr, 0, nullptr, nullptr);
        if (!bytes)
          throw std::runtime_error("plugin path encoding failed");
        std::string path(size_t(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, path.data(), bytes, nullptr, nullptr);
        auto result = api.avs_invoke(env, "LoadPlugin", make_avs_string(path.c_str()), nullptr);
        if (avs_is_error(result)) {
          std::string error = avs_as_error(result);
          api.avs_release_value(result);
          throw std::runtime_error(error);
        }
        api.avs_release_value(result);
      } else
        register_iris_expr_avs(api, env);
      if (api.avs_add_function(env, "IrisFixture", "c", fixture_create, &api) != 0)
        throw std::runtime_error("register fixture failed");
      if (api.avs_add_function(env, "IrisForbidden", "c", forbidden_create, &api) != 0)
        throw std::runtime_error("register forbidden fixture failed");
      errors(api, env);
      property_contract(api, env, nullptr);
      property_contract(api, env, "scalar");
      test(api, env, "scalar", false);
      test(api, env, "scalar", true);
      expr_adapter(api, env, "scalar");
      lut_adapter(api, env, "scalar");
      expr_lifecycle(api, env, "scalar");
#ifdef IRIS_TEST_LLVM
      property_contract(api, env, "llvm");
      test(api, env, "llvm", false);
      test(api, env, "llvm", true);
      expr_adapter(api, env, "llvm");
      lut_adapter(api, env, "llvm");
      expr_lifecycle(api, env, "llvm");
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
