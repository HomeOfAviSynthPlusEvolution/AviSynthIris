#include "frontend/float_parse.hpp"
#include <atomic>
#include <clocale>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef IRIS_TEST_NATIVE_FLOAT_PARSE
#include <charconv>
#endif

namespace {
uint32_t bits(float value) {
  uint32_t result;
  std::memcpy(&result, &value, sizeof result);
  return result;
}
void require(bool condition) {
  if (!condition)
    throw std::runtime_error("float parsing fallback mismatch");
}
bool parse(const std::string& text, float& value, bool hex = false) {
  return iris::parse_float(text.data(), text.data() + text.size(), value, hex);
}
void check(const std::string& text, bool hex, bool success, float expected) {
  float value = 42;
  if (parse(text, value, hex) != success || bits(value) != bits(expected))
    throw std::runtime_error("boundary: " + text);
}
} // namespace

int main() {
  try {
    // Exercise thread-safe locale initialization and concurrent conversions.
    std::atomic<bool> ok{true};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i)
      workers.emplace_back([&] {
        for (int j = 0; j < 100; ++j) {
          float value = 0;
          if (!parse("1.25", value) || value != 1.25f)
            ok = false;
        }
      });
    for (auto& worker : workers)
      worker.join();
    require(ok);
    check("-0", false, true, -0.0f);
    check("0e-9999", false, true, 0.0f);
    check("8e-46", false, true, 0x1p-149f);
    check("1p-149", true, true, 0x1p-149f);
    check("1.fffffep127", true, true, 0x1.fffffep127f);
    check("1.8", true, true, 1.5f);
    for (const char* text : {"1e9999", "1e-9999", "7e-46", "3.402824e38", "1.2tail"})
      check(text, false, false, 42);
    for (const char* text : {"1p128", "1p-150", "1p+", "1.2tail"})
      check(text, true, false, 42);

    const std::string previous = std::setlocale(LC_NUMERIC, nullptr);
    for (const char* name : {"de_DE.UTF-8", "fr_FR.UTF-8", "French_France.1252"}) {
      if (!std::setlocale(LC_NUMERIC, name))
        continue;
      float value = 0;
      const bool valid = parse("1.5", value) && value == 1.5f && !parse("1,5", value);
      std::setlocale(LC_NUMERIC, previous.c_str());
      require(valid);
    }
#ifdef IRIS_TEST_NATIVE_FLOAT_PARSE
    for (bool hex : {false, true}) {
      for (int exponent = -160; exponent <= 140; exponent += 3) {
        for (int mantissa = 1; mantissa < 100; mantissa += 7) {
          const std::string text = std::to_string(mantissa) + ".125" + (hex ? "p" : "e") + std::to_string(exponent);
          float native = 42, fallback = 42;
          const auto result = std::from_chars(text.data(), text.data() + text.size(), native,
                                              hex ? std::chars_format::hex : std::chars_format::general);
          require(parse(text, fallback, hex) == (result.ec == std::errc{} && result.ptr == text.data() + text.size()));
          if (result.ec == std::errc{} && bits(native) != bits(fallback))
            throw std::runtime_error("differential: " + text + " native=" + std::to_string(native) +
                                     " fallback=" + std::to_string(fallback));
        }
      }
    }
#endif
    std::cout << "Float fallback boundaries, locale and concurrency passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
