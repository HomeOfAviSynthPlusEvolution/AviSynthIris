#ifndef IRIS_IRIS_H
#define IRIS_IRIS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct iris_plan iris_plan;
typedef struct iris_context iris_context;
typedef enum iris_status {
  IRIS_OK,
  IRIS_INVALID_ARGUMENT,
  IRIS_PARSE_ERROR,
  IRIS_LIMIT_EXCEEDED,
  IRIS_OUT_OF_MEMORY,
  IRIS_INTERNAL_ERROR,
  IRIS_BACKEND_UNAVAILABLE,
  IRIS_BACKEND_ERROR
} iris_status;
typedef struct iris_diagnostic {
  size_t offset, length;
  char message[256];
} iris_diagnostic;
typedef enum iris_sample_type { IRIS_U8, IRIS_U16, IRIS_F32 } iris_sample_type;
typedef struct iris_format {
  iris_sample_type type;
  uint32_t bits;
} iris_format;
typedef struct iris_compile_options {
  uint32_t width, height, input_count;
  iris_format inputs[3], output;
  int optimize; /* exactly 0 or 1 */
} iris_compile_options;
/* COMPUTE is backend independent. NONE means a fill/copy/LUT strategy. */
typedef enum iris_strategy { IRIS_COMPUTE, IRIS_FILL, IRIS_COPY, IRIS_LUT_U8 } iris_strategy;
typedef enum iris_backend { IRIS_BACKEND_NONE, IRIS_BACKEND_SCALAR, IRIS_BACKEND_LLVM } iris_backend;
enum { IRIS_DEP_SX = 1, IRIS_DEP_SY = 2, IRIS_DEP_WIDTH = 4, IRIS_DEP_HEIGHT = 8, IRIS_DEP_FRAMENO = 16 };
typedef struct iris_plan_info {
  uint32_t width, height, input_mask, metadata_mask;
  size_t instruction_count, property_count;
  int has_relative_access;
  iris_strategy strategy;
} iris_plan_info;
typedef struct iris_property_dependency {
  uint32_t input;
  const char* name;
} iris_property_dependency;
typedef struct iris_input_plane {
  const void* data;
  ptrdiff_t stride;
} iris_input_plane;
typedef struct iris_output_plane {
  void* data;
  ptrdiff_t stride;
} iris_output_plane;
typedef struct iris_execute_args {
  iris_input_plane inputs[3];
  iris_output_plane output;
  uint64_t frameno;
  const float* properties;
  size_t property_count;
} iris_execute_args;
/* Expression is NUL terminated, <=65536 bytes. Diagnostics are optional.
   All failures clear output handles. Other pointers are required unless documented. */
iris_status iris_compile(const char*, const iris_compile_options*, iris_plan**, iris_diagnostic*);
/* Additive experimental API. Legacy compile selects scalar with LUT disabled.
   enable_lut=1 permits a bounded U8 table when eligible; otherwise compute.
   Explicit LLVM requests fail when LLVM is unavailable; never silently fall back. */
iris_status iris_compile_ex(const char*, const iris_compile_options*, iris_backend, int enable_lut, iris_plan**,
                            iris_diagnostic*);
iris_status iris_plan_get_backend(const iris_plan*, iris_backend*, iris_diagnostic*);
void iris_plan_destroy(iris_plan*);
iris_status iris_plan_get_info(const iris_plan*, iris_plan_info*, iris_diagnostic*);
iris_status iris_plan_get_property(const iris_plan*, size_t, iris_property_dependency*, iris_diagnostic*);
/* required includes NUL. NULL buffer with capacity=0 queries size. */
iris_status iris_plan_dump(const iris_plan*, char* buffer, size_t capacity, size_t* required, iris_diagnostic*);
iris_status iris_context_create(const iris_plan*, iris_context**, iris_diagnostic*);
void iris_context_destroy(iris_context*);
iris_status iris_execute(const iris_plan*, iris_context*, const iris_execute_args*, iris_diagnostic*);
#ifdef __cplusplus
}
#endif
#endif
