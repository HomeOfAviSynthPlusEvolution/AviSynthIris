#include <sleef.h>
#include <mpfr.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <cstring>
#include <stdexcept>
using Fn = __m256 (*)(__m256, __m256);
__m256 composed(__m256 x, __m256 y) {
  return Sleef_exp2f8_u10avx2(_mm256_mul_ps(y, Sleef_log2f8_u10avx2(x)));
}
__m256 composed_fast(__m256 x, __m256 y) {
  return Sleef_exp2f8_u35avx2(_mm256_mul_ps(y, Sleef_log2f8_u35avx2(x)));
}
__m256 composed_exp(__m256 x, __m256 y) {
  return Sleef_expf8_u10avx2(_mm256_mul_ps(y, Sleef_logf8_u10avx2(x)));
}
__m256 composed_exp_fast(__m256 x, __m256 y) {
  return Sleef_expf8_u10avx2(_mm256_mul_ps(y, Sleef_logf8_u35avx2(x)));
}
volatile float sink;
int main(int argc, char**) {
  constexpr size_t count = 65536;
  std::vector<float> input(count), output(count);
  for (size_t i = 0; i < count; ++i)
    input[i] = float(i) / 65535.0f;
  mpfr_t a, b, r;
  mpfr_inits2(256, a, b, r, (mpfr_ptr) nullptr);
  struct Candidate {
    const char* name;
    Fn fn;
  };
  const Candidate candidates[] = {{"pow_u10", Sleef_powf8_u10avx2}, {"fastpow_u3500", Sleef_fastpowf8_u3500avx2},
                                  {"exp2_log2_u10", composed},      {"exp2_log2_u35", composed_fast},
                                  {"exp_log_u10", composed_exp},    {"exp_u10_log_u35", composed_exp_fast}};
  std::cout << "candidate,exponent,max_ulp,max_abs_error,max_16bit_code_error,ns_per_sample\n";
  for (float exponent : {.454545f, .5f, 1.8f, 2.2f}) {
    std::vector<double> refs(count), ulps(count);
    mpfr_set_flt(b, exponent, MPFR_RNDN);
    for (size_t i = 0; i < count; ++i) {
      mpfr_set_flt(a, input[i], MPFR_RNDN);
      mpfr_pow(r, a, b, MPFR_RNDN);
      refs[i] = mpfr_get_d(r, MPFR_RNDN);
      float rounded = mpfr_get_flt(r, MPFR_RNDN);
      ulps[i] = std::ldexp(1.0, std::max(-126, std::ilogb(rounded == 0 ? 1 : rounded)) - 23);
    }
    for (auto candidate : candidates) {
      auto execute = [&] {
        for (size_t i = 0; i < count; i += 8)
          _mm256_storeu_ps(output.data() + i,
                           candidate.fn(_mm256_loadu_ps(input.data() + i), _mm256_set1_ps(exponent)));
      };
      execute();
      double maxulp = 0, maxabs = 0;
      int maxcode = 0;
      for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(output[i]))
          return 2;
        double error = std::abs(output[i] - refs[i]);
        maxabs = std::max(maxabs, error);
        maxulp = std::max(maxulp, error / ulps[i]);
        maxcode = std::max(
            maxcode, std::abs(int(std::floor(double(output[i]) * 65535 + .5)) - int(std::floor(refs[i] * 65535 + .5))));
      }
      std::vector<double> times;
      for (int trial = 0; trial < 5; ++trial) {
        size_t reps = 0;
        auto start = std::chrono::steady_clock::now();
        double elapsed;
        do {
          execute();
          sink = output[(reps * 7919) % count];
          ++reps;
          elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        } while (elapsed < .1);
        times.push_back(elapsed * 1e9 / (reps * count));
      }
      std::sort(times.begin(), times.end());
      std::cout << candidate.name << ',' << std::setprecision(9) << exponent << ',' << maxulp << ',' << maxabs << ','
                << maxcode << ',' << times[2] << '\n';
    }
  }
  // Log-distributed binary32 bases cover values between integer code points;
  // independently varying exponents check more than the four fixed examples.
  std::mt19937 random(0x504f57);
  double max_vector = 0, max_scalar = 0;
  constexpr size_t random_count = 262144;
  for (size_t i = 0; i < random_count; i += 8) {
    float bases[8], exponents[8], values[8];
    for (int lane = 0; lane < 8; ++lane) {
      uint32_t raw = 0x37800081u + random() % (0x3f800000u - 0x37800081u + 1);
      std::memcpy(&bases[lane], &raw, sizeof raw);
      exponents[lane] = .25f + float(random() & 0xffffffu) * (3.75f / 16777215.0f);
    }
    _mm256_storeu_ps(values, composed_fast(_mm256_loadu_ps(bases), _mm256_loadu_ps(exponents)));
    for (int lane = 0; lane < 8; ++lane) {
      mpfr_set_flt(a, bases[lane], MPFR_RNDN);
      mpfr_set_flt(b, exponents[lane], MPFR_RNDN);
      mpfr_pow(r, a, b, MPFR_RNDN);
      double reference = mpfr_get_d(r, MPFR_RNDN);
      float scalar = Sleef_exp2f1_u35purec(exponents[lane] * Sleef_log2f1_u35purec(bases[lane]));
      if (!std::isfinite(scalar) || !std::isfinite(values[lane]))
        throw std::runtime_error("nonfinite gamma result");
      max_vector = std::max(max_vector, std::abs(values[lane] - reference));
      max_scalar = std::max(max_scalar, std::abs(scalar - reference));
    }
  }
  std::cerr << "random gamma samples=" << random_count << " vector max abs=" << max_vector
            << " scalar max abs=" << max_scalar << '\n';
  if (argc > 1 && (max_vector > 1e-6 || max_scalar > 1e-6))
    throw std::runtime_error("gamma error budget exceeded");
  mpfr_clears(a, b, r, (mpfr_ptr) nullptr);
}
