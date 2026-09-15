#include "avs_host.hpp"

AVSC_EXPORT const char* AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment* env) {
  try {
    static AvsApi api(GetModuleHandleW(L"avisynth.dll"));
    register_iris_expr_avs(api, env);
    return "IrisExpr: scalar and optional LLVM expression engine";
  } catch (const std::exception& e) {
    // The legacy C plugin ABI returns a description, not an error value.
    // Keep failures visible in that result and do not cross the DLL with C++ exceptions.
    try {
      static thread_local std::string error;
      error = std::string("IrisExpr initialization failed: ") + e.what();
      return error.c_str();
    } catch (...) {
      return "IrisExpr initialization failed while preparing diagnostic";
    }
  } catch (...) {
    return "IrisExpr initialization failed";
  }
}
