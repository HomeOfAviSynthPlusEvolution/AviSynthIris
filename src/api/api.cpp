#include "runtime/runtime.hpp"
#include <cstdio>
#include <cstring>
#include <new>
namespace {
template <class F>
iris_status guard(iris_diagnostic* d, F f) noexcept {
  if (d)
    *d = {};
  try {
    f();
    return IRIS_OK;
  } catch (const iris::Error& e) {
    if (d) {
      d->offset = e.offset;
      d->length = e.length;
      std::snprintf(d->message, sizeof d->message, "%s", e.what());
    }
    return e.status;
  } catch (const std::bad_alloc&) {
    if (d)
      std::snprintf(d->message, sizeof d->message, "allocation failed");
    return IRIS_OUT_OF_MEMORY;
  } catch (...) {
    if (d)
      std::snprintf(d->message, sizeof d->message, "unexpected internal error");
    return IRIS_INTERNAL_ERROR;
  }
}
} // namespace
extern "C" {
iris_status iris_compile(const char* s, const iris_compile_options* o, iris_plan** out, iris_diagnostic* d) {
  return iris_compile_ex(s, o, IRIS_BACKEND_SCALAR, 0, out, d);
}
static iris_status compile_normalized(const char* s, const iris::CompileOptions* o, iris_backend backend,
                                      int enable_lut, iris_plan** out, iris_diagnostic* d) {
  if (out)
    *out = nullptr;
  return guard(d, [&] {
    if (!s || !o || !out)
      iris::fail("null compile argument");
    if ((backend != IRIS_BACKEND_SCALAR && !iris::uses_llvm(backend)) || (enable_lut != 0 && enable_lut != 1))
      iris::fail("invalid backend or LUT option");
    if (iris::uses_llvm(backend) && !iris::llvm_available())
      throw iris::Error(IRIS_BACKEND_UNAVAILABLE, "LLVM was disabled at build time; selected backend requires LLVM");
    const auto math = backend == IRIS_BACKEND_SLEEF        ? iris::IRIS_MATH_ACCURATE
                      : backend == IRIS_BACKEND_SLEEF_FAST ? iris::IRIS_MATH_FAST
                                                           : iris::IRIS_MATH_NATIVE;
    if (!iris::math_available(math))
      throw iris::Error(IRIS_BACKEND_UNAVAILABLE, "selected backend requires a build with SLEEF");
    iris::validate_options(*o);
    size_t len = 0;
    while (len <= iris::max_source && s[len])
      ++len;
    if (len > iris::max_source)
      throw iris::Error(IRIS_LIMIT_EXCEEDED, "expression length limit exceeded");
    auto p = std::make_shared<iris::Program>();
    p->options = *o;
    p->options.math = math;
    p->options.backend = backend;
    p->ir = iris::parse(std::string(s, len), o->input_count, o->extended_inputs, o->inputs,
                        o->expr.struct_size ? &o->expr : nullptr);
    iris::verify(p->ir);
    if (o->optimize) {
      iris::optimize(p->ir, math);
      iris::verify(p->ir);
    }
    iris::describe(*p);
    if (enable_lut)
      iris::prepare_lut(*p);
    if (iris::uses_llvm(backend) && p->info.strategy == IRIS_COMPUTE)
      p->jit = iris::compile_llvm(*p);
    *out = new iris_plan{std::move(p)};
  });
}
iris_status iris_compile_ex(const char* s, const iris_compile_options* o, iris_backend backend, int enable_lut,
                            iris_plan** out, iris_diagnostic* d) {
  if (!o)
    return compile_normalized(s, nullptr, backend, enable_lut, out, d);
  iris::CompileOptions options(*o);
  return compile_normalized(s, &options, backend, enable_lut, out, d);
}
uint32_t iris_get_api_version(void) {
  return IRIS_API_VERSION;
}
static iris_status compile_v1(const char* s, const iris_compile_options_v1* o, const iris_expr_options_v1* expr,
                              iris_plan** out, iris_diagnostic* d) {
  if (out)
    *out = nullptr;
  iris::CompileOptions options;
  auto status = guard(d, [&] {
    if (!o || o->struct_size != sizeof(*o) || !s || !out)
      iris::fail("invalid v1 compile arguments or structure size");
    options.width = o->width;
    options.height = o->height;
    options.input_count = o->input_count;
    std::copy_n(o->inputs, 26, options.inputs);
    options.output = o->output;
    options.optimize = o->optimize;
    options.extended_inputs = true;
    if (expr) {
      if (expr->struct_size != sizeof(*expr) || !expr->frame_count || (expr->chroma != 0 && expr->chroma != 1) ||
          expr->scale_inputs < IRIS_SCALE_NONE || expr->scale_inputs > IRIS_SCALE_FLOAT_UV ||
          (expr->clamp_float != 0 && expr->clamp_float != 1) ||
          (expr->clamp_float_uv != 0 && expr->clamp_float_uv != 1))
        iris::fail("invalid Expr frontend context");
      options.expr = *expr;
      if (!options.input_count && (expr->scale_inputs != IRIS_SCALE_NONE || expr->clamp_float))
        iris::fail("Expr scaling and clamping require a first input format");
    }
  });
  return status == IRIS_OK ? compile_normalized(s, &options, o->backend, o->enable_lut, out, d) : status;
}
iris_status iris_compile_v1(const char* s, const iris_compile_options_v1* o, iris_plan** out, iris_diagnostic* d) {
  return compile_v1(s, o, nullptr, out, d);
}
iris_status iris_compile_expr_v1(const char* s, const iris_compile_options_v1* o, const iris_expr_options_v1* expr,
                                 iris_plan** out, iris_diagnostic* d) {
  if (!expr) {
    if (out)
      *out = nullptr;
    return guard(d, [] { iris::fail("null Expr frontend context"); });
  }
  return compile_v1(s, o, expr, out, d);
}
int iris_backend_available(int backend) {
  if (backend == IRIS_BACKEND_SCALAR)
    return 1;
  if (backend == IRIS_BACKEND_LLVM)
    return iris::llvm_available();
  if (backend == IRIS_BACKEND_SLEEF || backend == IRIS_BACKEND_SLEEF_FAST)
    return iris::llvm_available() && iris::math_available(iris::IRIS_MATH_ACCURATE);
  return 0;
}
void iris_plan_destroy(iris_plan* p) {
  delete p;
}
iris_status iris_plan_get_backend(const iris_plan* p, iris_backend* backend, iris_diagnostic* d) {
  return guard(d, [&] {
    if (!p || !backend)
      iris::fail("null backend query");
    *backend = p->program->info.strategy != IRIS_COMPUTE ? IRIS_BACKEND_NONE
               : p->program->jit                         ? p->program->options.backend
                                                         : IRIS_BACKEND_SCALAR;
  });
}
iris_status iris_plan_get_info(const iris_plan* p, iris_plan_info* i, iris_diagnostic* d) {
  return guard(d, [&] {
    if (!p || !i)
      iris::fail("null info argument");
    *i = p->program->info;
  });
}
iris_status iris_plan_get_property(const iris_plan* p, size_t slot, iris_property_dependency* prop,
                                   iris_diagnostic* d) {
  return guard(d, [&] {
    if (!p || !prop || slot >= p->program->ir.properties.size())
      iris::fail("invalid property query");
    const auto& x = p->program->ir.properties[slot];
    prop->input = x.input;
    prop->name = x.name.c_str();
  });
}
iris_status iris_plan_dump(const iris_plan* p, char* buffer, size_t capacity, size_t* required, iris_diagnostic* d) {
  return guard(d, [&] {
    if (!p || !required)
      iris::fail("null dump argument");
    const auto& s = p->program->text;
    *required = s.size() + 1;
    if (!buffer && capacity == 0)
      return;
    if (!buffer || capacity < *required)
      iris::fail("dump buffer too small");
    std::memcpy(buffer, s.c_str(), *required);
  });
}
iris_status iris_context_create(const iris_plan* p, iris_context** out, iris_diagnostic* d) {
  if (out)
    *out = nullptr;
  return guard(d, [&] {
    if (!p || !out)
      iris::fail("null context argument");
    auto c = std::make_unique<iris_context>();
    c->program = p->program;
    c->values.resize(p->program->ir.nodes.size());
    *out = c.release();
  });
}
void iris_context_destroy(iris_context* c) {
  delete c;
}
iris_status iris_execute(const iris_plan* p, iris_context* c, const iris_execute_args* a, iris_diagnostic* d) {
  return guard(d, [&] {
    if (!p || !c || !a)
      iris::fail("null execute argument");
    if (p->program != c->program)
      iris::fail("context belongs to another plan");
    if (p->program->options.input_count > 3)
      iris::fail("plan requires the v1 execute entry point");
    iris::ExecuteArgs args(*a);
    iris::validate_execution(*p->program, args);
    iris::execute_program(*p->program, c->values, args);
  });
}
iris_status iris_execute_v1(const iris_plan* p, iris_context* c, const iris_execute_args_v1* a, iris_diagnostic* d) {
  return guard(d, [&] {
    if (!p || !c || !a || a->struct_size != sizeof(*a))
      iris::fail("invalid v1 execute arguments or structure size");
    if (p->program != c->program)
      iris::fail("context belongs to another plan");
    if (a->input_count != p->program->options.input_count)
      iris::fail("execute input count differs from plan");
    iris::ExecuteArgs args;
    std::copy_n(a->inputs, 26, args.inputs);
    args.output = a->output;
    args.frameno = a->frameno;
    args.properties = a->properties;
    args.property_count = a->property_count;
    iris::validate_execution(*p->program, args);
    iris::execute_program(*p->program, c->values, args);
  });
}
}
