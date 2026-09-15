#include "runtime/runtime.hpp"
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Target.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <cstring>
#include <limits>
#include <mutex>

namespace iris {
namespace {
// Private native ABI for libm nodes; no process-global symbol lookup is needed.
extern "C" float iris_host_math(uint32_t op, float a, float c) noexcept {
  return evaluate(static_cast<Op>(op), a, c);
}
void check(LLVMErrorRef error) {
  if (!error)
    return;
  std::unique_ptr<char, decltype(&LLVMDisposeErrorMessage)> message(LLVMGetErrorMessage(error),
                                                                    LLVMDisposeErrorMessage);
  throw Error(IRIS_BACKEND_ERROR, std::string("LLVM: ") + message.get());
}
struct Code final : JitCode {
  LLVMOrcLLJITRef jit = nullptr;
  ~Code() override {
    if (jit)
      LLVMConsumeError(LLVMOrcDisposeLLJIT(jit));
  }
};
struct Module {
  LLVMContextRef context = LLVMContextCreate();
  LLVMOrcThreadSafeContextRef thread_context = LLVMOrcCreateNewThreadSafeContextFromLLVMContext(context);
  LLVMModuleRef module = LLVMModuleCreateWithNameInContext("iris", context);
  LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
  ~Module() {
    LLVMDisposeBuilder(builder);
    if (module)
      LLVMDisposeModule(module);
    LLVMOrcDisposeThreadSafeContext(thread_context);
  }
};
// Byte offsets keep the generated private call ABI identical to the host ABI.
// LLVM types never model public C structure padding by assumption.
struct Lowering {
  Module& m;
  LLVMBuilderRef b;
  LLVMTypeRef f32, i1, i8, i16, i32, i64, iptr, ptr;
  LLVMValueRef args = nullptr, y = nullptr, x = nullptr;
  explicit Lowering(Module& module) : m(module), b(m.builder) {
    f32 = LLVMFloatTypeInContext(m.context);
    i1 = LLVMInt1TypeInContext(m.context);
    i8 = LLVMInt8TypeInContext(m.context);
    i16 = LLVMInt16TypeInContext(m.context);
    i32 = LLVMInt32TypeInContext(m.context);
    i64 = LLVMInt64TypeInContext(m.context);
    iptr = LLVMIntTypeInContext(m.context, unsigned(sizeof(ptrdiff_t) * 8));
    ptr = LLVMPointerTypeInContext(m.context, 0);
  }
  LLVMValueRef integer(int64_t value) { return LLVMConstInt(i64, uint64_t(value), 1); }
  LLVMValueRef number(float value) { return LLVMConstReal(f32, value); }
  LLVMValueRef gep(LLVMValueRef p, LLVMValueRef offset) { return LLVMBuildGEP2(b, i8, p, &offset, 1, ""); }
  LLVMValueRef load(LLVMTypeRef type, LLVMValueRef p) {
    auto v = LLVMBuildLoad2(b, type, p, "");
    LLVMSetAlignment(v, 1);
    return v;
  }
  LLVMValueRef field(LLVMTypeRef type, size_t offset) { return load(type, gep(args, integer(int64_t(offset)))); }
  LLVMValueRef cmp(LLVMRealPredicate op, LLVMValueRef a, LLVMValueRef c) { return LLVMBuildFCmp(b, op, a, c, ""); }
  LLVMValueRef select(LLVMValueRef cond, LLVMValueRef a, LLVMValueRef c) { return LLVMBuildSelect(b, cond, a, c, ""); }
  LLVMValueRef clamp_coord(LLVMValueRef coord, int32_t offset, uint32_t limit) {
    auto v = LLVMBuildAdd(b, coord, integer(offset), "");
    v = select(LLVMBuildICmp(b, LLVMIntSLT, v, integer(0), ""), integer(0), v);
    return select(LLVMBuildICmp(b, LLVMIntSGE, v, integer(limit), ""), integer(limit - 1), v);
  }
  LLVMValueRef input(const Node& n, const Program& p) {
    size_t offset = offsetof(ExecuteArgs, inputs) + n.input * sizeof(iris_input_plane);
    auto base = field(ptr, offset + offsetof(iris_input_plane, data));
    auto stride = field(iptr, offset + offsetof(iris_input_plane, stride));
    if (sizeof(ptrdiff_t) != 8)
      stride = LLVMBuildSExt(b, stride, i64, "");
    auto xx = clamp_coord(x, n.dx, p.options.width), yy = clamp_coord(y, n.dy, p.options.height);
    const auto format = p.options.inputs[n.input];
    auto address = gep(base, LLVMBuildAdd(b, LLVMBuildMul(b, yy, stride, ""),
                                          LLVMBuildMul(b, xx, integer(sample_bytes(format)), ""), ""));
    if (format.type == IRIS_F32)
      return load(f32, address);
    return LLVMBuildUIToFP(b, load(format.type == IRIS_U8 ? i8 : i16, address), f32, "");
  }
  LLVMValueRef intrinsic(const char* name, LLVMValueRef value) {
    auto fn = LLVMGetNamedFunction(m.module, name);
    auto type = LLVMFunctionType(f32, &f32, 1, 0);
    if (!fn)
      fn = LLVMAddFunction(m.module, name, type);
    return LLVMBuildCall2(b, type, fn, &value, 1, "");
  }
  LLVMValueRef minimum_maximum(Op op, LLVMValueRef a, LLVMValueRef c) {
    auto ordinary = select(cmp(op == Op::Min ? LLVMRealOLT : LLVMRealOGT, a, c), a, c);
    auto result = select(cmp(LLVMRealOEQ, ordinary, number(0)), number(0), ordinary);
    return select(cmp(LLVMRealUNO, a, c), number(std::numeric_limits<float>::quiet_NaN()), result);
  }
  LLVMValueRef math_call(Op op, LLVMValueRef a, LLVMValueRef c) {
    LLVMTypeRef types[] = {i32, f32, f32};
    auto type = LLVMFunctionType(f32, types, 3, 0);
    auto address = LLVMConstInt(iptr, reinterpret_cast<uintptr_t>(&iris_host_math), 0);
    auto fn = LLVMConstIntToPtr(address, ptr);
    LLVMValueRef args[] = {LLVMConstInt(i32, static_cast<unsigned>(op), 0), a, c};
    auto call = LLVMBuildCall2(b, type, fn, args, 3, "");
    auto nounwind = LLVMCreateEnumAttribute(m.context, LLVMGetEnumAttributeKindForName("nounwind", 8), 0);
    LLVMAddCallSiteAttribute(call, LLVMAttributeFunctionIndex, nounwind);
    return call;
  }
  void output(LLVMValueRef value, const Program& p) {
    auto base = field(ptr, offsetof(ExecuteArgs, output) + offsetof(iris_output_plane, data));
    auto stride = field(iptr, offsetof(ExecuteArgs, output) + offsetof(iris_output_plane, stride));
    if (sizeof(ptrdiff_t) != 8)
      stride = LLVMBuildSExt(b, stride, i64, "");
    auto offset = LLVMBuildAdd(b, LLVMBuildMul(b, y, stride, ""),
                               LLVMBuildMul(b, x, integer(sample_bytes(p.options.output)), ""), "");
    if (p.options.output.type != IRIS_F32) {
      auto max = number(float((1u << p.options.output.bits) - 1));
      value = select(cmp(LLVMRealOGT, value, number(0)), value, number(0));
      value = select(cmp(LLVMRealOGE, value, max), max, value);
      // Binary64 addition avoids rounding a value just below a half upwards.
      auto f64 = LLVMDoubleTypeInContext(m.context);
      value = LLVMBuildFPExt(b, value, f64, "");
      value = LLVMBuildFAdd(b, value, LLVMConstReal(f64, 0.5), "");
      value = LLVMBuildFPToUI(b, value, p.options.output.type == IRIS_U8 ? i8 : i16, "");
    }
    LLVMSetAlignment(LLVMBuildStore(b, value, gep(base, offset)), 1);
  }
  void lower(const Program& p) {
    LLVMTypeRef params[] = {ptr, i32};
    auto fn = LLVMAddFunction(m.module, "iris_row", LLVMFunctionType(LLVMVoidTypeInContext(m.context), params, 2, 0));
    LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex,
                            LLVMCreateEnumAttribute(m.context, LLVMGetEnumAttributeKindForName("nounwind", 8), 0));
    auto entry = LLVMAppendBasicBlockInContext(m.context, fn, "entry");
    auto loop = LLVMAppendBasicBlockInContext(m.context, fn, "pixels");
    auto done = LLVMAppendBasicBlockInContext(m.context, fn, "done");
    LLVMPositionBuilderAtEnd(b, entry);
    args = LLVMGetParam(fn, 0);
    y = LLVMBuildZExt(b, LLVMGetParam(fn, 1), i64, "");
    LLVMBuildBr(b, loop);
    LLVMPositionBuilderAtEnd(b, loop);
    x = LLVMBuildPhi(b, i64, "x");
    auto zero = integer(0);
    LLVMAddIncoming(x, &zero, &entry, 1);
    std::vector<LLVMValueRef> values;
    for (const auto& n : p.ir.nodes) {
      LLVMValueRef a = nullptr, c = nullptr, d = nullptr, v = nullptr;
      if (arity(n.op) > 0)
        a = values[n.args[0]];
      if (arity(n.op) > 1)
        c = values[n.args[1]];
      if (arity(n.op) > 2)
        d = values[n.args[2]];
      switch (n.op) {
        case Op::Constant:
          v = n.type == Type::Bool ? LLVMConstInt(i1, n.value != 0, 0) : number(n.value);
          break;
        case Op::Input:
          v = input(n, p);
          break;
        case Op::Property:
          v = load(f32, gep(field(ptr, offsetof(ExecuteArgs, properties)), integer(int64_t(n.slot) * sizeof(float))));
          break;
        case Op::Sx:
          v = LLVMBuildUIToFP(b, x, f32, "");
          break;
        case Op::Sy:
          v = LLVMBuildUIToFP(b, y, f32, "");
          break;
        case Op::Width:
          v = number(float(p.options.width));
          break;
        case Op::Height:
          v = number(float(p.options.height));
          break;
        case Op::Frame:
          v = LLVMBuildUIToFP(b, field(i64, offsetof(ExecuteArgs, frameno)), f32, "");
          break;
        case Op::ToBool:
          v = cmp(LLVMRealOGT, a, number(0));
          break;
        case Op::ToNumber:
          v = LLVMBuildUIToFP(b, a, f32, "");
          break;
        case Op::Add:
          v = LLVMBuildFAdd(b, a, c, "");
          break;
        case Op::Sub:
          v = LLVMBuildFSub(b, a, c, "");
          break;
        case Op::Mul:
          v = LLVMBuildFMul(b, a, c, "");
          break;
        case Op::Div:
          v = LLVMBuildFDiv(b, a, c, "");
          break;
        case Op::Min:
        case Op::Max:
          v = minimum_maximum(n.op, a, c);
          break;
        case Op::Abs:
          v = intrinsic("llvm.fabs.f32", a);
          break;
        case Op::Sqrt:
          v = intrinsic("llvm.sqrt.f32", select(cmp(LLVMRealOLE, a, number(0)), number(0), a));
          break;
        case Op::Neg:
          v = LLVMBuildFNeg(b, a, "");
          break;
        case Op::Sgn:
          v = select(cmp(LLVMRealOLT, a, number(0)), number(-1),
                     select(cmp(LLVMRealOGT, a, number(0)), number(1), number(0)));
          break;
        case Op::Round:
          v = intrinsic("llvm.round.f32", a);
          break;
        case Op::Floor:
          v = intrinsic("llvm.floor.f32", a);
          break;
        case Op::Ceil:
          v = intrinsic("llvm.ceil.f32", a);
          break;
        case Op::Trunc:
          v = intrinsic("llvm.trunc.f32", a);
          break;
        case Op::Exp:
        case Op::Log:
        case Op::Sin:
        case Op::Cos:
        case Op::Tan:
        case Op::Asin:
        case Op::Acos:
        case Op::Atan:
          v = math_call(n.op, a, number(0));
          break;
        case Op::Fmod:
        case Op::Pow:
        case Op::Atan2:
          v = math_call(n.op, a, c);
          break;
        case Op::Clip:
          v = minimum_maximum(Op::Max, minimum_maximum(Op::Min, a, d), c);
          break;
        case Op::Lt:
          v = cmp(LLVMRealOLT, a, c);
          break;
        case Op::Le:
          v = cmp(LLVMRealOLE, a, c);
          break;
        case Op::Eq:
          v = cmp(LLVMRealOEQ, a, c);
          break;
        case Op::Ne:
          v = cmp(LLVMRealUNE, a, c);
          break;
        case Op::Ge:
          v = cmp(LLVMRealOGE, a, c);
          break;
        case Op::Gt:
          v = cmp(LLVMRealOGT, a, c);
          break;
        case Op::And:
          v = LLVMBuildAnd(b, a, c, "");
          break;
        case Op::Or:
          v = LLVMBuildOr(b, a, c, "");
          break;
        case Op::Xor:
          v = LLVMBuildXor(b, a, c, "");
          break;
        case Op::Not:
          v = LLVMBuildNot(b, a, "");
          break;
        case Op::Select:
          v = select(a, c, d);
          break;
      }
      values.push_back(v);
    }
    output(values[p.ir.result], p);
    auto next = LLVMBuildAdd(b, x, integer(1), "");
    LLVMAddIncoming(x, &next, &loop, 1);
    LLVMBuildCondBr(b, LLVMBuildICmp(b, LLVMIntULT, next, integer(p.options.width), ""), loop, done);
    LLVMPositionBuilderAtEnd(b, done);
    LLVMBuildRetVoid(b);
  }
};
} // namespace
bool llvm_available() noexcept {
  return true;
}
std::shared_ptr<const JitCode> compile_llvm(const Program& p) {
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter())
      throw Error(IRIS_BACKEND_ERROR, "LLVM native target initialization failed");
  });
  auto code = std::make_shared<Code>();
  check(LLVMOrcCreateLLJIT(&code->jit, nullptr));
  Module m;
  LLVMSetTarget(m.module, LLVMOrcLLJITGetTripleString(code->jit));
  LLVMSetDataLayout(m.module, LLVMOrcLLJITGetDataLayoutStr(code->jit));
  Lowering lowering(m);
  lowering.lower(p);
  char* message = nullptr;
  if (LLVMVerifyModule(m.module, LLVMReturnStatusAction, &message)) {
    std::unique_ptr<char, decltype(&LLVMDisposeMessage)> text(message, LLVMDisposeMessage);
    throw Error(IRIS_BACKEND_ERROR, text.get());
  }
  LLVMDisposeMessage(message);
  auto passes = LLVMCreatePassBuilderOptions();
  auto error = LLVMRunPasses(m.module, "default<O2>", nullptr, passes);
  LLVMDisposePassBuilderOptions(passes);
  check(error);
  auto module = LLVMOrcCreateNewThreadSafeModule(m.module, m.thread_context);
  m.module = nullptr;
  check(LLVMOrcLLJITAddLLVMIRModule(code->jit, LLVMOrcLLJITGetMainJITDylib(code->jit), module));
  LLVMOrcExecutorAddress address = 0;
  check(LLVMOrcLLJITLookup(code->jit, &address, "iris_row")); // materialize before execute
  code->row = reinterpret_cast<JitCode::Row>(uintptr_t(address));
  return code;
}
} // namespace iris
