// Loads a Strategy from a shared library exporting the ALGO_EXPORT_STRATEGY
// symbols. The library handle is kept alive until the strategy is destroyed,
// because its vtable and code live inside the library.

#include <memory>

#include "algo/strategy/strategy_loader.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace algo {

namespace {

using AbiFn = int (*)();
using CreateFn = Strategy* (*)();
using DestroyFn = void (*)(Strategy*);

#if defined(_WIN32)
using LibHandle = HMODULE;
LibHandle open_lib(const std::string& p, std::string& err) {
  auto h = LoadLibraryA(p.c_str());
  if (!h) err = "LoadLibrary failed with code " + std::to_string(GetLastError());
  return h;
}
void* find_sym(LibHandle h, const char* n) { return reinterpret_cast<void*>(GetProcAddress(h, n)); }
void close_lib(LibHandle h) { FreeLibrary(h); }
#else
using LibHandle = void*;
LibHandle open_lib(const std::string& p, std::string& err) {
  void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) err = dlerror();
  return h;
}
void* find_sym(LibHandle h, const char* n) { return dlsym(h, n); }
void close_lib(LibHandle h) { dlclose(h); }
#endif

}  // namespace

StrategyPtr load_shared_library_strategy(const std::string& lib_path) {
  std::string err;
  LibHandle raw = open_lib(lib_path, err);
  if (!raw) throw StrategyLoadError("cannot load " + lib_path + ": " + err);
  std::shared_ptr<void> lib(reinterpret_cast<void*>(raw),
                            [](void* h) { close_lib(reinterpret_cast<LibHandle>(h)); });

  auto abi = reinterpret_cast<AbiFn>(find_sym(raw, "algo_strategy_abi_version"));
  auto create = reinterpret_cast<CreateFn>(find_sym(raw, "algo_create_strategy"));
  auto destroy = reinterpret_cast<DestroyFn>(find_sym(raw, "algo_destroy_strategy"));
  if (!abi || !create || !destroy) {
    throw StrategyLoadError(lib_path + " does not export a strategy. Add ALGO_EXPORT_STRATEGY(YourClass) "
                            "at the end of the file.");
  }
  if (abi() != ALGO_STRATEGY_ABI_VERSION) {
    throw StrategyLoadError("strategy ABI " + std::to_string(abi()) + " does not match engine ABI " +
                            std::to_string(ALGO_STRATEGY_ABI_VERSION) + "; recompile it");
  }
  Strategy* s = nullptr;
  try {
    s = create();
  } catch (const std::exception& e) {
    throw StrategyLoadError(std::string("strategy constructor threw: ") + e.what());
  }
  if (!s) throw StrategyLoadError("algo_create_strategy returned null");
  return StrategyPtr(s, [destroy, lib](Strategy* p) { destroy(p); });
}

}  // namespace algo
