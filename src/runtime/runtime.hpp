#pragma once
#include "ir/ir.hpp"
#include <memory>
namespace iris {
struct JitCode {
  using Row = void (*)(const iris_execute_args*, uint32_t);
  Row row = nullptr;
  virtual ~JitCode() = default;
};
struct Program {
  iris_compile_options options{};
  IR ir;
  iris_plan_info info{};
  std::string text;
  std::shared_ptr<const JitCode> jit;
  std::vector<unsigned char> lut;
  uint32_t lut_input = 0;
};
size_t sample_bytes(iris_format);
void validate_options(const iris_compile_options&);
void describe(Program&);
void validate_execution(const Program&, const iris_execute_args&);
void run(const Program&, std::vector<float>&, const iris_execute_args&) noexcept;
void execute_program(const Program&, std::vector<float>&, const iris_execute_args&) noexcept;
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
