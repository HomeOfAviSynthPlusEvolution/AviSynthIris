# AviSynth — Iris

**English** | [简体中文](README.zh-CN.md) | [日本語](README.ja.md)

AviSynth — Iris is an independent pixel-expression engine developed alongside AviSynthMinus. It compiles reverse Polish notation (RPN) expressions into a typed intermediate representation and evaluates them with a scalar interpreter or an optional LLVM JIT backend.

The interface uses C types and functions; the implementation uses C++17. The engine builds without the AviSynth SDK, AvsCore, or AvsSimd. AviSynth integration is handled by a separate host adapter.

## Why separate expression evaluation?

Separating the expression engine from the frameserver allows parsing, optimization, numerical behavior, and execution backends to be maintained and tested independently. The host owns clips, script registration, frame allocation, properties, and scheduling. Iris operates on explicit plane buffers, compiled plans, and execution contexts.

A plan holds reusable expression and backend state. Each concurrent execution uses its own context and output storage, allowing a host to share compiled code without duplicating a filter for each worker.

## Supported operations

| Area | Capabilities |
|---|---|
| Samples | U8, U16 with 9–16 effective bits, and F32; mixed input/output formats and signed byte strides. |
| Expressions | Arithmetic, comparisons, logic, conditional selection, variables, stack operations, rounding, and transcendental functions. |
| Inputs | Up to 26 inputs through the v1 API, named `x`, `y`, `z`, then `a` through `w`. |
| Spatial access | Pixel coordinates, plane dimensions, normalized coordinates, and fixed neighbor offsets with edge clamping. |
| Frame context | Frame number, time, frame properties, bit-depth/range constants, and Expr input-scaling modes. |
| Optimization | Shared IR optimization, fill/copy strategies, optional automatic U8 LUTs, and explicit integer 1D/2D LUTs. |

The expression language follows AviSynth Expr, with deliberate corrections and stricter parsing. It is not a promise of complete script-option compatibility or identical output across every historical Expr implementation. Reserved words cannot be assigned to; malformed stack indices such as `dup0tail` are rejected.

Numerical intermediates use binary32. `sqrt` of a negative value returns positive zero; NaN remains NaN. The `round` operator rounds halfway cases away from zero. Integer output is clamped and rounded half up, with NaN mapped to zero. Iris does not change the host floating-point environment; subnormal and rounding-boundary behavior can differ across backends and optimization settings.

## Backends and CPU selection

| Backend | Execution and math |
|---|---|
| `scalar` | Default scalar reference interpreter using the host math library. |
| `llvm` | LLVM JIT using the host math library. |
| `sleef` | LLVM JIT with SLEEF u10 math routines. |
| `sleef-fast` | LLVM JIT with selected SLEEF u35 routines and a bounded gamma-power fast path. |

Explicit requests for unavailable backends fail. LLVM and SLEEF are optional build dependencies; both SLEEF backends require both dependencies. `iris_backend_available` reports availability.

LLVM uses the local CPU's features and cost model. SLEEF can use AVX2/FMA vector calls when compiled in and supported by the CPU/OS; remaining calls use the corresponding scalar routines. `IRIS_SLEEF_AVX2=OFF` disables this vector mapping. Unlike the conversion modules, Iris does not currently integrate AviSynth's `SetMaxCPU` restrictions into JIT target selection.

The SLEEF backend names select a math policy, not an error bound for an entire expression. The fast path uses `exp2(exponent * log2(base))` for bases in [1/65535, 1] and exponents in [0.25, 4], with an absolute-error acceptance threshold of 1e-6; other powers use u10. `exp` also remains u10. The currently accepted non-FMA scalar u10 `atan2` tolerance is 2 ULP. Different backends need not produce bit-identical results.

## Building and integration

CMake 3.16 or later and a C++17 compiler with floating-point `std::from_chars` support are required. C examples and interface tests use C99. The default build needs neither LLVM nor AviSynth and does not download dependencies.

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/release --config Release --parallel
ctest --test-dir build/release -C Release --output-on-failure
```

Use `-DBUILD_TESTING=OFF` to disable tests. CMake 3.21 or later also supports the supplied Ninja presets: `scalar`, `llvm`, `sanitize`, and `llvm-sanitize`.

The static library target is `iris`, with the alias `Iris::Iris`. Once the source is available under the host's dependency directory:

```cmake
add_subdirectory(third_party/iris)
target_link_libraries(MyHost PRIVATE Iris::Iris)
```

Use matching headers and libraries. The C boundary exposes no C++ or LLVM types, and C++ exceptions do not cross it. Integration uses a joint source build with static linking. Final linking requires the C++ runtime.

The public API is [include/iris/iris.h](include/iris/iris.h). Compile with `iris_compile_v1` or `iris_compile_expr_v1`, query input/property dependencies, create an execution context, and call `iris_execute_v1`. Set each versioned structure's `struct_size` to its exact `sizeof`. A plan can be shared; the same context must not execute concurrently. Callers provide valid buffers, signed byte strides, and nonoverlapping input/output storage. See the [C example](examples/example.c).

### Optional LLVM and SLEEF

Enable LLVM with `-DIRIS_LLVM=ON -DLLVM_DIR=/path/to/lib/cmake/llvm`. The implementation supports LLVM 20–22's C API and requires the exported `LLVM` or `LLVM-C` CMake target. On Ubuntu 24.04, install `llvm-20-dev` from the official repositories and set `LLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm`.

Enable SLEEF with `-DIRIS_SLEEF=ON -DIRIS_FETCH_SLEEF=ON` to download SLEEF 3.9.0, verify its SHA256, and build a static library with Iris. This requires CMake 3.18 or later. Downloading is opt-in and uses the build directory; no separate SLEEF installation is needed.

```sh
cmake -S . -B build/release -DIRIS_LLVM=ON -DLLVM_DIR=/path/to/lib/cmake/llvm -DIRIS_SLEEF=ON -DIRIS_FETCH_SLEEF=ON
cmake --build build/release --config Release --parallel
```

For offline builds, also set `FETCHCONTENT_SOURCE_DIR_SLEEF` to an absolute path containing unpacked SLEEF 3.9.0 sources. Alternatively, set both `IRIS_SLEEF_INCLUDE_DIR` (containing `sleef.h`) and `IRIS_SLEEF_LIBRARY` (a compatible static library); these take priority over downloading. Partial or invalid paths cause an error. Required LLVM runtime libraries must be discoverable when running the host; statically linked SLEEF needs no separate runtime installation.

SLEEF is not supported on Windows ARM64/ARM64EC; keep `IRIS_SLEEF=OFF` on these targets.

## AviSynth integration

The AviSynthMinus native adapter uses the internal static [host bridge](include/iris/host.h) and declares `MT_NICE_FILTER`. It shares plans, JIT code, and built LUTs, while keeping frame references, properties, contexts, and diagnostics local to each request. The bridge inherits the plan's backend when creating a manual LUT.

```avs
IrisExpr(clip, "x 2 *", backend="llvm")
IrisExpr(a, b, "x y + 0.5 *", backend="sleef")
IrisExpr(clip, "x 255 / 0.45 pow 255 *", backend="sleef-fast")
```

Expressions follow Y/U/V/A or R/G/B/A plane order. An empty expression copies the first input plane; changing output bit depth requires an explicit expression, including for alpha. Options include `format`, `backend`, `scale_inputs`, `clamp_float`, `clamp_float_UV`, `optimize`, `lut`, and `lut_max_mb`. Defaults are scalar execution, no input scaling, no float clamping, optimization enabled, and `lut=0`.

Manual `lut=1` and `lut=2` require one and two integer inputs respectively. They snapshot properties from frame 0 and reject coordinates, time, and relative input access. `lut_max_mb=256` limits total table storage across output planes per filter instance. A positive value changes the budget; `-1` removes it. Exceeding the budget produces an error explaining this option rather than switching execution strategies. NICE workers share a table; separately created filter instances own separate tables.

## Testing

Standalone tests cover parsing, numerical rules, mixed formats, properties, signed strides, memory boundaries, plan/context lifetimes, concurrent execution, and scalar/LLVM comparisons. Pure C tests exercise the public API and internal LUT bridge. The native AviSynthMinus host has separate NICE concurrency tests.

Clang/GCC builds can enable `IRIS_SANITIZE=ON` for ASan/UBSan; prebuilt dependencies and JIT-generated machine code are outside that instrumentation.

## Development and contributions

The maintainer directs development, reviews changes, and is responsible for releases. Bug reports, suggestions, and contributions are welcome. Discuss numerical semantics, interface changes, and substantial architectural changes before implementation.

This project uses AI-assisted implementation, tests, and review. Contributions should explain the problem, approach, validation, and how AI was involved. Reports should include the commit, OS, CPU, compiler, build options, backend, expression, input/output formats, dimensions, and a minimal reproducer. Follow the repository's clang-format and clang-tidy configurations.

## Acknowledgments and license

Thanks to AviSynth, AviSynth+, AviSynthMinus, and their contributors for the expression language and host ecosystem, and to LLVM and SLEEF for the optional compilation and math backends.

Thanks to [SB.SB](https://sb.sb) for sponsoring the LLM subscription used in this project's development.

The project uses GPL version 2 or later with the AviSynth linking exception, retaining its original wording and scope. See [LICENSE](LICENSE). Providing a C interface does not expand that exception. Third-party dependencies retain their own license terms. The SLEEF notice is included in [LICENSES/SLEEF.txt](LICENSES/SLEEF.txt).
