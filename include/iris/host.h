#ifndef IRIS_HOST_H
#define IRIS_HOST_H
#include "iris.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Internal static host bridge. Not a separately versioned DLL ABI.
 * Create/build/destroy require exclusive access. After successful build, apply
 * is read-only and may run concurrently. Destroy only after all calls finish.
 * LUT generation inherits the backend selected by the plan.
 * Plan may be destroyed after create. Diagnostics belong to each call.
 */
typedef struct iris_host_lut iris_host_lut;
iris_status iris_host_lut_validate_source(const char* source, iris_diagnostic* diagnostic);
iris_status iris_host_lut_check_budget(uint64_t bytes, int64_t max_mib, iris_diagnostic* diagnostic);
iris_status iris_host_lut_create(const iris_plan* plan, iris_host_lut** out, uint64_t* bytes,
                                 iris_diagnostic* diagnostic);
void iris_host_lut_destroy(iris_host_lut* lut);
iris_status iris_host_lut_build(iris_host_lut* lut, const float* properties, size_t count, iris_diagnostic* diagnostic);
/* Caller supplies valid, nonoverlapping planes with the plan's original geometry
 * and capacity, and at least input_count input slots (unused data may be null).
 */
iris_status iris_host_lut_apply(const iris_host_lut* lut, const iris_input_plane* inputs, iris_output_plane output,
                                iris_diagnostic* diagnostic);
#ifdef __cplusplus
}
#endif
#endif
