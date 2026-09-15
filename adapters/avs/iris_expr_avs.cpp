#include "avs_host.hpp"
#include <iris/iris.h>
#include <array>
#include <memory>
#include <vector>
#include <cstring>

namespace {
struct PlanDelete {
  void operator()(iris_plan* p) const { iris_plan_destroy(p); }
};
struct ContextDelete {
  void operator()(iris_context* p) const { iris_context_destroy(p); }
};
struct Plane {
  std::unique_ptr<iris_plan, PlanDelete> plan;
  iris_plan_info info{};
  std::vector<iris_property_dependency> properties;
  int id = 0;
  bool copy = false;
};
struct Filter {
  AvsApi& api;
  std::vector<std::unique_ptr<AvsClip>> inputs;
  std::vector<AVS_VideoInfo> formats;
  std::array<Plane, 4> planes;
  int count = 0;
  uint32_t used = 0;
  explicit Filter(AvsApi& a) : api(a) {}
};
void check(iris_status status, const iris_diagnostic& d) {
  if (status != IRIS_OK)
    throw std::runtime_error(std::string("IrisExpr: ") + d.message);
}
int plane_id(const AVS_VideoInfo& vi, int index) {
  constexpr int yuv[] = {AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V, AVS_PLANAR_A};
  constexpr int rgb[] = {AVS_PLANAR_R, AVS_PLANAR_G, AVS_PLANAR_B, AVS_PLANAR_A};
  return avs_is_rgb(&vi) ? rgb[index] : yuv[index];
}
iris_format format(AvsApi& api, const AVS_VideoInfo& vi) {
  auto bits = uint32_t(api.avs_bits_per_component(&vi));
  return {bits == 8 ? IRIS_U8 : bits == 32 ? IRIS_F32 : IRIS_U16, bits};
}
uint32_t width(AvsApi& api, const AVS_VideoInfo& vi, int plane) {
  return uint32_t(vi.width >> api.avs_get_plane_width_subsampling(&vi, plane));
}
uint32_t height(AvsApi& api, const AVS_VideoInfo& vi, int plane) {
  return uint32_t(vi.height >> api.avs_get_plane_height_subsampling(&vi, plane));
}
int input_plane(Filter& f, size_t input, int output_index) {
  const auto& vi = f.formats[input];
  auto components = f.api.avs_num_components(&vi);
  if (components == 1)
    return AVS_PLANAR_Y;
  if (output_index >= components)
    throw std::runtime_error("IrisExpr: expression references a missing input plane");
  return plane_id(vi, output_index);
}
AVS_VideoFrame* AVSC_CC get_frame(AVS_FilterInfo* fi, int n) {
  auto& f = *static_cast<Filter*>(fi->user_data);
  auto& api = f.api;
  try {
    std::vector<std::unique_ptr<AvsFrame>> sources(f.inputs.size());
    AVS_VideoFrame* property_source = nullptr;
    for (size_t i = 0; i < f.inputs.size(); ++i)
      if (f.used & (1u << i)) {
        sources[i] = std::make_unique<AvsFrame>(api, api.avs_get_frame(f.inputs[i]->clip, n));
        if (!sources[i]->frame)
          throw std::runtime_error("IrisExpr: input frame failed: " + std::to_string(i));
        if (!property_source)
          property_source = sources[i]->frame;
      }
    AvsFrame output(api, property_source ? api.avs_new_video_frame_p(fi->env, &fi->vi, property_source)
                                         : api.avs_new_video_frame_a(fi->env, &fi->vi, AVS_FRAME_ALIGN));
    if (!output.frame)
      throw std::runtime_error("IrisExpr: output allocation failed");
    for (int i = 0; i < f.count; ++i) {
      auto& plane = f.planes[i];
      auto* dst = api.avs_get_write_ptr_p(output.frame, plane.id);
      auto pitch = api.avs_get_pitch_p(output.frame, plane.id);
      if (plane.copy) {
        int source_plane = input_plane(f, 0, i);
        auto* src = api.avs_get_read_ptr_p(sources[0]->frame, source_plane);
        int src_pitch = api.avs_get_pitch_p(sources[0]->frame, source_plane);
        for (int y = 0; y < api.avs_get_height_p(output.frame, plane.id); ++y)
          std::memcpy(dst + ptrdiff_t(y) * pitch, src + ptrdiff_t(y) * src_pitch,
                      size_t(api.avs_get_row_size_p(output.frame, plane.id)));
        continue;
      }
      std::vector<float> properties(plane.properties.size());
      for (size_t slot = 0; slot < properties.size(); ++slot) {
        const auto& dep = plane.properties[slot];
        const auto* map = api.avs_get_frame_props_ro(fi->env, sources[dep.input]->frame);
        char type = api.avs_prop_get_type(fi->env, map, dep.name);
        int error = 0;
        if (type == 'f')
          properties[slot] = api.avs_prop_get_float_saturated(fi->env, map, dep.name, 0, &error);
        else if (type == 'i')
          properties[slot] = float(api.avs_prop_get_int(fi->env, map, dep.name, 0, &error));
        if (error)
          properties[slot] = 0;
      }
      iris_diagnostic d{};
      iris_context* raw = nullptr;
      check(iris_context_create(plane.plan.get(), &raw, &d), d);
      std::unique_ptr<iris_context, ContextDelete> context(raw);
      iris_execute_args_v1 a{};
      a.struct_size = sizeof(a);
      a.input_count = uint32_t(f.inputs.size());
      a.frameno = uint64_t(n);
      a.output = {dst, pitch};
      a.properties = properties.data();
      a.property_count = properties.size();
      for (size_t j = 0; j < f.inputs.size(); ++j)
        if (plane.info.input_mask & (1u << j)) {
          auto id = input_plane(f, j, i);
          a.inputs[j] = {api.avs_get_read_ptr_p(sources[j]->frame, id), api.avs_get_pitch_p(sources[j]->frame, id)};
        }
      check(iris_execute_v1(plane.plan.get(), context.get(), &a, &d), d);
    }
    return output.release();
  } catch (const std::exception& e) {
    fi->error = api.avs_save_string(fi->env, e.what(), -1);
    return nullptr;
  } catch (...) {
    fi->error = "IrisExpr: unexpected frame exception";
    return nullptr;
  }
}
void AVSC_CC destroy(AVS_FilterInfo* fi) {
  delete static_cast<Filter*>(fi->user_data);
}
int AVSC_CC cache(AVS_FilterInfo*, int hint, int) {
  return hint == AVS_CACHE_GET_MTMODE ? AVS_MT_SERIALIZED : 0;
}
std::string string_option(AVS_Value args, int index, const char* fallback) {
  auto value = avs_array_elt(args, index);
  return avs_defined(value) ? avs_as_string(value) : fallback;
}
bool bool_option(AVS_Value args, int index, bool fallback) {
  auto value = avs_array_elt(args, index);
  return avs_defined(value) ? avs_as_bool(value) != 0 : fallback;
}
AVS_Value AVSC_CC create(AVS_ScriptEnvironment* env, AVS_Value args, void* user) {
  auto& api = *static_cast<AvsApi*>(user);
  try {
    auto f = std::make_unique<Filter>(api);
    auto clips = avs_array_elt(args, 0);
    auto expressions = avs_array_elt(args, 1);
    int inputs = avs_array_size(clips), expr_count = avs_array_size(expressions);
    if (inputs < 1 || inputs > 26)
      throw std::runtime_error("IrisExpr: expected 1..26 clips");
    for (int i = 0; i < inputs; ++i) {
      auto clip = std::make_unique<AvsClip>(api, api.avs_take_clip(avs_array_elt(clips, i), env));
      if (!clip->clip)
        throw std::runtime_error("IrisExpr: invalid input clip");
      auto vi = *api.avs_get_video_info(clip->clip);
      if (!avs_is_planar(&vi))
        throw std::runtime_error("IrisExpr: planar input required");
      if (!f->formats.empty()) {
        const auto& first = f->formats[0];
        if (vi.width != first.width || vi.height != first.height ||
            api.avs_num_components(&vi) != api.avs_num_components(&first))
          throw std::runtime_error("IrisExpr: input dimensions and plane counts must match");
        for (int p = 0; p < api.avs_num_components(&vi); ++p)
          if (width(api, vi, plane_id(vi, p)) != width(api, first, plane_id(first, p)) ||
              height(api, vi, plane_id(vi, p)) != height(api, first, plane_id(first, p)))
            throw std::runtime_error("IrisExpr: input subsampling must match");
      }
      f->formats.push_back(vi);
      f->inputs.push_back(std::move(clip));
    }
    auto vi = f->formats[0];
    auto output_format = string_option(args, 2, "");
    if (!output_format.empty()) {
      AVS_Value values[] = {avs_array_elt(clips, 0), make_avs_string(output_format.c_str())};
      const char* names[] = {nullptr, "pixel_type"};
      auto result = api.avs_invoke(env, "BlankClip", avs_new_value_array(values, 2), names);
      if (avs_is_error(result)) {
        std::string error = avs_as_error(result);
        api.avs_release_value(result);
        throw std::runtime_error(error);
      }
      AvsClip converted(api, api.avs_take_clip(result, env));
      api.avs_release_value(result);
      if (!converted.clip)
        throw std::runtime_error("IrisExpr: invalid output format");
      vi.pixel_type = api.avs_get_video_info(converted.clip)->pixel_type;
    }
    if (!avs_is_planar(&vi))
      throw std::runtime_error("IrisExpr: planar output required");
    f->count = api.avs_num_components(&vi);
    if (f->count < 1 || f->count > 4 || expr_count < 1 || expr_count > f->count)
      throw std::runtime_error("IrisExpr: invalid expression count");
    auto backend_name = string_option(args, 3, "scalar");
    if (backend_name != "scalar" && backend_name != "llvm")
      throw std::runtime_error("IrisExpr: unknown backend");
    auto scaling = string_option(args, 4, "none");
    for (auto& c : scaling)
      if (c >= 'A' && c <= 'Z')
        c = char(c - 'A' + 'a');
    const char* modes[] = {"none", "all", "allf", "int", "intf", "float", "floatf", "floatuv"};
    int mode = 0;
    while (mode < 8 && scaling != modes[mode])
      ++mode;
    if (mode == 8)
      throw std::runtime_error("IrisExpr: unknown scale_inputs mode");
    for (int p = 0; p < f->count; ++p) {
      auto& plane = f->planes[p];
      plane.id = plane_id(vi, p);
      std::string expression = p < expr_count ? avs_as_string(avs_array_elt(expressions, p))
                               : p == 3       ? ""
                                              : avs_as_string(avs_array_elt(expressions, expr_count - 1));
      if (expression.empty() && format(api, vi).bits != format(api, f->formats[0]).bits)
        throw std::runtime_error("IrisExpr: plane " + std::to_string(p) +
                                 " needs an explicit expression when bit depth changes");
      plane.copy = expression.empty();
      iris_compile_options_v1 o{};
      o.struct_size = sizeof(o);
      o.width = width(api, vi, plane.id);
      o.height = height(api, vi, plane.id);
      o.input_count = uint32_t(inputs);
      o.output = format(api, vi);
      o.optimize = bool_option(args, 7, true);
      o.backend = backend_name == "llvm" ? IRIS_BACKEND_LLVM : IRIS_BACKEND_SCALAR;
      for (int j = 0; j < inputs; ++j)
        o.inputs[j] = format(api, f->formats[j]);
      iris_expr_options_v1 e{};
      e.struct_size = sizeof(e);
      e.frame_count = uint64_t(vi.num_frames);
      e.chroma = !avs_is_rgb(&vi) && (p == 1 || p == 2);
      e.scale_inputs = static_cast<iris_scale_inputs>(mode);
      e.clamp_float = bool_option(args, 5, false);
      e.clamp_float_uv = bool_option(args, 6, false);
      if (plane.copy) {
        f->used |= 1;
        // Validate explicit LLVM requests even when all output planes are copied.
        iris_plan* probe = nullptr;
        iris_diagnostic d{};
        auto validation = o;
        validation.optimize = 1;
        check(iris_compile_v1("0", &validation, &probe, &d), d);
        std::unique_ptr<iris_plan, PlanDelete> validated(probe);
        plane.info.input_mask = 1;
      } else {
        iris_plan* raw = nullptr;
        iris_diagnostic d{};
        auto status = iris_compile_expr_v1(expression.c_str(), &o, &e, &raw, &d);
        if (status != IRIS_OK)
          throw std::runtime_error("IrisExpr: plane " + std::to_string(p) + ": " + d.message);
        plane.plan.reset(raw);
        check(iris_plan_get_info(raw, &plane.info, &d), d);
        plane.properties.resize(plane.info.property_count);
        for (size_t slot = 0; slot < plane.properties.size(); ++slot) {
          check(iris_plan_get_property(raw, slot, &plane.properties[slot], &d), d);
          f->used |= 1u << plane.properties[slot].input;
        }
        f->used |= plane.info.input_mask;
      }
      for (int j = 0; j < inputs; ++j) {
        // Geometry is a compile-time contract, even for currently unused inputs.
        int components = api.avs_num_components(&f->formats[j]);
        if (p >= components && components != 1 && !(plane.info.input_mask & (1u << j)))
          continue;
        int source_plane = input_plane(*f, size_t(j), p);
        if (width(api, f->formats[j], source_plane) != o.width || height(api, f->formats[j], source_plane) != o.height)
          throw std::runtime_error("IrisExpr: input and output plane geometry must match");
      }
    }
    AVS_FilterInfo* fi = nullptr;
    AvsClip result(api, api.avs_new_c_filter(env, &fi, avs_array_elt(clips, 0), 1));
    if (!result.clip)
      throw std::runtime_error("IrisExpr: filter allocation failed");
    fi->vi = vi;
    fi->user_data = f.release();
    fi->get_frame = get_frame;
    fi->free_filter = destroy;
    fi->set_cache_hints = cache;
    AVS_Value value{};
    api.avs_set_to_clip(&value, result.clip);
    return value;
  } catch (const std::exception& e) {
    return make_avs_error(api.avs_save_string(env, e.what(), -1));
  } catch (...) {
    return make_avs_error("IrisExpr: unexpected create exception");
  }
}
} // namespace
void register_iris_expr_avs(AvsApi& api, AVS_ScriptEnvironment* env) {
  if (api.avs_add_function(env, "IrisExpr",
                           "c+s+[format]s[backend]s[scale_inputs]s[clamp_float]b[clamp_float_UV]b[optimize]b", create,
                           &api))
    throw std::runtime_error("cannot register IrisExpr");
}
