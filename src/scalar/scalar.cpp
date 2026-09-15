#include "runtime/runtime.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace iris {
namespace {
float read(const unsigned char* p, iris_format f) noexcept {
  if (f.type == IRIS_U8)
    return *p;
  if (f.type == IRIS_U16) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
  }
  float v;
  std::memcpy(&v, p, 4);
  return v;
}
void write(unsigned char* p, iris_format f, float v) noexcept {
  if (f.type == IRIS_F32) {
    std::memcpy(p, &v, 4);
    return;
  }
  uint32_t max = (1u << f.bits) - 1, result;
  if (!(v > 0))
    result = 0;
  else if (v >= static_cast<float>(max))
    result = max;
  else
    result = static_cast<uint32_t>(std::floor(static_cast<double>(v) + 0.5));
  if (f.type == IRIS_U8)
    *p = static_cast<uint8_t>(result);
  else {
    uint16_t x = static_cast<uint16_t>(result);
    std::memcpy(p, &x, 2);
  }
}
} // namespace
void run(const Program& p, std::vector<float>& v, const ExecuteArgs& a) noexcept {
  const auto& o = p.options;
  const auto& ir = p.ir;
  const size_t out_size = o.output.type == IRIS_U8 ? 1 : o.output.type == IRIS_U16 ? 2 : 4;
  for (uint32_t y = 0; y < o.height; ++y) {
    auto* out = static_cast<unsigned char*>(a.output.data) + static_cast<ptrdiff_t>(y) * a.output.stride;
    if (p.info.strategy == IRIS_COPY) {
      const auto& in = a.inputs[ir.nodes[ir.result].input];
      std::memcpy(out, static_cast<const unsigned char*>(in.data) + static_cast<ptrdiff_t>(y) * in.stride,
                  size_t(o.width) * out_size);
      continue;
    }
    for (uint32_t x = 0; x < o.width; ++x) {
      if (p.info.strategy == IRIS_FILL) {
        write(out + size_t(x) * out_size, o.output, ir.nodes[ir.result].value);
        continue;
      }
      for (size_t i = 0; i < ir.nodes.size(); ++i) {
        const auto& n = ir.nodes[i];
        float value = 0;
        switch (n.op) {
          case Op::Constant:
            value = n.value;
            break;
          case Op::Input: {
            auto xx = std::clamp(int64_t(x) + n.dx, int64_t(0), int64_t(o.width) - 1);
            auto yy = std::clamp(int64_t(y) + n.dy, int64_t(0), int64_t(o.height) - 1);
            const auto& in = a.inputs[n.input];
            auto f = o.inputs[n.input];
            size_t size = f.type == IRIS_U8 ? 1 : f.type == IRIS_U16 ? 2 : 4;
            value = read(static_cast<const unsigned char*>(in.data) + static_cast<ptrdiff_t>(yy) * in.stride +
                             size_t(xx) * size,
                         f);
            break;
          }
          case Op::Property:
            value = a.properties[n.slot];
            break;
          case Op::Sx:
            value = static_cast<float>(x);
            break;
          case Op::Sy:
            value = static_cast<float>(y);
            break;
          case Op::Width:
            value = static_cast<float>(o.width);
            break;
          case Op::Height:
            value = static_cast<float>(o.height);
            break;
          case Op::Frame:
            value = static_cast<float>(a.frameno);
            break;
          default: {
            float args[3]{};
            for (unsigned j = 0; j < arity(n.op); ++j)
              args[j] = v[n.args[j]];
            value = evaluate(n.op, args[0], args[1], args[2]);
          }
        }
        v[i] = value;
      }
      write(out + size_t(x) * out_size, o.output, v[ir.result]);
    }
  }
}
} // namespace iris
