#include <iris/iris.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #condition);                                                          \
      return 1;                                                                                                        \
    }                                                                                                                  \
  } while (0)

int main(int argc, char** argv) {
  iris_compile_options_v1 o = {0};
  iris_execute_args_v1 a = {0};
  iris_execute_args old_args = {0};
  iris_compile_options old_options = {0};
  iris_plan *p = NULL, *other = NULL;
  iris_context* c = NULL;
  iris_diagnostic diagnostic;
  iris_plan_info info;
  iris_property_dependency property;
  float inputs[26][2][3], output[2][3], gain = 7;
  unsigned i, x, y;
  int optimize;
  CHECK(iris_get_api_version() == IRIS_API_VERSION);
  o.struct_size = sizeof(o);
  o.width = 3;
  o.height = 2;
  o.input_count = 26;
  o.output.type = IRIS_F32;
  o.output.bits = 32;
  o.backend = argc > 1 && strcmp(argv[1], "llvm") == 0 ? IRIS_BACKEND_LLVM : IRIS_BACKEND_SCALAR;
  a.struct_size = sizeof(a);
  a.input_count = 26;
  a.output.data = output;
  a.output.stride = sizeof(output[0]);
  for (i = 0; i < 26; ++i) {
    o.inputs[i] = o.output;
    a.inputs[i].data = inputs[i][1];
    a.inputs[i].stride = -(ptrdiff_t)sizeof(inputs[i][0]);
    for (y = 0; y < 2; ++y)
      for (x = 0; x < 3; ++x)
        inputs[i][1 - y][x] = (float)(i + 1 + x + y * 10);
  }
  for (optimize = 0; optimize <= 1; ++optimize) {
    o.optimize = optimize;
    CHECK(iris_compile_v1(
              "x y + z + a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q + r + s + t + u + v + w +",
              &o, &p, &diagnostic) == IRIS_OK);
    CHECK(iris_plan_get_info(p, &info, NULL) == IRIS_OK);
    CHECK(info.input_mask == ((1u << 26) - 1));
    CHECK(iris_context_create(p, &c, NULL) == IRIS_OK);
    CHECK(iris_execute_v1(p, c, &a, NULL) == IRIS_OK);
    for (y = 0; y < 2; ++y)
      for (x = 0; x < 3; ++x)
        CHECK(output[y][x] == (float)(351 + 26 * (x + 10 * y)));
    CHECK(iris_execute(p, c, &old_args, NULL) == IRIS_INVALID_ARGUMENT);
    a.input_count = 25;
    CHECK(iris_execute_v1(p, c, &a, NULL) == IRIS_INVALID_ARGUMENT);
    a.input_count = 26;
    a.struct_size = sizeof(a) - 1;
    CHECK(iris_execute_v1(p, c, &a, NULL) == IRIS_INVALID_ARGUMENT);
    a.struct_size = sizeof(a);
    CHECK(iris_compile_v1("w", &o, &other, NULL) == IRIS_OK);
    CHECK(iris_execute_v1(other, c, &a, NULL) == IRIS_INVALID_ARGUMENT);
    iris_plan_destroy(other);
    iris_context_destroy(c);
    iris_plan_destroy(p);
    CHECK(iris_compile_v1("w[-1,0] w.Gain + a +", &o, &p, NULL) == IRIS_OK);
    CHECK(iris_plan_get_property(p, 0, &property, NULL) == IRIS_OK);
    CHECK(property.input == 25 && strcmp(property.name, "Gain") == 0);
    CHECK(iris_context_create(p, &c, NULL) == IRIS_OK);
    a.properties = &gain;
    a.property_count = 1;
    CHECK(iris_execute_v1(p, c, &a, NULL) == IRIS_OK);
    for (y = 0; y < 2; ++y)
      for (x = 0; x < 3; ++x)
        CHECK(output[y][x] == (float)(37 + (x ? x - 1 : 0) + x + 20 * y));
    a.property_count = 0;
    CHECK(iris_execute_v1(p, c, &a, NULL) == IRIS_INVALID_ARGUMENT);
    iris_context_destroy(c);
    iris_plan_destroy(p);
  }
  o.struct_size = sizeof(o) - 1;
  CHECK(iris_compile_v1("x", &o, &p, NULL) == IRIS_INVALID_ARGUMENT && p == NULL);
  o.struct_size = sizeof(o) + 1;
  CHECK(iris_compile_v1("x", &o, &p, NULL) == IRIS_INVALID_ARGUMENT && p == NULL);
  o.struct_size = sizeof(o);
  o.input_count = 27;
  CHECK(iris_compile_v1("x", &o, &p, NULL) == IRIS_INVALID_ARGUMENT && p == NULL);
  o.input_count = 25;
  CHECK(iris_compile_v1("w", &o, &p, NULL) == IRIS_PARSE_ERROR && p == NULL);
  o.input_count = 26;
  CHECK(iris_compile_v1("2 a^ a", &o, &p, NULL) == IRIS_PARSE_ERROR && p == NULL);
  CHECK(iris_compile_v1("x", NULL, &p, NULL) == IRIS_INVALID_ARGUMENT && p == NULL);
  o.backend = IRIS_BACKEND_NONE;
  CHECK(iris_compile_v1("x", &o, &p, NULL) == IRIS_INVALID_ARGUMENT && p == NULL);
  old_options.width = 3;
  old_options.height = 2;
  old_options.output = o.output;
  CHECK(iris_compile("2 a^ a", &old_options, &p, NULL) == IRIS_OK);
  CHECK(iris_context_create(p, &c, NULL) == IRIS_OK);
  a.input_count = 0;
  CHECK(iris_execute_v1(p, c, &a, NULL) == IRIS_OK);
  CHECK(output[1][2] == 2);
  iris_context_destroy(c);
  iris_plan_destroy(p);
  {
    iris_expr_options_v1 e = {0};
    e.struct_size = sizeof(e);
    e.frame_count = 3;
    o.backend = IRIS_BACKEND_SCALAR;
    o.input_count = 0;
    CHECK(iris_compile_expr_v1("time", &o, &e, &p, NULL) == IRIS_OK);
    CHECK(iris_context_create(p, &c, NULL) == IRIS_OK);
    a.frameno = 1;
    CHECK(iris_execute_v1(p, c, &a, NULL) == IRIS_OK);
    CHECK(output[0][0] == 0.5f && output[1][2] == 0.5f);
    iris_context_destroy(c);
    iris_plan_destroy(p);
  }
  puts("C99 v1 API and 26-input checks passed");
  return 0;
}
