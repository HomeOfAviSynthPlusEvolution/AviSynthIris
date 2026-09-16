#include "avs_host.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Fixture {
  AvsApi& api;
  uint64_t calls = 0;
  bool random = false;
};
uint32_t sample(int x, int y, int n, int seed, int bits, bool random) {
  uint32_t value = uint32_t(x) + uint32_t(y) * 17u + uint32_t(n) * 13u + uint32_t(seed) * 97u;
  if (random) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
  }
  return value & ((1u << bits) - 1);
}
AVS_VideoFrame* AVSC_CC fixture_frame(AVS_FilterInfo* fi, int n) {
  auto& fixture = *static_cast<Fixture*>(fi->user_data);
  auto& api = fixture.api;
  ++fixture.calls;
  AvsFrame frame(api, api.avs_new_video_frame_a(fi->env, &fi->vi, 64));
  if (!frame.frame) {
    fi->error = "benchmark source allocation failed";
    return nullptr;
  }
  int bits = api.avs_bits_per_component(&fi->vi);
  int bytes = bits == 8 ? 1 : 2;
  auto* data = api.avs_get_write_ptr_p(frame.frame, AVS_PLANAR_Y);
  auto pitch = api.avs_get_pitch_p(frame.frame, AVS_PLANAR_Y);
  // The two sources use different frame counts as a fixed seed.
  int seed = fi->vi.num_frames & 1;
  for (int y = 0; y < fi->vi.height; ++y)
    for (int x = 0; x < fi->vi.width; ++x) {
      uint16_t value = uint16_t(sample(x, y, n, seed, bits, fixture.random));
      std::memcpy(data + ptrdiff_t(y) * pitch + x * bytes, &value, size_t(bytes));
    }
  return frame.release();
}
AVS_Value AVSC_CC fixture_create(AVS_ScriptEnvironment* env, AVS_Value args, void* user) {
  auto& fixture = *static_cast<Fixture*>(user);
  auto& api = fixture.api;
  AVS_FilterInfo* fi = nullptr;
  AvsClip clip(api, api.avs_new_c_filter(env, &fi, avs_array_elt(args, 0), 1));
  if (!clip.clip)
    return make_avs_error("benchmark source creation failed");
  fi->user_data = user;
  fi->get_frame = fixture_frame;
  AVS_Value result{};
  api.avs_set_to_clip(&result, clip.clip);
  return result;
}
AVS_Value invoke(AvsApi& api, AVS_ScriptEnvironment* env, const char* function, const char* text) {
  auto result = api.avs_invoke(env, function, make_avs_string(text), nullptr);
  if (avs_is_error(result)) {
    std::string error = avs_as_error(result);
    api.avs_release_value(result);
    throw std::runtime_error(error);
  }
  return result;
}
std::unique_ptr<AvsClip> eval(AvsApi& api, AVS_ScriptEnvironment* env, const std::string& script) {
  auto result = invoke(api, env, "Eval", script.c_str());
  auto clip = std::make_unique<AvsClip>(api, api.avs_take_clip(result, env));
  api.avs_release_value(result);
  if (!clip->clip)
    throw std::runtime_error("benchmark script returned no clip");
  return clip;
}
double elapsed(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
uint64_t read_frame(AvsApi& api, AVS_Clip* clip, int n, int bits, bool random, int formula, bool verify) {
  AvsFrame frame(api, api.avs_get_frame(clip, n));
  if (!frame.frame) {
    auto* error = api.avs_clip_get_error(clip);
    throw std::runtime_error(error ? error : "benchmark GetFrame failed");
  }
  int bytes = bits == 8 ? 1 : 2;
  int width = api.avs_get_row_size_p(frame.frame, AVS_PLANAR_Y) / bytes;
  int height = api.avs_get_height_p(frame.frame, AVS_PLANAR_Y);
  auto* data = api.avs_get_read_ptr_p(frame.frame, AVS_PLANAR_Y);
  int pitch = api.avs_get_pitch_p(frame.frame, AVS_PLANAR_Y);
  uint64_t checksum = 0;
  int count = verify ? 1024 : 1;
  for (int i = 0; i < count; ++i) {
    int x = int((uint64_t(i) * 7919 + uint32_t(n)) % uint32_t(width));
    int y = int((uint64_t(i) * 3571 + uint32_t(n)) % uint32_t(height));
    uint16_t actual = 0;
    std::memcpy(&actual, data + ptrdiff_t(y) * pitch + x * bytes, size_t(bytes));
    if (verify) {
      auto a = sample(x, y, n, 0, bits, random), b = sample(x, y, n, 1, bits, random);
      auto expected = formula == 0 ? (a + b + 1) / 2 : std::max(a, b) / 2;
      if (actual != expected)
        throw std::runtime_error("benchmark pixel mismatch at frame " + std::to_string(n) + ": expected " +
                                 std::to_string(expected) + " got " + std::to_string(actual));
    }
    checksum += actual;
  }
  return checksum;
}
void run(AvsApi& api, AVS_ScriptEnvironment* env, Fixture& fixture, bool quick) {
  int width = quick ? 257 : 1280, height = quick ? 17 : 720;
  const char* formulas[] = {"x y + 0.5 * 0.25 +", "x y - abs x y min + x y max + 0.25 * floor"};
  std::vector<std::string> modes = {"source_pair", "original_default", "original_lut", "iris_scalar", "iris_lut"};
#ifdef IRIS_TEST_LLVM
  modes.push_back("iris_llvm");
#endif
  std::cout << "bits,pattern,formula,mode,width,height,create_ms,first_frame_ms,min_ms,median_ms,max_ms,frames,source_"
               "calls,checksum\n";
  for (int bits : {8, 10, 12})
    for (bool random : {false, true}) {
      fixture.random = random;
      std::string base = "IrisBenchSource(BlankClip(width=" + std::to_string(width) +
                         ",height=" + std::to_string(height) + ",pixel_type=\"Y" + std::to_string(bits) + "\",length=";
      auto a = eval(api, env, "global bench_a=" + base + "1000000))\nreturn bench_a");
      auto b = eval(api, env, "global bench_b=" + base + "1000001))\nreturn bench_b");
      int next_frame = 0;
      for (int formula = 0; formula < 2; ++formula)
        for (const auto& mode : modes) {
          bool source = mode == "source_pair", original = mode.find("original") == 0;
          auto start = Clock::now();
          std::unique_ptr<AvsClip> out;
          if (!source) {
            std::string script =
                std::string(original ? "Expr" : "IrisExpr") + "(bench_a,bench_b,\"" + formulas[formula] + "\"";
            if (original)
              script += mode == "original_lut" ? ",lut=2" : ",lut=0";
            else
              script += std::string(",backend=\"") + (mode == "iris_llvm" ? "llvm" : "scalar") +
                        "\",lut=" + (mode == "iris_lut" ? "2" : "0");
            out = eval(api, env, script + ")");
          }
          double create_ms = elapsed(start);
          auto calls_before = fixture.calls;
          uint64_t checksum = 0, frames = 0;
          auto get = [&](bool verify) {
            if (next_frame >= 1000000)
              throw std::runtime_error("benchmark exhausted distinct frame numbers");
            int n = next_frame++;
            if (source) {
              checksum += read_frame(api, a->clip, n, bits, random, formula, false);
              checksum += read_frame(api, b->clip, n, bits, random, formula, false);
            } else {
              checksum += read_frame(api, out->clip, n, bits, random, formula, verify);
            }
            ++frames;
          };
          start = Clock::now();
          get(false);
          double first_ms = elapsed(start);
          get(true);
          std::vector<double> timings;
          for (int repetition = 0; repetition < 5; ++repetition) {
            uint64_t count = 0;
            start = Clock::now();
            do {
              get(false);
              ++count;
            } while (elapsed(start) < (quick ? 2.0 : 100.0));
            timings.push_back(elapsed(start) / double(count));
          }
          get(true);
          auto calls = fixture.calls - calls_before;
          if (calls != frames * 2)
            throw std::runtime_error("benchmark input cache/call-count mismatch");
          std::sort(timings.begin(), timings.end());
          std::cout << bits << ',' << (random ? "random" : "spatial") << ',' << formula << ',' << mode << ',' << width
                    << ',' << height << ',' << create_ms << ',' << first_ms << ',' << timings.front() << ','
                    << timings[2] << ',' << timings.back() << ',' << frames << ',' << calls << ',' << checksum << '\n';
        }
    }
}
} // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc != 3 && !(argc == 4 && std::wstring(argv[3]) == L"--quick"))
      throw std::runtime_error("usage: iris_avs_benchmark <avisynth.dll> <IrisExpr.dll> [--quick]");
    AvsApi api(argv[1]);
    auto* env = api.avs_create_script_environment(8);
    if (!env)
      throw std::runtime_error("cannot create AVS environment");
    Fixture fixture{api};
    try {
      int length = WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, nullptr, 0, nullptr, nullptr);
      if (!length)
        throw std::runtime_error("plugin path conversion failed");
      std::string path(size_t(length), '\0');
      WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, path.data(), length, nullptr, nullptr);
      api.avs_release_value(invoke(api, env, "LoadPlugin", path.c_str()));
      if (api.avs_add_function(env, "IrisBenchSource", "c", fixture_create, &fixture))
        throw std::runtime_error("benchmark source registration failed");
      std::cout << std::fixed << std::setprecision(6);
      run(api, env, fixture, argc == 4);
    } catch (...) {
      api.avs_delete_script_environment(env);
      throw;
    }
    api.avs_delete_script_environment(env);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "AVS benchmark failed: " << e.what() << '\n';
    return 1;
  }
}
