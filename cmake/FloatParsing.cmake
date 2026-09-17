function(iris_configure_float_parsing)
  include(CheckCXXSourceCompiles)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  # Compile and link against the selected SDK/deployment target, not just the compiler version.
  check_cxx_source_compiles("
    #include <charconv>
    int main(int argc, char** argv) {
      float value = 0;
      const char* text = argv[argc - 1];
      auto result = std::from_chars(text, text + 1, value, std::chars_format::general);
      return int(result.ec);
    }
  " IRIS_HAS_FLOAT_FROM_CHARS)
  if(IRIS_HAS_FLOAT_FROM_CHARS AND NOT IRIS_FORCE_FLOAT_PARSE_FALLBACK)
    target_compile_definitions(iris PRIVATE IRIS_HAS_FLOAT_FROM_CHARS=1)
  else()
    # strtof_l uses an explicit C locale without modifying process/thread locale.
    if(WIN32)
      set(iris_locale_probe "
        #include <cstdlib>
        #include <locale.h>
        int main() {
          auto locale = _create_locale(LC_NUMERIC, \"C\");
          char* end;
          float value = _strtof_l(\"1.5\", &end, locale);
          _free_locale(locale);
          return value != 1.5f;
        }")
    else()
      set(iris_locale_probe "
        #include <cstdlib>
        #include <locale.h>
        #ifdef __APPLE__
        #include <xlocale.h>
        #endif
        int main() {
          auto locale = newlocale(LC_NUMERIC_MASK, \"C\", nullptr);
          char* end;
          float value = strtof_l(\"1.5\", &end, locale);
          freelocale(locale);
          return value != 1.5f;
        }")
    endif()
    check_cxx_source_compiles("${iris_locale_probe}" IRIS_HAS_LOCALE_STRTOF)
    if(NOT IRIS_HAS_LOCALE_STRTOF)
      message(FATAL_ERROR "Iris needs floating-point std::from_chars or a locale-specific strtof_l")
    endif()
    message(STATUS "Iris: using the C-locale float parsing fallback")
  endif()
endfunction()
