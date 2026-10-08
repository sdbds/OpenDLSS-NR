#include "ngx_parameters.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

void require(bool condition, const char* text) {
  if (!condition) throw std::runtime_error(text);
}

int main() try {
  ngx_test::Parameters map;
  NVSDK_NGX_Parameter* abi = &map;
  abi->Set("Float", 0.375f);
  abi->Set("UInt", 1920u);
  abi->Set("Int", -7);
  abi->Set("Double", 1.25);
  abi->Set("U64", 0x123456789abcdef0ull);
  void* marker = &map;
  abi->Set("Pointer", marker);
  float f = 0;
  unsigned int u = 0;
  int i = 0;
  double d = 0;
  unsigned long long big = 0;
  void* pointer = nullptr;
  require(abi->Get("Float", &f) == NVSDK_NGX_Result_Success && f == 0.375f, "float round trip");
  require(abi->Get("UInt", &u) == NVSDK_NGX_Result_Success && u == 1920, "uint round trip");
  require(abi->Get("Int", &i) == NVSDK_NGX_Result_Success && i == -7, "int round trip");
  require(abi->Get("Double", &d) == NVSDK_NGX_Result_Success && d == 1.25, "double round trip");
  require(abi->Get("U64", &big) == NVSDK_NGX_Result_Success && big == 0x123456789abcdef0ull, "u64 precision");
  require(abi->Get("Pointer", &pointer) == NVSDK_NGX_Result_Success && pointer == marker, "pointer round trip");
  require(abi->Get("UInt", &f) == NVSDK_NGX_Result_Success && f == 1920.0f, "numeric conversion");
  require(abi->Get("Pointer", &u) != NVSDK_NGX_Result_Success, "pointer is not a number");
  u = 42;
  require(abi->Get("Missing", &u) != NVSDK_NGX_Result_Success && u == 42, "missing output is unchanged");
  abi->Reset();
  require(abi->Get("Float", &f) != NVSDK_NGX_Result_Success, "reset clears values");
  puts("PASS NGX parameter round trips through the public ABI");
  return 0;
} catch (const std::exception& error) {
  fprintf(stderr, "FAIL: %s\n", error.what());
  return 1;
}
