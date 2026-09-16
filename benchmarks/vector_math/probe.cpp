#include <sleef.h>
#include <mpfr.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Unary = __m256 (*)(__m256);
using Binary = __m256 (*)(__m256, __m256);
using Oracle1 = int (*)(mpfr_ptr, mpfr_srcptr, mpfr_rnd_t);
using Oracle2 = int (*)(mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t);
struct Operation {
  const char* name;
  float (*host)(float, float);
  Unary accurate = nullptr, fast = nullptr;
  Binary accurate2 = nullptr, fast2 = nullptr;
  Oracle1 oracle = nullptr;
  Oracle2 oracle2 = nullptr;
};
const Operation operations[] = {
    {"sin", [](float a, float) { return std::sin(a); }, Sleef_sinf8_u10avx2, Sleef_sinf8_u35avx2, nullptr, nullptr,
     mpfr_sin},
    {"cos", [](float a, float) { return std::cos(a); }, Sleef_cosf8_u10avx2, Sleef_cosf8_u35avx2, nullptr, nullptr,
     mpfr_cos},
    {"tan", [](float a, float) { return std::tan(a); }, Sleef_tanf8_u10avx2, Sleef_tanf8_u35avx2, nullptr, nullptr,
     mpfr_tan},
    {"asin", [](float a, float) { return std::asin(a); }, Sleef_asinf8_u10avx2, Sleef_asinf8_u35avx2, nullptr, nullptr,
     mpfr_asin},
    {"acos", [](float a, float) { return std::acos(a); }, Sleef_acosf8_u10avx2, Sleef_acosf8_u35avx2, nullptr, nullptr,
     mpfr_acos},
    {"atan", [](float a, float) { return std::atan(a); }, Sleef_atanf8_u10avx2, Sleef_atanf8_u35avx2, nullptr, nullptr,
     mpfr_atan},
    {"exp", [](float a, float) { return std::exp(a); }, Sleef_expf8_u10avx2, nullptr, nullptr, nullptr, mpfr_exp},
    {"log", [](float a, float) { return std::log(a); }, Sleef_logf8_u10avx2, Sleef_logf8_u35avx2, nullptr, nullptr,
     mpfr_log},
    {"pow", [](float a, float b) { return std::pow(a, b); }, nullptr, nullptr, Sleef_powf8_u10avx2, nullptr, nullptr,
     mpfr_pow},
    {"atan2", [](float a, float b) { return std::atan2(a, b); }, nullptr, nullptr, Sleef_atan2f8_u10avx2,
     Sleef_atan2f8_u35avx2, nullptr, mpfr_atan2},
    {"fmod", [](float a, float b) { return std::fmod(a, b); }, nullptr, nullptr, Sleef_fmodf8_avx2, nullptr, nullptr,
     mpfr_fmod}};
mpfr_prec_t reference_precision = 256;
struct Big {
  mpfr_t value;
  Big() { mpfr_init2(value, reference_precision); }
  ~Big() { mpfr_clear(value); }
  Big(const Big&) = delete;
};
uint32_t bits(float value) {
  uint32_t result;
  std::memcpy(&result, &value, 4);
  return result;
}
float from_bits(uint32_t value) {
  float result;
  std::memcpy(&result, &value, 4);
  return result;
}
bool available(const Operation& op, int mode) {
  if (mode >= 3)
    return std::string(op.name) != "fmod";
  return mode != 2 || op.fast || op.fast2;
}
float scalar(const Operation& op, bool fast, float x, float y) {
  std::string name = op.name;
#define UNARY(NAME)                                                                                                    \
  if (name == #NAME)                                                                                                   \
  return fast ? Sleef_##NAME##f1_u35purec(x) : Sleef_##NAME##f1_u10purec(x)
  UNARY(sin);
  UNARY(cos);
  UNARY(tan);
  UNARY(asin);
  UNARY(acos);
  UNARY(atan);
  UNARY(log);
#undef UNARY
  if (name == "exp")
    return Sleef_expf1_u10purec(x);
  if (name == "pow")
    return Sleef_powf1_u10purec(x, y);
  if (name == "atan2")
    return fast ? Sleef_atan2f1_u35purec(x, y) : Sleef_atan2f1_u10purec(x, y);
  throw std::runtime_error("unknown scalar operation");
}
const char* label(const Operation& op, int mode) {
  if (mode >= 3)
    return mode == 3 ? "scalar_accurate" : "scalar_fast";
  return mode == 0 ? "host" : mode == 2 ? "sleef_u35" : std::string(op.name) == "fmod" ? "sleef" : "sleef_u10";
}
void run(const Operation& op, int mode, const std::vector<float>& x, const std::vector<float>& y,
         std::vector<float>& out) {
  if (mode >= 3) {
    for (size_t i = 0; i < x.size(); ++i)
      out[i] = scalar(op, mode == 4, x[i], y[i]);
  } else if (!mode) {
    for (size_t i = 0; i < x.size(); ++i)
      out[i] = op.host(x[i], y[i]);
  } else {
    auto unary = mode == 1 ? op.accurate : op.fast;
    auto binary = mode == 1 ? op.accurate2 : op.fast2;
    for (size_t i = 0; i < x.size(); i += 8) {
      auto a = _mm256_loadu_ps(x.data() + i);
      auto value = unary ? unary(a) : binary(a, _mm256_loadu_ps(y.data() + i));
      _mm256_storeu_ps(out.data() + i, value);
    }
  }
}
void fill(const Operation& op, std::vector<float>& x, std::vector<float>& y, bool broad) {
  std::mt19937 rng(0x1a15);
  std::string name = op.name;
  for (size_t i = 0; i < x.size(); ++i) {
    float a = float(rng() & 0xffffu) / 65535.0f, b = float(rng() & 0xffffu) / 65535.0f;
    if (broad) {
      x[i] = from_bits(rng());
      y[i] = from_bits(rng());
      // Keep pow's exponent moderate for meaningful finite results as well.
      if (name == "pow" && i % 2 == 0)
        y[i] = b * 20 - 10;
    } else {
      x[i] = a * 8 - 4;
      y[i] = b * 8 - 4;
      if (name == "asin" || name == "acos")
        x[i] = a * 2 - 1;
      if (name == "log" || name == "pow")
        x[i] = a * 4 + 0.001f;
      if (name == "exp")
        x[i] = a * 200 - 110;
    }
  }
}
struct Stats {
  double max_ulp = 0;
  uint64_t rounded_different = 0, category_errors = 0, zero_sign_errors = 0;
  uint32_t worst_x = 0, worst_y = 0, worst_result = 0;
};
void accuracy(bool enforce) {
  uint64_t failures = 0;
  constexpr size_t count = 65536;
  std::vector<float> x(count), y(count);
  std::array<std::vector<float>, 5> outputs;
  for (auto& output : outputs)
    output.resize(count);
  Big a, b, reference, difference;
  std::cout << "function,distribution,implementation,samples,max_ulp,rounded_different,category_errors,zero_sign_"
               "errors,worst_x_bits,worst_y_bits,worst_result_bits\n";
  for (const auto& op : operations)
    for (bool broad : {false, true}) {
      fill(op, x, y, broad);
      const float special[] = {0,
                               -0.0f,
                               1,
                               -1,
                               2,
                               -2,
                               std::numeric_limits<float>::denorm_min(),
                               -std::numeric_limits<float>::denorm_min(),
                               std::numeric_limits<float>::min(),
                               std::numeric_limits<float>::max(),
                               -std::numeric_limits<float>::max(),
                               INFINITY,
                               -INFINITY,
                               NAN,
                               0.5f,
                               -0.5f};
      size_t cursor = 0;
      for (float first : special)
        for (float second : special) {
          x[cursor] = first;
          y[cursor++] = second;
        }
      // Adjacent binary32 values around domain endpoints, range-reduction
      // boundaries, and exponential overflow/underflow. Mix lanes deliberately.
      const float centers[] = {
          0,          1,           -1,          0.5f, -0.5f,  1.5707963267948966f, 3.141592653589793f,
          88.722839f, -87.336545f, -103.97208f, 8192, 1048576};
      for (float center : centers)
        for (float direction : {-INFINITY, INFINITY}) {
          float value = center;
          for (int step = 0; step < 64; ++step) {
            x[cursor] = value;
            y[cursor++] = special[size_t(step) % std::size(special)];
            value = std::nextafter(value, direction);
          }
        }
      // Persist the discovered non-FMA atan2 u10 counterexample independently
      // of the random generator and future sample-count changes.
      if (std::string(op.name) == "atan2") {
        x[cursor] = from_bits(0x06130708);
        y[cursor] = from_bits(0x44be6425);
      }
      for (int mode = 0; mode < 5; ++mode)
        if (available(op, mode))
          run(op, mode, x, y, outputs[size_t(mode)]);
      std::array<Stats, 5> stats{};
      for (size_t i = 0; i < count; ++i) {
        mpfr_set_flt(a.value, x[i], MPFR_RNDN);
        mpfr_set_flt(b.value, y[i], MPFR_RNDN);
        if (op.oracle)
          op.oracle(reference.value, a.value, MPFR_RNDN);
        else
          op.oracle2(reference.value, a.value, b.value, MPFR_RNDN);
        float rounded = mpfr_get_flt(reference.value, MPFR_RNDN);
        for (int mode = 0; mode < 5; ++mode) {
          if (!available(op, mode))
            continue;
          float actual = outputs[size_t(mode)][i];
          auto& s = stats[size_t(mode)];
          if (std::isnan(rounded)   ? !std::isnan(actual)
              : std::isinf(rounded) ? actual != rounded
                                    : !std::isfinite(actual)) {
            if (s.category_errors < 3)
              std::cerr << op.name << ',' << (broad ? "broad" : "ordinary") << ',' << label(op, mode)
                        << ",category: x=" << x[i] << " y=" << y[i] << " actual=" << actual << " expected=" << rounded
                        << '\n';
            ++s.category_errors;
            continue;
          }
          if (!std::isfinite(rounded))
            continue;
          if (bits(actual) != bits(rounded))
            ++s.rounded_different;
          if (actual == 0 && rounded == 0 && std::signbit(actual) != std::signbit(rounded))
            ++s.zero_sign_errors;
          mpfr_set_flt(difference.value, actual, MPFR_RNDN);
          mpfr_sub(difference.value, difference.value, reference.value, MPFR_RNDN);
          // One binary32 ULP at the exact result's binade, floored at 2^-149.
          long exponent =
              mpfr_zero_p(reference.value) ? -149 : std::max(-149L, long(mpfr_get_exp(reference.value)) - 24);
          mpfr_mul_2si(difference.value, difference.value, -exponent, MPFR_RNDN);
          double error = std::abs(mpfr_get_d(difference.value, MPFR_RNDN));
          if (error > s.max_ulp) {
            s.max_ulp = error;
            s.worst_x = bits(x[i]);
            s.worst_y = bits(y[i]);
            s.worst_result = bits(actual);
          }
        }
      }
      for (int mode = 0; mode < 5; ++mode)
        if (available(op, mode)) {
          const auto& s = stats[size_t(mode)];
          if (mode && std::string(op.name) != "fmod") {
            double limit = (mode == 2 || (mode == 4 && (op.fast || op.fast2))) ? 3.5 : 1.0;
            if (s.max_ulp > limit || s.category_errors || s.zero_sign_errors) {
              std::cerr << "REJECT " << op.name << ',' << (broad ? "broad" : "ordinary") << ',' << label(op, mode)
                        << ": max_ulp=" << s.max_ulp << " limit=" << limit << " category_errors=" << s.category_errors
                        << " zero_sign_errors=" << s.zero_sign_errors << '\n';
              ++failures;
            }
          }
          std::cout << op.name << ',' << (broad ? "broad" : "ordinary") << ',' << label(op, mode) << ',' << count << ','
                    << s.max_ulp << ',' << s.rounded_different << ',' << s.category_errors << ',' << s.zero_sign_errors
                    << ',' << s.worst_x << ',' << s.worst_y << ',' << s.worst_result << '\n'
                    << std::flush;
        }
    }
  if (enforce && failures)
    throw std::runtime_error("math precision acceptance failed: " + std::to_string(failures) + " rows");
}
volatile uint64_t sink = 0;
void speed() {
  using Clock = std::chrono::steady_clock;
  constexpr size_t count = 1280 * 720;
  std::vector<float> x(count), y(count), out(count);
  std::cout << "function,distribution,implementation,pixels,min_ms,median_ms,max_ms,repetitions\n";
  for (const auto& op : operations)
    for (bool broad : {false, true}) {
      fill(op, x, y, broad);
      for (int mode = 0; mode < 3; ++mode) {
        if (!available(op, mode))
          continue;
        run(op, mode, x, y, out);
        std::array<double, 5> timings{};
        uint64_t total = 0;
        for (auto& time : timings) {
          auto start = Clock::now();
          uint64_t iterations = 0;
          double duration = 0;
          do {
            run(op, mode, x, y, out);
            sink = sink + bits(out[size_t(iterations) % count]);
            ++iterations;
            duration = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
          } while (duration < 100);
          time = duration / double(iterations);
          total += iterations;
        }
        std::sort(timings.begin(), timings.end());
        std::cout << op.name << ',' << (broad ? "broad" : "ordinary") << ',' << label(op, mode) << ',' << count << ','
                  << timings.front() << ',' << timings[2] << ',' << timings.back() << ',' << total << '\n'
                  << std::flush;
      }
    }
}
} // namespace
int main(int argc, char** argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("AVX2+FMA research probe: use accuracy, accuracy512, check, check512 or speed");
    std::cout << std::setprecision(10);
    std::cerr << "SLEEF " << SLEEF_VERSION_MAJOR << '.' << SLEEF_VERSION_MINOR << '.' << SLEEF_VERSION_PATCHLEVEL
              << "; MPFR " << mpfr_get_version() << "; MXCSR=" << _mm_getcsr() << '\n';
    std::string mode = argv[1];
    if (mode == "accuracy512" || mode == "check512")
      reference_precision = 512;
    if (mode == "accuracy" || mode == "accuracy512" || mode == "check" || mode == "check512")
      accuracy(mode == "check" || mode == "check512");
    else if (mode == "speed")
      speed();
    else
      throw std::runtime_error("expected accuracy, accuracy512, check, check512 or speed");
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
