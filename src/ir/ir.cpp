#include "ir.hpp"
#include <cmath>
#include <limits>
#include <sstream>
#include <iomanip>
namespace iris {
unsigned arity(Op op) {
  if (op <= Op::Frame)
    return 0;
  if (op == Op::ToBool || op == Op::ToNumber || op == Op::Abs || op == Op::Sqrt || op == Op::Not ||
      (op >= Op::Neg && op <= Op::Atan))
    return 1;
  return op == Op::Select || op == Op::Clip ? 3 : 2;
}
Type result_type(Op op) {
  return op == Op::ToBool || (op >= Op::Lt && op <= Op::Not) ? Type::Bool : Type::Number;
}
float evaluate(Op op, float a, float b, float c) noexcept {
  switch (op) {
    case Op::ToBool:
      return a > 0;
    case Op::ToNumber:
      return a;
    case Op::Add:
      return a + b;
    case Op::Sub:
      return a - b;
    case Op::Mul:
      return a * b;
    case Op::Div:
      return a / b;
    case Op::Min:
    case Op::Max:
      if (std::isnan(a) || std::isnan(b))
        return std::numeric_limits<float>::quiet_NaN();
      {
        float result = op == Op::Min ? (a < b ? a : b) : (a > b ? a : b);
        return result == 0.0f ? 0.0f : result;
      }
    case Op::Abs:
      return std::fabs(a);
    case Op::Sqrt:
      return std::sqrt(a <= 0.0f ? 0.0f : a);
    case Op::Neg:
      return -a;
    case Op::Sgn:
      return a < 0 ? -1.0f : a > 0 ? 1.0f : 0.0f;
    case Op::Round:
      return std::round(a);
    case Op::Floor:
      return std::floor(a);
    case Op::Ceil:
      return std::ceil(a);
    case Op::Trunc:
      return std::trunc(a);
    case Op::Exp:
      return std::exp(a);
    case Op::Log:
      return std::log(a);
    case Op::Sin:
      return std::sin(a);
    case Op::Cos:
      return std::cos(a);
    case Op::Tan:
      return std::tan(a);
    case Op::Asin:
      return std::asin(a);
    case Op::Acos:
      return std::acos(a);
    case Op::Atan:
      return std::atan(a);
    case Op::Fmod:
      return std::fmod(a, b);
    case Op::Pow:
      return std::pow(a, b);
    case Op::Atan2:
      return std::atan2(a, b);
    case Op::Clip:
      return evaluate(Op::Max, evaluate(Op::Min, a, c), b);
    case Op::Lt:
      return a < b;
    case Op::Le:
      return a <= b;
    case Op::Eq:
      return a == b;
    case Op::Ne:
      return a != b;
    case Op::Ge:
      return a >= b;
    case Op::Gt:
      return a > b;
    case Op::And:
      return a != 0 && b != 0;
    case Op::Or:
      return a != 0 || b != 0;
    case Op::Xor:
      return (a != 0) != (b != 0);
    case Op::Not:
      return a == 0;
    case Op::Select:
      return a != 0 ? b : c;
    default:
      return a;
  }
}
void verify(const IR& ir) {
  auto bad = []() {
    throw Error(IRIS_INTERNAL_ERROR, "invalid typed IR");
  };
  if (ir.nodes.empty() || ir.nodes.size() > max_nodes || ir.result >= ir.nodes.size())
    bad();
  for (size_t i = 0; i < ir.nodes.size(); ++i) {
    const auto& n = ir.nodes[i];
    if (n.op < Op::Constant || n.op > Op::Select)
      bad();
    if (n.type != Type::Number && n.type != Type::Bool)
      bad();
    if (n.op != Op::Constant && n.type != result_type(n.op))
      bad();
    if (n.op == Op::Constant && n.type == Type::Bool && n.value != 0 && n.value != 1)
      bad();
    if (n.op == Op::Input && n.input >= 3)
      bad();
    if (n.op == Op::Property && (n.slot >= ir.properties.size() || ir.properties[n.slot].input >= 3))
      bad();
    for (unsigned j = 0; j < arity(n.op); ++j) {
      if (n.args[j] >= i)
        bad();
      Type wanted = Type::Number;
      if (n.op == Op::ToNumber || (n.op >= Op::And && n.op <= Op::Not) || (n.op == Op::Select && j == 0))
        wanted = Type::Bool;
      if (ir.nodes[n.args[j]].type != wanted)
        bad();
    }
  }
  if (ir.nodes[ir.result].type != Type::Number)
    bad();
}
void optimize(IR& ir) {
  for (auto& n : ir.nodes) {
    const unsigned count = arity(n.op);
    if (!count)
      continue;
    bool constant = true;
    float v[3]{};
    for (unsigned j = 0; j < count; ++j) {
      const auto& a = ir.nodes[n.args[j]];
      constant &= a.op == Op::Constant;
      v[j] = a.value;
    }
    if (constant) {
      n.value = evaluate(n.op, v[0], v[1], v[2]);
      n.op = Op::Constant;
    }
  }
  // Reverse topological liveness: bounded and non-recursive even for deep input.
  std::vector<bool> live(ir.nodes.size());
  live[ir.result] = true;
  for (size_t i = ir.nodes.size(); i-- > 0;)
    if (live[i])
      for (unsigned j = 0; j < arity(ir.nodes[i].op); ++j)
        live[ir.nodes[i].args[j]] = true;
  std::vector<uint32_t> ids(ir.nodes.size());
  std::vector<Node> nodes;
  std::vector<Property> props;
  std::vector<uint32_t> slots(ir.properties.size(), UINT32_MAX);
  for (size_t i = 0; i < ir.nodes.size(); ++i)
    if (live[i]) {
      auto n = ir.nodes[i];
      for (unsigned j = 0; j < arity(n.op); ++j)
        n.args[j] = ids[n.args[j]];
      if (n.op == Op::Property) {
        if (slots[n.slot] == UINT32_MAX) {
          slots[n.slot] = static_cast<uint32_t>(props.size());
          props.push_back(ir.properties[n.slot]);
        }
        n.slot = slots[n.slot];
      }
      ids[i] = static_cast<uint32_t>(nodes.size());
      nodes.push_back(n);
    }
  ir.result = ids[ir.result];
  ir.nodes = std::move(nodes);
  ir.properties = std::move(props);
}
std::string dump(const IR& ir) {
  static const char* names[] = {"const",   "input",     "property", "sx",   "sy",    "width", "height", "frameno",
                                "to_bool", "to_number", "add",      "sub",  "mul",   "div",   "min",    "max",
                                "abs",     "sqrt",      "neg",      "sgn",  "round", "floor", "ceil",   "trunc",
                                "exp",     "log",       "sin",      "cos",  "tan",   "asin",  "acos",   "atan",
                                "fmod",    "pow",       "atan2",    "clip", "lt",    "le",    "eq",     "ne",
                                "ge",      "gt",        "and",      "or",   "xor",   "not",   "select"};
  std::ostringstream s;
  s << std::setprecision(9);
  for (size_t i = 0; i < ir.nodes.size(); ++i) {
    const auto& n = ir.nodes[i];
    s << '%' << i << ":" << (n.type == Type::Bool ? "bool" : "f32") << " = " << names[static_cast<unsigned>(n.op)];
    for (unsigned j = 0; j < arity(n.op); ++j)
      s << " %" << n.args[j];
    if (n.op == Op::Constant)
      s << ' ' << n.value;
    if (n.op == Op::Input)
      s << " input=" << n.input << " offset=[" << n.dx << ',' << n.dy << ']';
    if (n.op == Op::Property)
      s << " slot=" << n.slot << ' ' << ir.properties[n.slot].input << '.' << ir.properties[n.slot].name;
    s << " @" << n.offset << ':' << n.length << '\n';
  }
  s << "return %" << ir.result << '\n';
  return s.str();
}
} // namespace iris
