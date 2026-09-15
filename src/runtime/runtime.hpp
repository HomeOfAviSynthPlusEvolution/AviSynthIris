#pragma once
#include "ir/ir.hpp"
#include <memory>
#include <algorithm>
namespace iris {
struct CompileOptions {
  uint32_t width = 0, height = 0, input_count = 0;
  iris_format inputs[26]{}, output{};
  int optimize = 0;
  bool extended_inputs = false;
  iris_expr_options_v1 expr{};
  CompileOptions() = default;
  CompileOptions(const iris_compile_options& o)
      : width(o.width), height(o.height), input_count(o.input_count), output(o.output), optimize(o.optimize) {
    std::copy_n(o.inputs, 3, inputs);
  }
};
struct ExecuteArgs {
  iris_input_plane inputs[26]{};
  iris_output_plane output{};
  uint64_t frameno = 0;
  const float* properties = nullptr;
  size_t property_count = 0;
  ExecuteArgs() = default;
  ExecuteArgs(const iris_execute_args& a)
      : output(a.output), frameno(a.frameno), properties(a.properties), property_count(a.property_count) {
    std::copy_n(a.inputs, 3, inputs);
  }
};
struct JitCode {
  using Row = void (*)(const ExecuteArgs*, uint32_t);
  Row row = nullptr;
  virtual ~JitCode() = default;
};
struct Program {
  CompileOptions options{};
  IR ir;
  iris_plan_info info{};
  std::string text;
  std::shared_ptr<const JitCode> jit;
  std::vector<unsigned char> lut;
  uint32_t lut_input = 0;
};
size_t sample_bytes(iris_format);
void validate_options(const CompileOptions&);
void describe(Program&);
void validate_execution(const Program&, const ExecuteArgs&);
void run(const Program&, std::vector<float>&, const ExecuteArgs&) noexcept;
void execute_program(const Program&, std::vector<float>&, const ExecuteArgs&) noexcept;
std::shared_ptr<const JitCode> compile_llvm(const Program&);
bool llvm_available() noexcept;
void prepare_lut(Program&);
} // namespace iris
struct iris_plan {
  std::shared_ptr<const iris::Program> program;
};
struct iris_context {
  std::shared_ptr<const iris::Program> program;
  std::vector<float> values;
};
