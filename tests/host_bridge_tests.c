#include <iris/host.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                                                                       \
  do {                                                                                                                 \
    if (!(x)) {                                                                                                        \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                                                  \
      return 1;                                                                                                        \
    }                                                                                                                  \
  } while (0)
int main(int argc, char** argv) {
  iris_compile_options_v1 options = {0};
  iris_plan* plan = NULL;
  iris_host_lut* lut = NULL;
  iris_diagnostic d;
  uint64_t bytes = 99;
  unsigned char in[] = {0, 1, 17, 250}, out[4] = {0};
  iris_input_plane input = {in, 4};
  iris_output_plane output = {out, 4};
  float gain = 3;
  iris_backend backend = argc > 1 && strcmp(argv[1], "llvm") == 0 ? IRIS_BACKEND_LLVM : IRIS_BACKEND_SCALAR;
  CHECK(iris_host_lut_create(NULL, &lut, &bytes, &d) == IRIS_INVALID_ARGUMENT && !lut && bytes == 0);
  CHECK(iris_host_lut_validate_source(NULL, &d) == IRIS_INVALID_ARGUMENT);
  CHECK(iris_host_lut_validate_source("x[0,0]", &d) == IRIS_INVALID_ARGUMENT);
  CHECK(iris_host_lut_validate_source("frameno 0 *", &d) == IRIS_INVALID_ARGUMENT);
  CHECK(iris_host_lut_check_budget(257ULL * 1024 * 1024, 256, &d) == IRIS_LIMIT_EXCEEDED);
  CHECK(strstr(d.message, "lut_max_mb=-1") != NULL);
  CHECK(iris_host_lut_check_budget(20ULL * 1024 * 1024 * 1024, -1, &d) == IRIS_OK);
  CHECK(iris_host_lut_check_budget(0, 0, &d) == IRIS_INVALID_ARGUMENT);
  options.struct_size = sizeof(options);
  options.width = 4;
  options.height = 1;
  options.input_count = 1;
  options.inputs[0].type = IRIS_U8;
  options.inputs[0].bits = 8;
  options.output = options.inputs[0];
  options.backend = backend;
  options.optimize = 1;
  CHECK(iris_compile_v1("x x.Gain +", &options, &plan, &d) == IRIS_OK);
  CHECK(iris_host_lut_create(plan, &lut, &bytes, &d) == IRIS_OK && bytes == 256);
  iris_plan_destroy(plan); /* The bridge owns all information needed to build. */
  CHECK(iris_host_lut_apply(lut, &input, output, &d) == IRIS_INVALID_ARGUMENT);
  CHECK(iris_host_lut_build(lut, NULL, 1, &d) == IRIS_INVALID_ARGUMENT);
  CHECK(iris_host_lut_build(lut, NULL, 0, &d) == IRIS_INVALID_ARGUMENT);
  CHECK(iris_host_lut_build(lut, &gain, 1, &d) == IRIS_OK);
  gain = 100;
  CHECK(iris_host_lut_build(lut, &gain, 1, &d) == IRIS_INVALID_ARGUMENT);
  CHECK(iris_host_lut_apply(lut, &input, output, &d) == IRIS_OK);
  CHECK(out[0] == 3 && out[1] == 4 && out[2] == 20 && out[3] == 253);
  input.data = NULL;
  CHECK(iris_host_lut_apply(lut, &input, output, &d) == IRIS_INVALID_ARGUMENT);
  iris_host_lut_destroy(lut);
  iris_host_lut_destroy(NULL);
  return 0;
}
