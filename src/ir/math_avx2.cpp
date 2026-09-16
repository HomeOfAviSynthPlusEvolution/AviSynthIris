#include "ir.hpp"
#include <sleef.h>
namespace iris {
namespace {
__m256 fast_pow(__m256 base, __m256 exponent) {
  auto bases = _mm256_and_ps(_mm256_cmp_ps(base, _mm256_set1_ps(fast_pow_min_base), _CMP_GE_OQ),
                             _mm256_cmp_ps(base, _mm256_set1_ps(1.0f), _CMP_LE_OQ));
  auto exponents = _mm256_and_ps(_mm256_cmp_ps(exponent, _mm256_set1_ps(0.25f), _CMP_GE_OQ),
                                 _mm256_cmp_ps(exponent, _mm256_set1_ps(4.0f), _CMP_LE_OQ));
  // A mixed vector falls back as a whole: never evaluate log/exp on invalid
  // lanes or pay for both implementations on the common all-gamma path.
  if (_mm256_movemask_ps(_mm256_and_ps(bases, exponents)) == 255)
    return Sleef_exp2f8_u35avx2(_mm256_mul_ps(exponent, Sleef_log2f8_u35avx2(base)));
  return Sleef_powf8_u10avx2(base, exponent);
}
} // namespace
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
      return reinterpret_cast<uintptr_t>(fast ? &fast_pow : &Sleef_powf8_u10avx2);
    default:
      return 0;
  }
#undef IRIS_ADDRESS
}
} // namespace iris
