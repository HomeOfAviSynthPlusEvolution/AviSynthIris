#pragma once
#include "iris/iris.h"
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
namespace iris {
constexpr size_t max_source = 65536, max_nodes = 8192;
enum class Type { Number, Bool };
enum class Op {
  Constant,
  Input,
  Property,
  Sx,
  Sy,
  Width,
  Height,
  Frame,
  ToBool,
  ToNumber,
  Add,
  Sub,
  Mul,
  Div,
  Min,
  Max,
  Abs,
  Sqrt,
  Neg,
  Sgn,
  Round,
  Floor,
  Ceil,
  Trunc,
  Exp,
  Log,
  Sin,
  Cos,
  Tan,
  Asin,
  Acos,
  Atan,
  Fmod,
  Pow,
  Atan2,
  Clip,
  Lt,
  Le,
  Eq,
  Ne,
  Ge,
  Gt,
  And,
  Or,
  Xor,
  Not,
  Select
};
struct Node {
  Op op = Op::Constant;
  Type type = Type::Number;
  std::array<uint32_t, 3> args{};
  float value = 0;
  uint32_t input = 0, slot = 0;
  int32_t dx = 0, dy = 0;
  size_t offset = 0, length = 0;
};
struct Property {
  uint32_t input;
  std::string name;
};
struct IR {
  std::vector<Node> nodes;
  uint32_t result = 0;
  std::vector<Property> properties;
};
struct Error : std::runtime_error {
  iris_status status;
  size_t offset, length;
  Error(iris_status s, std::string msg, size_t o = 0, size_t l = 0)
      : runtime_error(std::move(msg)), status(s), offset(o), length(l) {}
};
[[noreturn]] inline void fail(std::string msg) {
  throw Error(IRIS_INVALID_ARGUMENT, std::move(msg));
}
unsigned arity(Op);
Type result_type(Op);
float evaluate(Op, float, float = 0, float = 0) noexcept;
IR parse(const std::string&, uint32_t input_count, bool extended_inputs = false);
void verify(const IR&);
void optimize(IR&);
std::string dump(const IR&);
} // namespace iris
