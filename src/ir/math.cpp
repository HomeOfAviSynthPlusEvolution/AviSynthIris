#include "ir.hpp"
#ifdef IRIS_WITH_SLEEF
#include <sleef.h>
#endif
namespace iris {
#ifdef IRIS_WITH_SLEEF
namespace {
float fast_pow(float base, float exponent) {
  if (fast_pow_domain(base, exponent))
    return Sleef_exp2f1_u35purec(exponent * Sleef_log2f1_u35purec(base));
  return Sleef_powf1_u10purec(base, exponent);
}
} // namespace
#endif
bool math_available(iris_math_mode mode) noexcept {
  if (mode == IRIS_MATH_NATIVE)
    return true;
#ifdef IRIS_WITH_SLEEF
  return mode == IRIS_MATH_ACCURATE || mode == IRIS_MATH_FAST;
#else
  return false;
#endif
}
uintptr_t scalar_math_address(Op op, iris_math_mode mode) noexcept {
  if (mode == IRIS_MATH_NATIVE)
    return 0;
#ifdef IRIS_WITH_SLEEF
  bool fast = mode == IRIS_MATH_FAST;
#define IRIS_ADDRESS(OP, FN)                                                                                           \
  case Op::OP:                                                                                                         \
    return reinterpret_cast<uintptr_t>(fast ? &Sleef_##FN##f1_u35purec : &Sleef_##FN##f1_u10purec)
  switch (op) {
    IRIS_ADDRESS(Sin, sin);
    IRIS_ADDRESS(Cos, cos);
    IRIS_ADDRESS(Tan, tan);
    IRIS_ADDRESS(Asin, asin);
    IRIS_ADDRESS(Acos, acos);
    IRIS_ADDRESS(Atan, atan);
    IRIS_ADDRESS(Log, log);
    IRIS_ADDRESS(Atan2, atan2);
    case Op::Exp:
      return reinterpret_cast<uintptr_t>(&Sleef_expf1_u10purec);
    case Op::Pow:
      return reinterpret_cast<uintptr_t>(fast ? &fast_pow : &Sleef_powf1_u10purec);
    default:
      return 0;
  }
#undef IRIS_ADDRESS
#else
  (void)op;
  (void)mode;
  return 0;
#endif
}
#ifndef IRIS_WITH_SLEEF_AVX2
uintptr_t vector_math_address(Op, iris_math_mode) noexcept {
  return 0;
}
#endif
bool evaluate_math(Op op, float a, float b, iris_math_mode mode, float& result) noexcept {
  if (mode == IRIS_MATH_NATIVE)
    return false;
#ifdef IRIS_WITH_SLEEF
  const bool fast = mode == IRIS_MATH_FAST;
#define IRIS_UNARY(OP, FN)                                                                                             \
  case Op::OP:                                                                                                         \
    result = fast ? Sleef_##FN##f1_u35purec(a) : Sleef_##FN##f1_u10purec(a);                                           \
    return true
  switch (op) {
    IRIS_UNARY(Sin, sin);
    IRIS_UNARY(Cos, cos);
    IRIS_UNARY(Tan, tan);
    IRIS_UNARY(Asin, asin);
    IRIS_UNARY(Acos, acos);
    IRIS_UNARY(Atan, atan);
    IRIS_UNARY(Log, log);
    case Op::Exp:
      result = Sleef_expf1_u10purec(a);
      return true;
    case Op::Pow:
      result = fast ? fast_pow(a, b) : Sleef_powf1_u10purec(a, b);
      return true;
    case Op::Atan2:
      result = fast ? Sleef_atan2f1_u35purec(a, b) : Sleef_atan2f1_u10purec(a, b);
      return true;
    default:
      return false;
  }
#undef IRIS_UNARY
#else
  (void)op;
  (void)a;
  (void)b;
  (void)mode;
  (void)result;
  return false;
#endif
}
} // namespace iris
