#include "ir/ir.hpp"
#include "expr.hpp"
#include <charconv>
#include <cmath>
#include <string_view>
#include <unordered_map>
namespace iris {
namespace {
bool digit(char c) {
  return c >= '0' && c <= '9';
}
bool letter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool identifier(std::string_view s) {
  if (s.empty() || !letter(s[0]))
    return false;
  for (char c : s)
    if (!letter(c) && !digit(c))
      return false;
  return true;
}
bool space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
bool hexadecimal(std::string_view s) {
  if (!s.empty() && (s.front() == '+' || s.front() == '-'))
    s.remove_prefix(1);
  return s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
}
bool decimal(std::string_view s) {
  size_t i = 0, digits = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-'))
    ++i;
  while (i < s.size() && digit(s[i])) {
    ++i;
    ++digits;
  }
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && digit(s[i])) {
      ++i;
      ++digits;
    }
  }
  if (!digits)
    return false;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-'))
      ++i;
    size_t start = i;
    while (i < s.size() && digit(s[i]))
      ++i;
    if (start == i)
      return false;
  }
  return i == s.size();
}
const std::unordered_map<std::string, Op> ops = {
    {"+", Op::Add},       {"-", Op::Sub},       {"*", Op::Mul},         {"/", Op::Div},         {"min", Op::Min},
    {"max", Op::Max},     {"abs", Op::Abs},     {"sqrt", Op::Sqrt},     {"<", Op::Lt},          {"<=", Op::Le},
    {"=", Op::Eq},        {"!=", Op::Ne},       {">=", Op::Ge},         {">", Op::Gt},          {"and", Op::And},
    {"or", Op::Or},       {"xor", Op::Xor},     {"not", Op::Not},       {"?", Op::Select},      {"sx", Op::Sx},
    {"sy", Op::Sy},       {"width", Op::Width}, {"height", Op::Height}, {"frameno", Op::Frame}, {"==", Op::Eq},
    {"&", Op::And},       {"|", Op::Or},        {"neg", Op::Neg},       {"sgn", Op::Sgn},       {"round", Op::Round},
    {"floor", Op::Floor}, {"ceil", Op::Ceil},   {"trunc", Op::Trunc},   {"exp", Op::Exp},       {"log", Op::Log},
    {"sin", Op::Sin},     {"cos", Op::Cos},     {"tan", Op::Tan},       {"asin", Op::Asin},     {"acos", Op::Acos},
    {"atan", Op::Atan},   {"%", Op::Fmod},      {"pow", Op::Pow},       {"^", Op::Pow},         {"atan2", Op::Atan2},
    {"clip", Op::Clip}};
bool reserved(const std::string& s) {
  return ops.count(s) || s == "pi" || s == "sxr" || s == "syr" || s == "dup" || s == "swap" || s == "x" || s == "y" ||
         s == "z";
}
} // namespace
IR parse(const std::string& source, uint32_t input_count, bool extended_inputs, const iris_format* formats,
         const iris_expr_options_v1* expr) {
  IR ir;
  std::vector<uint32_t> stack;
  std::unordered_map<std::string, uint32_t> vars;
  size_t pos = 0, start = 0, len = 0;
  auto error = [&](std::string s) -> void {
    throw Error(IRIS_PARSE_ERROR, std::move(s), start, len);
  };
  auto emit = [&](Node n) {
    if (ir.nodes.size() >= max_nodes)
      throw Error(IRIS_LIMIT_EXCEEDED, "IR instruction limit exceeded", start, len);
    n.offset = start;
    n.length = len;
    ir.nodes.push_back(n);
    return static_cast<uint32_t>(ir.nodes.size() - 1);
  };
  auto convert = [&](uint32_t id, Type type) {
    if (ir.nodes[id].type == type)
      return id;
    Node n;
    n.op = type == Type::Bool ? Op::ToBool : Op::ToNumber;
    n.type = type;
    n.args[0] = id;
    return emit(n);
  };
  auto need = [&](size_t n) {
    if (stack.size() < n)
      error("stack underflow");
  };
  uint32_t source_bits = 8;
  ExprScaling scaling{expr ? expr->scale_inputs : IRIS_SCALE_NONE};
  auto literal = [&](float value) {
    Node n;
    n.value = value;
    return emit(n);
  };
  auto binary = [&](Op op, uint32_t value, float operand) {
    Node n;
    n.op = op;
    n.args = {convert(value, Type::Number), literal(operand), 0};
    return emit(n);
  };
  auto scale = [&](uint32_t value, uint32_t from, uint32_t to, bool chroma, bool full, bool shift) {
    if (from == to)
      return value;
    auto source_range = expr_range(from, chroma, full);
    auto destination_range = expr_range(to, chroma, full);
    if (shift && chroma && to == 32 && from != 32)
      destination_range.origin = 0.5f;
    if (source_range.origin != 0)
      value = binary(Op::Sub, value, source_range.origin);
    float factor = destination_range.span / source_range.span;
    if (factor != 1)
      value = binary(Op::Mul, value, factor);
    if (destination_range.origin != 0)
      value = binary(Op::Add, value, destination_range.origin);
    return value;
  };
  while (pos < source.size()) {
    if (space(source[pos])) {
      ++pos;
      continue;
    }
    start = pos;
    while (pos < source.size() && !space(source[pos]))
      ++pos;
    len = pos - start;
    std::string t = source.substr(start, len);
    Node n;
    if (expr && (t == "i8" || t == "i10" || t == "i12" || t == "i14" || t == "i16" || t == "f32")) {
      source_bits = t == "i8" ? 8u : t == "i10" ? 10u : t == "i12" ? 12u : t == "i14" ? 14u : t == "i16" ? 16u : 32u;
      continue;
    }
    if (expr && (t == "scaleb" || t == "scalef" || t == "yscaleb" || t == "yscalef")) {
      need(1);
      if (!input_count)
        error("scaling requires a first input format");
      uint32_t target = scaling.converts(formats[0].bits) ? source_bits : formats[0].bits;
      stack.back() = scale(convert(stack.back(), Type::Number), source_bits, target, expr->chroma && t[0] != 'y',
                           t.back() == 'f', scaling.shift());
      continue;
    }
    std::string_view constant_name(t);
    if (t.size() > 2 && t[t.size() - 2] == '_')
      constant_name.remove_suffix(2);
    auto indexed = [&](std::string_view prefix) {
      return t.compare(0, prefix.size(), prefix) == 0 && (t.size() == prefix.size() || digit(t[prefix.size()]) ||
                                                          t[prefix.size()] == '+' || t[prefix.size()] == '-');
    };
    auto depth = [&](size_t prefix, uint32_t default_value, uint32_t minimum) {
      uint32_t value = default_value;
      if (t.size() != prefix) {
        const char* begin = t.data() + prefix;
        const bool negative = *begin == '-';
        if (*begin == '+' || negative)
          ++begin;
        auto result = std::from_chars(begin, t.data() + t.size(), value);
        if (result.ec != std::errc{} || result.ptr != t.data() + t.size() || (negative && value != 0))
          error("invalid stack index");
      }
      if (value < minimum)
        error("stack index below minimum");
      if (value >= stack.size())
        error("stack underflow");
      return value;
    };
    if (indexed("dup")) {
      uint32_t index = depth(3, 0, 0);
      if (stack.size() >= max_nodes)
        throw Error(IRIS_LIMIT_EXCEEDED, "stack limit exceeded", start, len);
      stack.push_back(stack[stack.size() - 1 - index]);
      continue;
    }
    if (indexed("swap")) {
      uint32_t index = depth(4, 1, 1);
      std::swap(stack.back(), stack[stack.size() - 1 - index]);
      continue;
    }
    auto op = ops.find(t);
    if (op != ops.end()) {
      n.op = op->second;
      n.type = result_type(n.op);
      unsigned count = arity(n.op);
      need(count);
      for (unsigned j = 0; j < count; ++j) {
        Type wanted =
            ((n.op >= Op::And && n.op <= Op::Not) || (n.op == Op::Select && j == 0)) ? Type::Bool : Type::Number;
        n.args[j] = convert(stack[stack.size() - count + j], wanted);
      }
      stack.resize(stack.size() - count);
    } else if (expr && t == "time") {
      n.op = Op::Time;
    } else if (expr && t == "sbitdepth") {
      n.value = float(source_bits);
    } else if (expr && expr_constant_name(constant_name)) {
      uint32_t input = 0;
      if (constant_name.size() != t.size()) {
        char suffix = t.back();
        if (suffix < 'a' || suffix > 'z')
          error("invalid range constant input suffix");
        input = uint32_t(suffix >= 'x' ? suffix - 'x' : suffix - 'a' + 3);
      }
      if (input >= input_count)
        error("range constant input outside configured count");
      uint32_t bits = scaling.converts(formats[input].bits) ? source_bits : formats[input].bits;
      n.value = expr_constant(constant_name, bits, expr->chroma != 0, scaling.shift());
    } else if (t == "sxr" || t == "syr") {
      // Expand into ordinary IR so all backends retain the same division and dependencies.
      n.op = t == "sxr" ? Op::Sx : Op::Sy;
      auto coordinate = emit(n);
      n.op = t == "sxr" ? Op::Width : Op::Height;
      auto dimension = emit(n);
      n.op = Op::Constant;
      n.value = 1.0f;
      auto one = emit(n);
      n.op = Op::Sub;
      n.args = {dimension, one, 0};
      auto span = emit(n);
      n.op = Op::Max;
      n.args = {span, one, 0};
      auto denominator = emit(n);
      n.op = Op::Div;
      n.args = {coordinate, denominator, 0};
    } else if (t == "pi") {
      n.value = 3.14159265358979323846f;
    } else if (decimal(t) || hexadecimal(t)) {
      const bool hex = hexadecimal(t);
      const bool negative = t.front() == '-';
      const char* begin = t.data();
      if (*begin == '+' || (hex && negative))
        ++begin;
      if (hex)
        begin += 2; // from_chars hex format does not consume the 0x prefix.
      if (hex && (*begin == '+' || *begin == '-'))
        error("invalid hexadecimal constant");
      auto r = std::from_chars(begin, t.data() + t.size(), n.value,
                               hex ? std::chars_format::hex : std::chars_format::general);
      if (r.ec != std::errc{} || r.ptr != t.data() + t.size() || !std::isfinite(n.value))
        error("constant outside binary32 range");
      if (hex && negative)
        n.value = -n.value;
    } else if (((t[0] >= 'x' && t[0] <= 'z') || (extended_inputs && t[0] >= 'a' && t[0] <= 'w')) &&
               (t.size() == 1 || t[1] == '[' || t[1] == '.')) {
      n.input = static_cast<uint32_t>(t[0] >= 'x' ? t[0] - 'x' : t[0] - 'a' + 3);
      if (n.input >= input_count)
        error("input reference outside configured count");
      n.op = Op::Input;
      if (t.size() > 1 && t[1] == '.') {
        std::string name = t.substr(2);
        if (!identifier(name))
          error("invalid property name");
        n.op = Op::Property;
        n.slot = 0;
        while (n.slot < ir.properties.size() &&
               (ir.properties[n.slot].input != n.input || ir.properties[n.slot].name != name))
          ++n.slot;
        if (n.slot == ir.properties.size())
          ir.properties.push_back({n.input, std::move(name)});
      } else if (t.size() > 1) {
        auto comma = t.find(',');
        if (t.back() != ']' || comma == std::string::npos)
          error("expected input[dx,dy]");
        auto integer = [&](size_t a, size_t b) {
          if (a < b && (t[a] == '+' || t[a] == '-')) {
            if (a + 1 == b || !digit(t[a + 1]))
              error("invalid relative offset");
            if (t[a] == '+')
              ++a;
          }
          int32_t v = 0;
          auto r = std::from_chars(t.data() + a, t.data() + b, v);
          if (r.ec != std::errc{} || r.ptr != t.data() + b || v == INT32_MIN)
            error("invalid relative offset");
          return v;
        };
        n.dx = integer(2, comma);
        n.dy = integer(comma + 1, t.size() - 1);
      }
    } else if (t.back() == '@' || t.back() == '^') {
      const char mode = t.back();
      t.pop_back();
      if (!identifier(t) || reserved(t) || (expr && expr_reserved(t)) ||
          (extended_inputs && t.size() == 1 && t[0] >= 'a' && t[0] <= 'w'))
        error("invalid or reserved variable name");
      if (vars.size() >= max_nodes && vars.find(t) == vars.end())
        throw Error(IRIS_LIMIT_EXCEEDED, "variable limit exceeded", start, len);
      need(1);
      vars[t] = stack.back();
      if (mode == '^')
        stack.pop_back();
      continue;
    } else {
      auto v = vars.find(t);
      if (v == vars.end())
        error(identifier(t) ? "undefined variable or unsupported token" : "unsupported token");
      if (stack.size() >= max_nodes)
        throw Error(IRIS_LIMIT_EXCEEDED, "stack limit exceeded", start, len);
      stack.push_back(v->second);
      continue;
    }
    if (stack.size() >= max_nodes)
      throw Error(IRIS_LIMIT_EXCEEDED, "stack limit exceeded", start, len);
    auto value = emit(n);
    if (expr && n.op == Op::Input) {
      uint32_t bits = formats[n.input].bits;
      if (scaling.converts(bits))
        value = scale(value, bits, source_bits, expr->chroma != 0, scaling.full(), false);
      if (scaling.shift() && expr->chroma && bits == 32)
        value = binary(Op::Add, value, 0.5f);
    }
    stack.push_back(value);
  }
  if (stack.size() != 1) {
    start = source.size();
    len = 0;
    error(stack.empty() ? "expression has no result" : "expression leaves multiple values");
  }
  ir.result = convert(stack.back(), Type::Number);
  if (expr && input_count) {
    uint32_t target = formats[0].bits;
    if (scaling.converts(target))
      ir.result = scale(ir.result, source_bits, target, expr->chroma != 0, scaling.full(), false);
    if (target == 32) {
      if (scaling.shift() && expr->chroma)
        ir.result = binary(Op::Sub, ir.result, 0.5f);
      if (expr->clamp_float) {
        ir.result = binary(Op::Max, ir.result, expr->chroma && !expr->clamp_float_uv ? -0.5f : 0.0f);
        ir.result = binary(Op::Min, ir.result, expr->chroma && !expr->clamp_float_uv ? 0.5f : 1.0f);
      }
    }
  }
  return ir;
}
} // namespace iris
