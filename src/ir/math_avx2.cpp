#include "ir.hpp"
#include <sleef.h>
namespace iris {
// Only call after checking native AVX2/FMA availability. This translation unit
// is compiled for AVX2; no vector types leak through Iris's public or private ABI.
uintptr_t vector_math_address(Op op, iris_math_mode mode) noexcept {
  if (mode == IRIS_MATH_NATIVE)
    return 0;
  bool fast = mode == IRIS_MATH_FAST;
#define IRIS_ADDRESS(OP, FN)                                                                                           \
  case Op::OP:                                                                                                         \
    return reinterpret_cast<uintptr_t>(fast ? &Sleef_##FN##f8_u35avx2 : &Sleef_##FN##f8_u10avx2)
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
      return reinterpret_cast<uintptr_t>(&Sleef_expf8_u10avx2);
    case Op::Pow:
      return reinterpret_cast<uintptr_t>(&Sleef_powf8_u10avx2);
    default:
      return 0;
  }
#undef IRIS_ADDRESS
}
} // namespace iris
