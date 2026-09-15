# Optional developer tools; neither tool is a dependency of the library build.
find_program(IRIS_CLANG_FORMAT NAMES clang-format-22 clang-format)
if(IRIS_CLANG_FORMAT)
  file(GLOB_RECURSE iris_format_sources CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/*.h"
    "${PROJECT_SOURCE_DIR}/src/*.hpp"
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/adapters/*.hpp"
    "${PROJECT_SOURCE_DIR}/adapters/*.cpp"
    "${PROJECT_SOURCE_DIR}/examples/*.c"
    "${PROJECT_SOURCE_DIR}/tests/*.cpp")
  add_custom_target(iris-format-check
    COMMAND "${IRIS_CLANG_FORMAT}" --style=file --dry-run --Werror ${iris_format_sources}
    COMMENT "Checking project source formatting" VERBATIM)
  add_custom_target(iris-format
    COMMAND "${IRIS_CLANG_FORMAT}" --style=file -i ${iris_format_sources}
    COMMENT "Formatting project sources" VERBATIM)
endif()
