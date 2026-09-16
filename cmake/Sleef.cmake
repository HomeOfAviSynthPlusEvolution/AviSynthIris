function(iris_check_sleef_platform)
  if(WIN32)
    # Inspect the compiler target, not the host CPU or generator spelling.
    # This also covers ARM64EC and cross-compilation from an x64 host.
    include(CheckCSourceCompiles)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    check_c_source_compiles("
      #if !defined(_M_ARM64) && !defined(_M_ARM64EC) && !defined(__aarch64__) && !defined(__arm64__)
      #error Not an ARM64 target
      #endif
      int main(void) { return 0; }
    " IRIS_SLEEF_WINDOWS_ARM64)
    if(IRIS_SLEEF_WINDOWS_ARM64)
      message(FATAL_ERROR
        "SLEEF is not supported on Windows ARM64/ARM64EC. Set IRIS_SLEEF=OFF; "
        "the scalar and LLVM backends do not require SLEEF")
    endif()
  endif()
endfunction()

# Keep dependency options local so embedding Iris does not change its host's defaults.
function(iris_link_sleef)
  iris_check_sleef_platform()
  if(IRIS_SLEEF_INCLUDE_DIR OR IRIS_SLEEF_LIBRARY)
    if(NOT EXISTS "${IRIS_SLEEF_INCLUDE_DIR}/sleef.h" OR NOT EXISTS "${IRIS_SLEEF_LIBRARY}")
      message(FATAL_ERROR
        "Set both IRIS_SLEEF_INCLUDE_DIR (containing sleef.h) and IRIS_SLEEF_LIBRARY "
        "(a static library), or clear both and enable IRIS_FETCH_SLEEF")
    endif()
    target_include_directories(iris SYSTEM PRIVATE "${IRIS_SLEEF_INCLUDE_DIR}")
    target_link_libraries(iris PRIVATE "${IRIS_SLEEF_LIBRARY}")
    return()
  endif()
  if(NOT IRIS_FETCH_SLEEF)
    message(FATAL_ERROR
      "IRIS_SLEEF requires a static SLEEF library. Set IRIS_SLEEF_INCLUDE_DIR and "
      "IRIS_SLEEF_LIBRARY, or enable IRIS_FETCH_SLEEF to download and build SLEEF 3.9.0")
  endif()
  if(CMAKE_VERSION VERSION_LESS 3.18)
    message(FATAL_ERROR "Building SLEEF with IRIS_FETCH_SLEEF requires CMake 3.18 or newer")
  endif()

  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  set(SLEEF_BUILD_SHARED_LIBS OFF)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  set(SLEEF_BUILD_LIBM ON)
  set(SLEEF_BUILD_TESTS OFF)
  set(SLEEF_BUILD_BENCH OFF)
  set(SLEEF_BUILD_BENCH_REF OFF)
  set(SLEEF_BUILD_DFT OFF)
  set(SLEEF_BUILD_QUAD OFF)
  set(SLEEF_BUILD_GNUABI_LIBS OFF)
  set(SLEEF_BUILD_SCALAR_LIB OFF)
  set(SLEEF_BUILD_INLINE_HEADERS OFF)
  set(SLEEF_ENABLE_TLFLOAT OFF)
  set(SLEEF_DISABLE_SSL ON)
  set(SLEEF_DISABLE_MPFR ON)
  set(SLEEF_DISABLE_FFTW ON)
  FetchContent_Declare(sleef
    URL https://github.com/shibatch/sleef/archive/refs/tags/3.9.0.tar.gz
    URL_HASH SHA256=af60856abac08a3b5e72a8d156dd71fec1f7ac23de8ee67793f45f9edcdf0908)
  FetchContent_MakeAvailable(sleef)
  # SLEEF also defines auxiliary targets outside SLEEF_BUILD_TESTS.
  # Only build the library and its transitive dependencies with Iris.
  set_property(DIRECTORY "${sleef_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
  target_include_directories(iris SYSTEM PRIVATE "${sleef_BINARY_DIR}/include")
  target_link_libraries(iris PRIVATE sleef)
endfunction()
