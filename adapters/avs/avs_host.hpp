#pragma once
// Test-host loader. All resolved functions are the SDK's C API types.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define AVSC_NO_DECLSPEC
// The C SDK's inline helpers use malloc/free without including their header.
#include <cstdlib>
#include <avisynth_c.h>
#include <stdexcept>
#include <string>

// Keep the symbol table flat; adjacent macro invocations are not expressions.
// clang-format off
#define IRIS_AVS_FUNCTIONS(X) \
  X(avs_create_script_environment) \
  X(avs_delete_script_environment) \
  X(avs_add_function) \
  X(avs_invoke) \
  X(avs_release_value) \
  X(avs_take_clip) \
  X(avs_release_clip) \
  X(avs_set_to_clip) \
  X(avs_get_video_info) \
  X(avs_new_c_filter) \
  X(avs_get_frame) \
  X(avs_release_video_frame) \
  X(avs_clip_get_error) \
  X(avs_save_string) \
  X(avs_get_pitch_p) \
  X(avs_get_row_size_p) \
  X(avs_get_height_p) \
  X(avs_get_read_ptr_p) \
  X(avs_get_write_ptr_p) \
  X(avs_new_video_frame_p) \
  X(avs_new_video_frame_a) \
  X(avs_num_components) \
  X(avs_bits_per_component) \
  X(avs_get_plane_width_subsampling) \
  X(avs_get_plane_height_subsampling) \
  X(avs_get_audio) \
  X(avs_get_frame_props_ro) \
  X(avs_get_frame_props_rw) \
  X(avs_prop_get_type) \
  X(avs_prop_get_float) \
  X(avs_prop_get_float_saturated) \
  X(avs_prop_get_int) \
  X(avs_prop_set_int) \
  X(avs_prop_set_data) \
  X(avs_prop_set_float)
// clang-format on

// Initialize the whole C value, including fields unused by string/error values.
inline AVS_Value make_avs_string(const char* text) {
  AVS_Value value{};
  value.type = 's';
  value.d.string = text;
  return value;
}
inline AVS_Value make_avs_error(const char* text) {
  AVS_Value value = make_avs_string(text);
  value.type = 'e';
  return value;
}

struct AvsApi {
  HMODULE handle = nullptr;
  bool owns_handle = true;
#define FIELD(name) name##_func name = nullptr;
  IRIS_AVS_FUNCTIONS(FIELD)
#undef FIELD
  explicit AvsApi(const wchar_t* path) {
    handle = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!handle)
      throw std::runtime_error("cannot load specified AVS runtime, Windows error " + std::to_string(GetLastError()));
    try {
      resolve();
    } catch (...) {
      FreeLibrary(handle);
      handle = nullptr;
      throw;
    }
  }
  // Plugins borrow the runtime already loaded by their host.
  explicit AvsApi(HMODULE module) : handle(module), owns_handle(false) {
    if (!handle)
      throw std::runtime_error("AviSynth runtime is not loaded in this process");
    resolve();
  }
  void resolve() {
#define LOAD(name)                                                                                                     \
  name = reinterpret_cast<name##_func>(GetProcAddress(handle, #name));                                                 \
  if (!name)                                                                                                           \
    throw std::runtime_error("AVS missing " #name);
    IRIS_AVS_FUNCTIONS(LOAD)
#undef LOAD
  }
  ~AvsApi() {
    if (handle && owns_handle)
      FreeLibrary(handle);
  }
  AvsApi(const AvsApi&) = delete;
};
struct AvsFrame {
  AvsApi& api;
  AVS_VideoFrame* frame;
  AvsFrame(AvsApi& a, AVS_VideoFrame* f) : api(a), frame(f) {}
  ~AvsFrame() {
    if (frame)
      api.avs_release_video_frame(frame);
  }
  AvsFrame(const AvsFrame&) = delete;
  AVS_VideoFrame* release() {
    auto* p = frame;
    frame = nullptr;
    return p;
  }
};
struct AvsClip {
  AvsApi& api;
  AVS_Clip* clip;
  AvsClip(AvsApi& a, AVS_Clip* c) : api(a), clip(c) {}
  ~AvsClip() {
    if (clip)
      api.avs_release_clip(clip);
  }
  AvsClip(const AvsClip&) = delete;
};
void register_iris_avs(AvsApi&, AVS_ScriptEnvironment*);
void register_iris_expr_avs(AvsApi&, AVS_ScriptEnvironment*);
