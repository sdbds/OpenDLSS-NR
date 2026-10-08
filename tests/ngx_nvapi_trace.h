#pragma once
#include <nvapi.h>
#include <nvapi_interface.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

// Optional test-process diagnostics. Redirect the loaded DLL's import, never
// the reference file on disk or the system driver's code.
namespace ngx_test::driver_trace {
using Query = void* (__cdecl*)(unsigned);
inline Query realQuery = nullptr;
inline std::filesystem::path directory;
inline std::mutex mutex;
inline std::set<uint64_t> saved;
inline std::map<NVDX_ObjectHandle, std::string> names;
inline unsigned preCapture = 0, postCapture = 0;
inline decltype(&NvAPI_D3D12_CreateCuModule) createModule = nullptr;
inline decltype(&NvAPI_D3D12_CreateCuFunction) createFunction = nullptr;
inline decltype(&NvAPI_D3D12_LaunchCuKernelChain) launchChain = nullptr;

inline NvAPI_Status __cdecl module(ID3D12Device* device, const void* blob, NvU32 bytes, NVDX_ObjectHandle* result) {
  if (blob && bytes) {
    const std::lock_guard lock(mutex);
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; i < bytes; ++i) { hash ^= static_cast<const uint8_t*>(blob)[i]; hash *= 1099511628211ull; }
    char filename[64]; sprintf_s(filename, "module-%016llx.bin", static_cast<unsigned long long>(hash));
    fprintf(stderr, "DRIVER module=%s bytes=%u\n", filename, bytes);
    if (saved.insert(hash).second) {
      std::ofstream file(directory / filename, std::ios::binary);
      file.write(static_cast<const char*>(blob), bytes);
    }
  }
  return createModule(device, blob, bytes, result);
}
inline NvAPI_Status __cdecl function(ID3D12Device* device, NVDX_ObjectHandle module, const char* name, NVDX_ObjectHandle* result) {
  const auto status = createFunction(device, module, name, result);
  if (status == NVAPI_OK && name && result) {
    const std::lock_guard lock(mutex);
    names[*result] = name;
    fprintf(stderr, "DRIVER function=%s\n", name);
  }
  return status;
}
inline NvAPI_Status __cdecl launch(ID3D12GraphicsCommandList* commands, const NVAPI_CU_KERNEL_LAUNCH_PARAMS* kernels, NvU32 count) {
  if (kernels) {
    const std::lock_guard lock(mutex);
    for (NvU32 i = 0; i < count; ++i) {
      const auto found = names.find(kernels[i].hFunction);
      if (found == names.end()) continue;
      const bool pre = found->second.find("cc_tinlayout_fused_pre_block") == 0;
      if (!pre && found->second.find("cc_tinlayout_fused_post_block") != 0) continue;
      const unsigned capture = pre ? preCapture++ : postCapture++;
      fprintf(stderr, "DRIVER args=%s size=%u capture=%u\n", found->second.c_str(), kernels[i].paramSize, capture);
      std::ofstream file(directory / ((pre ? "pre-" : "post-") + std::to_string(capture) + ".params"), std::ios::binary);
      if (kernels[i].pParams) file.write(static_cast<const char*>(kernels[i].pParams), kernels[i].paramSize);
    }
  }
  return launchChain(commands, kernels, count);
}
inline void* __cdecl query(unsigned id) {
  void* address = realQuery(id);
  for (const auto& entry : nvapi_interface_table) if (entry.id == id) {
    fprintf(stderr, "DRIVER query=%s address=%p\n", entry.func, address);
    if (!address) return nullptr;
    if (!strcmp(entry.func, "NvAPI_D3D12_LaunchCuKernelChain")) {
      launchChain = reinterpret_cast<decltype(launchChain)>(address); return reinterpret_cast<void*>(&launch);
    }
    if (!strcmp(entry.func, "NvAPI_D3D12_CreateCuModule")) {
      createModule = reinterpret_cast<decltype(createModule)>(address); return reinterpret_cast<void*>(&module);
    }
    if (!strcmp(entry.func, "NvAPI_D3D12_CreateCuFunction")) {
      createFunction = reinterpret_cast<decltype(createFunction)>(address); return reinterpret_cast<void*>(&function);
    }
    break;
  }
  return address;
}
inline FARPROC WINAPI getProc(HMODULE module, LPCSTR name) {
  FARPROC address = GetProcAddress(module, name);
  if (reinterpret_cast<uintptr_t>(name) > 0xffff && !strcmp(name, "nvapi_QueryInterface") && address) {
    realQuery = reinterpret_cast<Query>(address);
    return reinterpret_cast<FARPROC>(&query);
  }
  return address;
}
inline void install(HMODULE module, const std::filesystem::path& output) {
  directory = output; std::filesystem::create_directories(directory);
  auto* base = reinterpret_cast<uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const auto rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  bool installed = false;
  for (auto* imported = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + rva); imported->Name; ++imported) {
    if (!imported->OriginalFirstThunk) continue;
    const auto* imports = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imported->OriginalFirstThunk);
    auto* functions = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imported->FirstThunk);
    for (size_t i = 0; imports[i].u1.AddressOfData; ++i) {
      if (IMAGE_SNAP_BY_ORDINAL64(imports[i].u1.Ordinal)) continue;
      const auto* name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + imports[i].u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(name->Name), "GetProcAddress")) continue;
      DWORD protection = 0, unused = 0;
      if (!VirtualProtect(&functions[i], sizeof(functions[i]), PAGE_READWRITE, &protection)) throw std::runtime_error("cannot instrument import");
      functions[i].u1.Function = reinterpret_cast<ULONGLONG>(&getProc);
      VirtualProtect(&functions[i], sizeof(functions[i]), protection, &unused);
      installed = true;
    }
  }
  if (!installed) throw std::runtime_error("target does not import GetProcAddress");
}
}  // namespace ngx_test::driver_trace
