#include "runtime/runtime.hpp"
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm/Config/llvm-config.h>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

namespace iris {
namespace {
// Private native ABI for libm nodes; no process-global symbol lookup is needed.
extern "C" float iris_host_math(uint32_t op, float a, float c, uint32_t math) noexcept {
  return evaluate(static_cast<Op>(op), a, c, 0, static_cast<iris_math_mode>(math));
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
  // LLVM 21 replaced direct context access with wrapping an owned LLVMContext.
#if LLVM_VERSION_MAJOR == 20
  LLVMOrcThreadSafeContextRef thread_context = LLVMOrcCreateNewThreadSafeContext();
  LLVMContextRef context = LLVMOrcThreadSafeContextGetContext(thread_context);
#else
  LLVMContextRef context = LLVMContextCreate();
  LLVMOrcThreadSafeContextRef thread_context = LLVMOrcCreateNewThreadSafeContextFromLLVMContext(context);
#endif
  LLVMModuleRef module = LLVMModuleCreateWithNameInContext("iris", context);
  LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
  LLVMBuilderRef parameter_builder = LLVMCreateBuilderInContext(context);
  ~Module() {
    LLVMDisposeBuilder(parameter_builder);
    LLVMDisposeBuilder(builder);
    if (module)
      LLVMDisposeModule(module);
    LLVMOrcDisposeThreadSafeContext(thread_context);
  }
};
struct NativeTarget {
  std::unique_ptr<char, decltype(&LLVMDisposeMessage)> cpu{LLVMGetHostCPUName(), LLVMDisposeMessage};
  std::unique_ptr<char, decltype(&LLVMDisposeMessage)> features{LLVMGetHostCPUFeatures(), LLVMDisposeMessage};
  LLVMTargetMachineRef machine = nullptr;
  explicit NativeTarget(const char* triple) {
    LLVMTargetRef target = nullptr;
    char* error = nullptr;
    if (LLVMGetTargetFromTriple(triple, &target, &error)) {
      std::unique_ptr<char, decltype(&LLVMDisposeMessage)> message(error, LLVMDisposeMessage);
      throw Error(IRIS_BACKEND_ERROR, message ? message.get() : "LLVM target lookup failed");
    }
    LLVMDisposeMessage(error);
    machine = LLVMCreateTargetMachine(target, triple, cpu.get(), features.get(), LLVMCodeGenLevelDefault,
                                      LLVMRelocDefault, LLVMCodeModelJITDefault);
    if (!machine)
      throw Error(IRIS_BACKEND_ERROR, "LLVM native target machine creation failed");
  }
  ~NativeTarget() { LLVMDisposeTargetMachine(machine); }
  NativeTarget(const NativeTarget&) = delete;
};
// Byte offsets keep the generated private call ABI identical to the host ABI.
// LLVM types never model public C structure padding by assumption.
struct Lowering {
  Module& m;
  LLVMBuilderRef b;
  LLVMTypeRef f32, i1, i8, i16, i32, i64, iptr, ptr;
  LLVMValueRef args = nullptr, y = nullptr, x = nullptr;
  LLVMBasicBlockRef entry = nullptr;
  std::map<size_t, LLVMValueRef> fields;
  LLVMOrcLLJITRef jit;
  bool vector_math;
  bool interior = false;
  std::vector<LLVMValueRef> vector_functions;
  void symbol(const std::string& name, uintptr_t address) {
    LLVMOrcCSymbolMapPair pair{};
    pair.Name = LLVMOrcLLJITMangleAndIntern(jit, name.c_str());
    pair.Sym.Address = address;
    pair.Sym.Flags.GenericFlags = LLVMJITSymbolGenericFlagsExported | LLVMJITSymbolGenericFlagsCallable;
    auto unit = LLVMOrcAbsoluteSymbols(&pair, 1);
    auto error = LLVMOrcJITDylibDefine(LLVMOrcLLJITGetMainJITDylib(jit), unit);
    if (error)
      LLVMOrcDisposeMaterializationUnit(unit);
    check(error);
  }
  void pure(LLVMValueRef fn) {
    for (const char* name : {"nounwind", "willreturn", "memory"})
      LLVMAddAttributeAtIndex(
          fn, LLVMAttributeFunctionIndex,
          LLVMCreateEnumAttribute(m.context, LLVMGetEnumAttributeKindForName(name, std::strlen(name)), 0));
  }
  explicit Lowering(Module& module, LLVMOrcLLJITRef j, bool vectors)
      : m(module), b(m.builder), jit(j), vector_math(vectors) {
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
  LLVMValueRef field(LLVMTypeRef type, size_t offset) {
    auto found = fields.find(offset);
    if (found != fields.end())
      return found->second;
    // ExecuteArgs is a private immutable snapshot, independent of pixel buffers.
    // Load its fields once per row, before stores can inhibit LLVM's alias analysis.
    auto builder = m.parameter_builder;
    LLVMPositionBuilderBefore(builder, LLVMGetBasicBlockTerminator(entry));
    auto index = integer(int64_t(offset));
    auto address = LLVMBuildGEP2(builder, i8, args, &index, 1, "");
    auto value = LLVMBuildLoad2(builder, type, address, "");
    LLVMSetAlignment(value, 1);
    fields.emplace(offset, value);
    return value;
  }
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
    auto xx = interior ? LLVMBuildAdd(b, x, integer(n.dx), "") : clamp_coord(x, n.dx, p.options.width);
    auto yy = clamp_coord(y, n.dy, p.options.height);
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
  LLVMValueRef math_call(Op op, LLVMValueRef a, LLVMValueRef c, iris_math_mode math) {
    if (auto address = scalar_math_address(op, math)) {
      const unsigned count = arity(op);
      const std::string name = "iris_math_" + std::to_string(unsigned(op));
      LLVMTypeRef params[] = {f32, f32};
      auto type = LLVMFunctionType(f32, params, count, 0);
      auto fn = LLVMGetNamedFunction(m.module, name.c_str());
      std::string mapping;
      uintptr_t vector_address = vector_math ? vector_math_address(op, math) : 0;
      if (!fn) {
        fn = LLVMAddFunction(m.module, name.c_str(), type);
        pure(fn);
        symbol(name, address);
        if (vector_address) {
          auto vector = LLVMVectorType(f32, 8);
          LLVMTypeRef vp[] = {vector, vector};
          auto vf = LLVMAddFunction(m.module, (name + "_v8").c_str(), LLVMFunctionType(vector, vp, count, 0));
          pure(vf);
          vector_functions.push_back(vf);
          symbol(name + "_v8", vector_address);
        }
      }
      LLVMValueRef values[] = {a, c};
      auto call = LLVMBuildCall2(b, type, fn, values, count, "");
      if (vector_address) {
        mapping = "_ZGV_LLVM_N8" + std::string(count, 'v') + "_" + name + "(" + name + "_v8)";
        constexpr char attr[] = "vector-function-abi-variant";
        LLVMAddCallSiteAttribute(
            call, LLVMAttributeFunctionIndex,
            LLVMCreateStringAttribute(m.context, attr, sizeof(attr) - 1, mapping.c_str(), unsigned(mapping.size())));
      }
      return call;
    }
    LLVMTypeRef types[] = {i32, f32, f32, i32};
    auto type = LLVMFunctionType(f32, types, 4, 0);
    auto address = LLVMConstInt(iptr, reinterpret_cast<uintptr_t>(&iris_host_math), 0);
    auto fn = LLVMConstIntToPtr(address, ptr);
    LLVMValueRef args[] = {LLVMConstInt(i32, static_cast<unsigned>(op), 0), a, c, LLVMConstInt(i32, math, 0)};
    auto call = LLVMBuildCall2(b, type, fn, args, 4, "");
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
  void lower_loop(const Program& p, LLVMValueRef fn, uint32_t begin, uint32_t end, bool unclamped) {
    if (begin == end)
      return;
    interior = unclamped;
    auto previous = LLVMGetInsertBlock(b);
    auto loop = LLVMAppendBasicBlockInContext(m.context, fn, unclamped ? "interior" : "boundary");
    auto done = LLVMAppendBasicBlockInContext(m.context, fn, "region.done");
    LLVMBuildBr(b, loop);
    LLVMPositionBuilderAtEnd(b, loop);
    x = LLVMBuildPhi(b, i64, "x");
    auto first = integer(begin);
    LLVMAddIncoming(x, &first, &previous, 1);
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
        case Op::Time: {
          auto f64 = LLVMDoubleTypeInContext(m.context);
          auto frame = LLVMBuildUIToFP(b, field(i64, offsetof(ExecuteArgs, frameno)), f64, "");
          v = p.options.expr.frame_count > 1
                  ? LLVMBuildFPTrunc(
                        b, LLVMBuildFDiv(b, frame, LLVMConstReal(f64, double(p.options.expr.frame_count - 1)), ""), f32,
                        "")
                  : number(0);
          break;
        }
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
          v = math_call(n.op, a, number(0), p.options.math);
          break;
        case Op::Fmod:
        case Op::Pow:
        case Op::Atan2:
          v = math_call(n.op, a, c, p.options.math);
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
    LLVMBuildCondBr(b, LLVMBuildICmp(b, LLVMIntULT, next, integer(end), ""), loop, done);
    LLVMPositionBuilderAtEnd(b, done);
  }
  void lower(const Program& p) {
    LLVMTypeRef params[] = {ptr, i32};
    auto fn = LLVMAddFunction(m.module, "iris_row", LLVMFunctionType(LLVMVoidTypeInContext(m.context), params, 2, 0));
    LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex,
                            LLVMCreateEnumAttribute(m.context, LLVMGetEnumAttributeKindForName("nounwind", 8), 0));
    entry = LLVMAppendBasicBlockInContext(m.context, fn, "entry");
    LLVMPositionBuilderAtEnd(b, entry);
    args = LLVMGetParam(fn, 0);
    y = LLVMBuildZExt(b, LLVMGetParam(fn, 1), i64, "");
    int64_t begin = 0, end = p.options.width;
    for (const auto& node : p.ir.nodes)
      if (node.op == Op::Input) {
        begin = std::max(begin, -int64_t(node.dx));
        end = std::min(end, int64_t(p.options.width) - node.dx);
      }
    if (begin < end) {
      lower_loop(p, fn, 0, uint32_t(begin), false);
      lower_loop(p, fn, uint32_t(begin), uint32_t(end), true);
      lower_loop(p, fn, uint32_t(end), p.options.width, false);
    } else {
      // Large offsets or short rows may leave no common unclamped interval.
      lower_loop(p, fn, 0, p.options.width, false);
    }
    LLVMBuildRetVoid(b);
    // The vector declarations are referenced by VFABI string attributes only.
    // Keep them through early GlobalDCE so the loop vectorizer can find them.
    if (!vector_functions.empty()) {
      auto count = unsigned(vector_functions.size());
      auto used = LLVMAddGlobal(m.module, LLVMArrayType(ptr, count), "llvm.compiler.used");
      LLVMSetInitializer(used, LLVMConstArray(ptr, vector_functions.data(), count));
      LLVMSetLinkage(used, LLVMAppendingLinkage);
      LLVMSetSection(used, "llvm.metadata");
    }
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
  NativeTarget target(LLVMOrcLLJITGetTripleString(code->jit));
  LLVMSetTarget(m.module, LLVMOrcLLJITGetTripleString(code->jit));
  LLVMSetDataLayout(m.module, LLVMOrcLLJITGetDataLayoutStr(code->jit));
  // LLVM host features account for OS AVX state support as well as CPUID.
  const std::string features = std::string(",") + target.features.get() + ",";
  bool vectors = features.find(",+avx2,") != std::string::npos && features.find(",+fma,") != std::string::npos;
  Lowering lowering(m, code->jit, vectors);
  lowering.lower(p);
  auto row = LLVMGetNamedFunction(m.module, "iris_row");
  const std::pair<const char*, const char*> attributes[] = {
      {"target-cpu", target.cpu.get()}, {"target-features", target.features.get()}};
  for (const auto& attribute : attributes)
    LLVMAddAttributeAtIndex(row, LLVMAttributeFunctionIndex,
                            LLVMCreateStringAttribute(m.context, attribute.first,
                                                      unsigned(std::strlen(attribute.first)), attribute.second,
                                                      unsigned(std::strlen(attribute.second))));
  char* message = nullptr;
  if (LLVMVerifyModule(m.module, LLVMReturnStatusAction, &message)) {
    std::unique_ptr<char, decltype(&LLVMDisposeMessage)> text(message, LLVMDisposeMessage);
    throw Error(IRIS_BACKEND_ERROR, text.get());
  }
  LLVMDisposeMessage(message);
  auto passes = LLVMCreatePassBuilderOptions();
  auto error = LLVMRunPasses(m.module, "default<O2>", target.machine, passes);
  LLVMDisposePassBuilderOptions(passes);
  check(error);
  code->vector_math_available = vectors && vector_math_address(Op::Sin, p.options.math) != 0;
  for (auto block = LLVMGetFirstBasicBlock(row); block; block = LLVMGetNextBasicBlock(block))
    for (auto instruction = LLVMGetFirstInstruction(block); instruction;
         instruction = LLVMGetNextInstruction(instruction))
      if (LLVMIsACallInst(instruction)) {
        auto called = LLVMGetCalledValue(instruction);
        const std::string name = LLVMGetValueName(called);
        if (name.find("iris_math_") == 0 && name.size() >= 3 && name.substr(name.size() - 3) == "_v8")
          ++code->vector_math_calls;
      }
  auto module = LLVMOrcCreateNewThreadSafeModule(m.module, m.thread_context);
  m.module = nullptr;
  check(LLVMOrcLLJITAddLLVMIRModule(code->jit, LLVMOrcLLJITGetMainJITDylib(code->jit), module));
  LLVMOrcExecutorAddress address = 0;
  check(LLVMOrcLLJITLookup(code->jit, &address, "iris_row")); // materialize before execute
  code->row = reinterpret_cast<JitCode::Row>(uintptr_t(address));
  return code;
}
} // namespace iris
