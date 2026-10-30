#pragma once
// Turns a user-supplied file into a running Strategy object.
//
//   .py              -> embedded CPython interpreter (pybind11)
//   .cpp / .cc       -> compiled on the fly into a shared library, then loaded
//   .so / .dylib/.dll -> loaded directly (must use ALGO_EXPORT_STRATEGY)
//
// Uploaded strategies are arbitrary code running inside this process with
// your IBKR session. Only load files you wrote or reviewed.

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "algo/strategy/strategy.hpp"

namespace algo {

using StrategyPtr = std::unique_ptr<Strategy, std::function<void(Strategy*)>>;

class StrategyLoadError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

enum class StrategyKind { Python, CppSource, SharedLibrary, Unsupported };

StrategyKind strategy_kind(const std::string& path);
const char* to_string(StrategyKind k);

class StrategyLoader {
 public:
  /// cache_dir holds compiled .so files keyed by a hash of the source.
  explicit StrategyLoader(std::string cache_dir = ".algo_cache");

  /// Throws StrategyLoadError with a readable message (compiler output,
  /// Python traceback, missing export...) on failure.
  StrategyPtr load(const std::string& path) const;

  /// Compiles a C++ strategy and returns the path of the shared library.
  std::string compile_cpp(const std::string& source_path) const;

 private:
  std::string cache_dir_;
};

// Implemented in cpp_plugin.cpp / python_strategy.cpp
StrategyPtr load_shared_library_strategy(const std::string& lib_path);
StrategyPtr load_python_strategy(const std::string& py_path);
bool python_strategies_supported();

}  // namespace algo
