#include "float_parse.hpp"
#ifdef IRIS_HAS_FLOAT_FROM_CHARS
#include <charconv>
#else
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <locale.h>
#include <new>
#include <string>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#endif

namespace iris {
#ifndef IRIS_HAS_FLOAT_FROM_CHARS
namespace {
struct NumericLocale {
#ifdef _WIN32
  _locale_t handle = _create_locale(LC_NUMERIC, "C");
  ~NumericLocale() { _free_locale(handle); }
#else
  locale_t handle = newlocale(LC_NUMERIC_MASK, "C", nullptr);
  ~NumericLocale() { freelocale(handle); }
#endif
  NumericLocale() {
    if (!handle)
      throw std::bad_alloc();
  }
};
} // namespace
#endif

bool parse_float(const char* first, const char* last, float& value, bool hex) {
#ifdef IRIS_HAS_FLOAT_FROM_CHARS
  auto result = std::from_chars(first, last, value, hex ? std::chars_format::hex : std::chars_format::general);
  return result.ec == std::errc{} && result.ptr == last;
#else
  static const NumericLocale locale;
  std::string text = hex ? "0x" : "";
  text.append(first, last);
  char* end = nullptr;
  const int previous_errno = errno;
  errno = 0;
#ifdef _WIN32
  const float parsed = _strtof_l(text.c_str(), &end, locale.handle);
#else
  const float parsed = strtof_l(text.c_str(), &end, locale.handle);
#endif
  const int conversion_errno = errno;
  errno = previous_errno;
  // C libraries can report ERANGE for representable subnormals. Accept those,
  // but reject nonzero literals rounded to zero, matching from_chars.
  if (end == text.c_str() || end != text.c_str() + text.size() || !std::isfinite(parsed) ||
      (conversion_errno == ERANGE && parsed == 0))
    return false;
  value = parsed;
  return true;
#endif
}
} // namespace iris
