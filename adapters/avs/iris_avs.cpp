#include "avs_host.hpp"
#include <iris/iris.h>
#include <array>
#include <memory>
#include <vector>

namespace {
constexpr int planes[] = {AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V};
using Plan = std::unique_ptr<iris_plan, decltype(&iris_plan_destroy)>;
using Context = std::unique_ptr<iris_context, decltype(&iris_context_destroy)>;
struct Filter {
  AvsApi& api;
  std::array<Plan, 3> plans{Plan(nullptr, iris_plan_destroy), Plan(nullptr, iris_plan_destroy),
                            Plan(nullptr, iris_plan_destroy)};
  explicit Filter(AvsApi& a) : api(a) {}
};
void ensure(iris_status status, const iris_diagnostic& d) {
  if (status != IRIS_OK)
    throw std::runtime_error(std::string("IrisPoC: ") + d.message);
}
AVS_VideoFrame* AVSC_CC frame(AVS_FilterInfo* fi, int n) {
  auto& f = *static_cast<Filter*>(fi->user_data);
  auto& api = f.api;
  try {
    AvsFrame source(api, api.avs_get_frame(fi->child, n));
    if (!source.frame)
      throw std::runtime_error("IrisPoC: input frame failed");
    AvsFrame output(api, api.avs_new_video_frame_p(fi->env, &fi->vi, source.frame));
    if (!output.frame)
      throw std::runtime_error("IrisPoC: output allocation failed");
    for (size_t i = 0; i < 3; ++i) {
      auto* plan = f.plans[i].get();
      iris_diagnostic d{};
      iris_plan_info info{};
      ensure(iris_plan_get_info(plan, &info, &d), d);
      std::vector<float> properties(info.property_count);
      const auto* map = api.avs_get_frame_props_ro(fi->env, source.frame);
      for (size_t slot = 0; slot < properties.size(); ++slot) {
        iris_property_dependency dep{};
        ensure(iris_plan_get_property(plan, slot, &dep, &d), d);
        int error = 0;
        char type = api.avs_prop_get_type(fi->env, map, dep.name);
        if (type == 'f')
          properties[slot] = api.avs_prop_get_float_saturated(fi->env, map, dep.name, 0, &error);
        else if (type == 'i')
          properties[slot] = float(api.avs_prop_get_int(fi->env, map, dep.name, 0, &error));
        if (error)
          properties[slot] = 0.0f;
      }
      iris_context* raw = nullptr;
      ensure(iris_context_create(plan, &raw, &d), d);
      Context context(raw, iris_context_destroy);
      iris_execute_args args{};
      args.frameno = uint64_t(n);
      args.properties = properties.data();
      args.property_count = properties.size();
      args.inputs[0] = {api.avs_get_read_ptr_p(source.frame, planes[i]), api.avs_get_pitch_p(source.frame, planes[i])};
      args.output = {api.avs_get_write_ptr_p(output.frame, planes[i]), api.avs_get_pitch_p(output.frame, planes[i])};
      ensure(iris_execute(plan, context.get(), &args, &d), d);
    }
    return output.release();
  } catch (const std::exception& e) {
    fi->error = api.avs_save_string(fi->env, e.what(), -1);
    return nullptr;
  } catch (...) {
    fi->error = "IrisPoC: unexpected exception";
    return nullptr;
  }
}
void AVSC_CC destroy(AVS_FilterInfo* fi) {
  delete static_cast<Filter*>(fi->user_data);
}
AVS_Value AVSC_CC create(AVS_ScriptEnvironment* env, AVS_Value args, void* user) {
  auto& api = *static_cast<AvsApi*>(user);
  try {
    auto child_value = avs_array_elt(args, 0);
    AvsClip child(api, api.avs_take_clip(child_value, env));
    if (!child.clip)
      throw std::runtime_error("IrisPoC: expected clip");
    auto vi = *api.avs_get_video_info(child.clip);
    if (vi.pixel_type != AVS_CS_YV12)
      throw std::runtime_error("IrisPoC adapter currently requires 8-bit YUV420 (YV12); actual=" +
                               std::to_string(vi.pixel_type) + ", expected=" + std::to_string(AVS_CS_YV12));
    std::string backend_name = avs_as_string(avs_array_elt(args, 2));
    iris_backend backend = backend_name == "llvm" ? IRIS_BACKEND_LLVM : IRIS_BACKEND_SCALAR;
    if (backend_name != "llvm" && backend_name != "scalar")
      throw std::runtime_error("IrisPoC: unknown backend");
    auto filter = std::make_unique<Filter>(api);
    for (size_t i = 0; i < 3; ++i) {
      iris_compile_options options{};
      options.width = uint32_t(vi.width >> (i ? 1 : 0));
      options.height = uint32_t(vi.height >> (i ? 1 : 0));
      options.input_count = 1;
      options.inputs[0] = options.output = {IRIS_U8, 8};
      options.optimize = 1;
      iris_diagnostic d{};
      iris_plan* raw = nullptr;
      ensure(iris_compile_ex(avs_as_string(avs_array_elt(args, 1)), &options, backend,
                             avs_as_bool(avs_array_elt(args, 3)), &raw, &d),
             d);
      filter->plans[i].reset(raw);
    }
    AVS_FilterInfo* fi = nullptr;
    AvsClip result(api, api.avs_new_c_filter(env, &fi, child_value, 1));
    if (!result.clip)
      throw std::runtime_error("IrisPoC: cannot create C filter");
    fi->vi = vi;
    fi->user_data = filter.release();
    fi->get_frame = frame;
    fi->free_filter = destroy;
    AVS_Value value{};
    api.avs_set_to_clip(&value, result.clip);
    return value;
  } catch (const std::exception& e) {
    return make_avs_error(api.avs_save_string(env, e.what(), -1));
  } catch (...) {
    return make_avs_error("IrisPoC: unexpected create exception");
  }
}
} // namespace
void register_iris_avs(AvsApi& api, AVS_ScriptEnvironment* env) {
  // Required arguments keep the small adapter's defaults unambiguous.
  if (api.avs_add_function(env, "IrisPoC", "cssb", create, &api) != 0)
    throw std::runtime_error("cannot register IrisPoC");
}
