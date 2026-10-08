#pragma once
#include <nvsdk_ngx.h>

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <variant>

namespace ngx_test {

// A test-owned parameter map. The inherited declarations fix the MSVC ABI;
// hand-written vtable slot numbers would not test the real caller contract.
class Parameters final : public NVSDK_NGX_Parameter {
 public:
  bool trace = false;

#define NGX_TEST_PARAMETER(Type) \
  void Set(const char* key, Type value) override { \
    values_[key] = value; \
    if (trace) fprintf(stderr, "NGX Set<%s>(%s)\n", typeName<Type>(), key); \
  } \
  NVSDK_NGX_Result Get(const char* key, Type* value) const override { return read(key, value); }
  NGX_TEST_PARAMETER(unsigned long long)
  NGX_TEST_PARAMETER(float)
  NGX_TEST_PARAMETER(double)
  NGX_TEST_PARAMETER(unsigned int)
  NGX_TEST_PARAMETER(int)
  NGX_TEST_PARAMETER(ID3D11Resource*)
  NGX_TEST_PARAMETER(ID3D12Resource*)
  NGX_TEST_PARAMETER(void*)
#undef NGX_TEST_PARAMETER
  void Reset() override { values_.clear(); traced_.clear(); }

 private:
  using Value = std::variant<unsigned long long, float, double, unsigned int, int,
                             ID3D11Resource*, ID3D12Resource*, void*>;
  std::map<std::string, Value> values_;
  mutable std::set<std::string> traced_;

  template<class T> static const char* typeName() {
    if constexpr (std::is_same_v<T, float>) return "float";
    else if constexpr (std::is_same_v<T, double>) return "double";
    else if constexpr (std::is_same_v<T, int>) return "int";
    else if constexpr (std::is_same_v<T, unsigned int>) return "uint";
    else if constexpr (std::is_same_v<T, unsigned long long>) return "u64";
    else if constexpr (std::is_same_v<T, ID3D11Resource*>) return "d3d11";
    else if constexpr (std::is_same_v<T, ID3D12Resource*>) return "d3d12";
    else return "pointer";
  }
  template<class T> NVSDK_NGX_Result read(const char* key, T* out) const {
    bool found = false;
    const auto item = values_.find(key);
    if (item != values_.end() && out) {
      std::visit([&](auto value) {
        using U = decltype(value);
        if constexpr (std::is_arithmetic_v<T> && std::is_arithmetic_v<U>) {
          *out = static_cast<T>(value);
          found = true;
        } else if constexpr (std::is_pointer_v<T> && std::is_pointer_v<U>) {
          *out = reinterpret_cast<T>(value);
          found = true;
        }
      }, item->second);
    }
    if (trace) {
      const std::string tag = std::string(typeName<T>()) + ":" + key + (found ? ":set" : ":missing");
      if (traced_.insert(tag).second) fprintf(stderr, "NGX Get<%s>(%s): %s\n", typeName<T>(), key, found ? "set" : "missing");
    }
    return found ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
};

}  // namespace ngx_test
