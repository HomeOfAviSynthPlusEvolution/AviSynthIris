# AviSynth — Iris

[English](README.md) | **简体中文** | [日本語](README.ja.md)

AviSynth — Iris 是与 AviSynthMinus 协同开发的独立像素表达式引擎。它将逆波兰表达式（RPN）编译为带类型的中间表示，通过标量解释器或可选的 LLVM JIT 后端执行。

接口使用 C 类型和函数，内部实现使用 C++17。引擎构建不依赖 AviSynth SDK、AvsCore 或 AvsSimd；AviSynth 集成由独立的宿主适配层负责。

## 为什么分离表达式计算？

将表达式引擎从帧服务器分离，可以独立维护和测试解析、优化、数值行为及执行后端。宿主负责 clip、脚本注册、帧分配、属性和调度；Iris 处理显式传入的平面缓冲区、编译计划和执行上下文。

计划保存可复用的表达式及后端状态。每次并发执行使用独立的 context 和输出存储，使宿主可以共享编译代码，无须为每个工作线程复制滤镜。

## 支持的能力

| 类别 | 能力 |
|---|---|
| 样本 | U8、有效位深 9–16 位的 U16、F32；混合输入输出格式和有符号字节步长。 |
| 表达式 | 算术、比较、逻辑、条件选择、变量、栈操作、舍入及超越函数。 |
| 输入 | v1 API 最多支持 26 路，依次命名为 `x`、`y`、`z`、`a` 至 `w`。 |
| 空间访问 | 像素坐标、平面尺寸、归一化坐标，以及钳位到边缘的固定邻域偏移。 |
| 帧上下文 | 帧号、时间、帧属性、位深与范围常量，以及 Expr 输入缩放模式。 |
| 优化 | 公共 IR 优化、填充与复制策略、可选的自动 U8 LUT，以及显式整数一维/二维 LUT。 |

表达式语言以 AviSynth Expr 为基础，包含明确的修正和更严格的解析规则，不承诺完整兼容所有脚本参数，也不保证与每个历史 Expr 实现输出相同。保留字不能赋值，`dup0tail` 等非法栈索引会被拒绝。

数值中间值使用 binary32。负值的 `sqrt` 返回正零，NaN 仍为 NaN；`round` 的半整数向远离零的方向舍入。整数输出先限制范围，再按半数向上规则舍入，NaN 转为零。Iris 不改变宿主浮点环境；次正规数和临界舍入行为可能随后端及优化设置不同而变化。

## 后端与 CPU 选择

| 后端 | 执行与数学实现 |
|---|---|
| `scalar` | 默认标量参考解释器，使用宿主数学库。 |
| `llvm` | LLVM JIT，使用宿主数学库。 |
| `sleef` | LLVM JIT，使用 SLEEF u10 数学函数。 |
| `sleef-fast` | LLVM JIT，使用部分 SLEEF u35 函数和限定范围的 gamma 幂运算快路径。 |

显式请求不可用后端会报错。LLVM 和 SLEEF 都是可选构建依赖；两个 SLEEF 后端均要求同时具备这两项依赖。`iris_backend_available` 可查询可用性。

LLVM 使用本机 CPU 特征和成本模型。编入向量支持且 CPU/操作系统允许时，SLEEF 可使用 AVX2/FMA 向量调用，其余调用使用对应标量函数。`IRIS_SLEEF_AVX2=OFF` 禁用这类向量映射。与转换模块不同，Iris 当前尚未将 AviSynth 的 `SetMaxCPU` 限制接入 JIT 目标选择。

SLEEF 后端名称选择的是数学策略，并非整个表达式的误差上限。快路径在底数 [1/65535, 1]、指数 [0.25, 4] 内采用 `exp2(exponent * log2(base))`，验收绝对误差阈值为 1e-6；范围外的幂运算使用 u10。`exp` 同样保持 u10。当前非 FMA 标量 u10 `atan2` 的允许误差为 2 ULP。不同后端不要求逐位一致。

## 构建与集成

需要 CMake 3.16 或更新版本，以及标准库支持浮点 `std::from_chars` 的 C++17 编译器。C 示例和接口测试使用 C99。默认构建不需要 LLVM 或 AviSynth，也不会下载依赖。

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/release --config Release --parallel
ctest --test-dir build/release -C Release --output-on-failure
```

`-DBUILD_TESTING=OFF` 可关闭测试。CMake 3.21 或更新版本还可以使用仓库提供的 Ninja presets：`scalar`、`llvm`、`sanitize` 和 `llvm-sanitize`。

静态库目标为 `iris`，别名为 `Iris::Iris`。将源码放入宿主依赖目录后直接链接：

```cmake
add_subdirectory(third_party/iris)
target_link_libraries(MyHost PRIVATE Iris::Iris)
```

升级时应使用配套的头文件与库。C 接口不暴露 C++ 或 LLVM 类型，C++ 异常不会跨越接口边界。当前支持源码联合构建，不承诺稳定的独立 DLL ABI；最终链接需要 C++ 运行库。

公开 API 位于 [include/iris/iris.h](include/iris/iris.h)。通过 `iris_compile_v1` 或 `iris_compile_expr_v1` 编译，查询输入与属性依赖，创建执行 context，再调用 `iris_execute_v1`。版本化结构的 `struct_size` 必须设为该结构的准确 `sizeof`。plan 可以共享，同一个 context 不能并发执行。调用方负责提供有效缓冲区、有符号字节步长，以及互不重叠的输入输出存储。参见 [C 示例](examples/example.c)。

### 可选 LLVM 与 SLEEF

使用 `-DIRIS_LLVM=ON -DLLVM_DIR=/path/to/lib/cmake/llvm` 启用 LLVM。实现支持 LLVM 20–22 C API，要求提供导出的 `LLVM` 或 `LLVM-C` CMake 目标。Ubuntu 24.04 可从官方仓库安装 `llvm-20-dev`，并设置 `LLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm`。

使用 `-DIRIS_SLEEF=ON -DIRIS_FETCH_SLEEF=ON` 下载 SLEEF 3.9.0、校验 SHA256，并与 Iris 一起构建静态库。这需要 CMake 3.18 或更新版本。下载默认关闭，启用后使用构建目录，无需单独安装 SLEEF。

```sh
cmake -S . -B build/release -DIRIS_LLVM=ON -DLLVM_DIR=/path/to/lib/cmake/llvm -DIRIS_SLEEF=ON -DIRIS_FETCH_SLEEF=ON
cmake --build build/release --config Release --parallel
```

离线构建时，另将 `FETCHCONTENT_SOURCE_DIR_SLEEF` 指向已解压 SLEEF 3.9.0 源码的绝对路径。也可以同时设置 `IRIS_SLEEF_INCLUDE_DIR`（包含 `sleef.h`）和 `IRIS_SLEEF_LIBRARY`（兼容的静态库）；手动指定的依赖优先于下载，路径不完整或无效时会报错。运行宿主时必须能找到所需的 LLVM 运行库；静态链接的 SLEEF 无需另行安装运行库。

Windows ARM64/ARM64EC 不支持 SLEEF，这些目标需保持 `IRIS_SLEEF=OFF`。

## AviSynth 集成

AviSynthMinus 原生适配通过内部静态 [宿主桥接](include/iris/host.h) 声明 `MT_NICE_FILTER`。它共享 plan、JIT 代码和已构建的 LUT，每次请求独立持有帧引用、属性、context 和诊断。桥接创建手动 LUT 时继承 plan 的后端选择。

可选 Windows C 插件通过 `IRIS_AVS=ON` 构建，`IRIS_AVS_INCLUDE_DIR` 指向 SDK 头文件目录。启用测试时还需用 `IRIS_AVS_RUNTIME` 指定现有 AviSynth DLL 的绝对路径。插件名为 `IrisExpr.dll`，使用前通过 `LoadPlugin` 加载，再调用 `IrisExpr`。由于公共 AVS C 包装层含有可变状态，这个独立 C 插件仍声明 `MT_SERIALIZED`。

```avs
IrisExpr(clip, "x 2 *", backend="llvm")
IrisExpr(a, b, "x y + 0.5 *", backend="sleef")
IrisExpr(clip, "x 255 / 0.45 pow 255 *", backend="sleef-fast")
```

表达式按 Y/U/V/A 或 R/G/B/A 平面顺序排列。空表达式复制首输入对应平面；改变输出位深时必须填写显式表达式，alpha 也不例外。可选参数包括 `format`、`backend`、`scale_inputs`、`clamp_float`、`clamp_float_UV`、`optimize`、`lut` 和 `lut_max_mb`。默认使用标量、不缩放输入、不钳位浮点输出、开启优化，且 `lut=0`。

手动 `lut=1`、`lut=2` 分别要求一路和两路整数输入。建表固定第 0 帧的属性快照，拒绝坐标、时间和相对输入访问。`lut_max_mb=256` 限制每个滤镜实例所有输出平面的表存储总量；正数可调整预算，`-1` 解除预算。超限时明确报错并说明该选项，不自动改用其他执行策略。NICE 工作线程共享表，显式创建的不同滤镜实例各自持有表。

## 测试

独立测试覆盖解析、数值规则、混合格式、属性、有符号步长、内存边界、plan/context 生命周期、并发执行及标量/LLVM 对照。纯 C 测试验证公开 API 和内部 LUT 桥接。可选 AVS 测试覆盖脚本行为、格式、元数据、LUT 预算和插件加载；原生宿主另有 NICE 并发测试。

Clang/GCC 构建可通过 `IRIS_SANITIZE=ON` 启用 ASan/UBSan；预编译依赖和 JIT 生成的机器码不在该插桩范围内。

## 开发与贡献

维护者负责技术方向、变更审核和发布。欢迎问题报告、建议和代码贡献；数值语义、接口及重大架构改动应先讨论目标与方案。

本项目使用 AI 辅助实现、测试和审查。贡献应说明问题、方案、验证方式及 AI 的参与情况。报告问题时请提供提交号、操作系统、CPU、编译器、构建选项、后端、表达式、输入输出格式、尺寸和最小复现示例。代码应遵循仓库的 clang-format 与 clang-tidy 配置。

## 致谢与许可证

感谢 AviSynth、AviSynth+、AviSynthMinus 及其贡献者提供表达式语言和宿主生态，也感谢 LLVM 与 SLEEF 提供可选的编译和数学后端。

感谢 [烧饼论坛](https://sb.sb) 赞助本项目开发所使用的 LLM 订阅。

项目使用 GPL 第 2 版或更新版本，并保留 AviSynth 链接例外的原文及适用范围，详见 [LICENSE](LICENSE)。提供 C 接口不会扩大该例外。第三方依赖保留各自的许可证条款；SLEEF 声明见 [LICENSES/SLEEF.txt](LICENSES/SLEEF.txt)。
