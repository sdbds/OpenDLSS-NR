#pragma once
#include <nvsdk_ngx.h>
#include <stdexcept>
#include <string>

namespace ngx {
struct Error : std::runtime_error {
  NVSDK_NGX_Result code;
  Error(NVSDK_NGX_Result status, const std::string& text) : std::runtime_error(text), code(status) {}
};
inline void require(bool condition, NVSDK_NGX_Result code, const std::string& text) {
  if (!condition) throw Error(code, text);
}
template<class T> T optional(const NVSDK_NGX_Parameter* parameters, const char* name, T fallback) {
  T value = fallback;
  return parameters && parameters->Get(name, &value) == NVSDK_NGX_Result_Success ? value : fallback;
}
template<class T> T required(const NVSDK_NGX_Parameter* parameters, const char* name) {
  T value{};
  require(parameters && parameters->Get(name, &value) == NVSDK_NGX_Result_Success,
          NVSDK_NGX_Result_FAIL_MissingInput, std::string("missing ") + name);
  return value;
}
}  // namespace ngx
