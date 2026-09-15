#include <iris/iris.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  const char* expressions[] = {"x 2 * x[-1,0] x[1,0] + 0.5 * -", "x 20 > x 0 ?", "x[1,0] x[-1,0] -",
                               "x x.Gain * frameno +", "x 257 *"};
  const char* labels[] = {"sharpen", "select", "neighbor difference", "dynamic property", "U8 to U16"};
  const float expected[4][4] = {{5, 20, 30, 45}, {0, 0, 30, 40}, {10, 20, 20, 10}, {20, 40, 60, 80}};
  unsigned char input[] = {10, 20, 30, 40};
  size_t k;
  for (k = 0; k < 5; ++k) {
    iris_compile_options o = {0};
    iris_plan* p = NULL;
    iris_context* c = NULL;
    iris_diagnostic d;
    iris_execute_args a = {0};
    float out[4] = {0};
    uint16_t converted[4] = {0};
    float gain = 2;
    o.width = 4;
    o.height = 1;
    o.input_count = 1;
    o.inputs[0].type = IRIS_U8;
    o.inputs[0].bits = 8;
    o.output.type = k == 4 ? IRIS_U16 : IRIS_F32;
    o.output.bits = k == 4 ? 16 : 32;
    o.optimize = 1;
    if (iris_compile(expressions[k], &o, &p, &d) != IRIS_OK) {
      fprintf(stderr, "%s\n", d.message);
      return 1;
    }
    if (iris_context_create(p, &c, &d) != IRIS_OK) {
      iris_plan_destroy(p);
      return 1;
    }
    a.inputs[0].data = input;
    a.inputs[0].stride = 4;
    a.output.data = k == 4 ? (void*)converted : (void*)out;
    a.output.stride = k == 4 ? 8 : 16;
    a.properties = &gain;
    a.property_count = 1;
    for (a.frameno = 0; a.frameno < 2; ++a.frameno) {
      size_t i;
      if (iris_execute(p, c, &a, &d) != IRIS_OK) {
        fprintf(stderr, "%s\n", d.message);
        iris_context_destroy(c);
        iris_plan_destroy(p);
        return 1;
      }
      printf("%s frame %llu:", labels[k], (unsigned long long)a.frameno);
      for (i = 0; i < 4; ++i) {
        float v = k == 4 ? (float)converted[i] : out[i];
        float want = k == 4 ? (float)(input[i] * 257) : expected[k][i] + (k == 3 ? (float)a.frameno : 0);
        if (v != want) {
          iris_context_destroy(c);
          iris_plan_destroy(p);
          return 2;
        }
        printf(" %.2f", v);
      }
      printf("\n");
    }
    iris_context_destroy(c);
    iris_plan_destroy(p);
  }
  return 0;
}
