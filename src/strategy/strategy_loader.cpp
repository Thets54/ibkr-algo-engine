#include "algo/strategy/strategy_loader.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace algo {

namespace fs = std::filesystem;

namespace {

std::string lower_ext(const std::string& path) {
  std::string e = fs::path(path).extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
  return e;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw StrategyLoadError("cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  return out + "'";
}

/// Runs a command, returns {exit_code, combined output}.
std::pair<int, std::string> run_command(const std::string& cmd) {
  std::string output;
  FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
  if (!pipe) return {-1, "popen failed"};
  std::array<char, 4096> buf{};
  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) output += buf.data();
  const int status = pclose(pipe);
  return {status, output};
}

const char* env_or(const char* name, const char* fallback) {
  const char* v = std::getenv(name);
  return v && *v ? v : fallback;
}

}  // namespace

StrategyKind strategy_kind(const std::string& path) {
  const auto e = lower_ext(path);
  if (e == ".py") return StrategyKind::Python;
  if (e == ".cpp" || e == ".cc" || e == ".cxx") return StrategyKind::CppSource;
  if (e == ".so" || e == ".dylib" || e == ".dll") return StrategyKind::SharedLibrary;
  return StrategyKind::Unsupported;
}

const char* to_string(StrategyKind k) {
  switch (k) {
    case StrategyKind::Python: return "python";
    case StrategyKind::CppSource: return "cpp";
    case StrategyKind::SharedLibrary: return "shared-library";
    case StrategyKind::Unsupported: return "unsupported";
  }
  return "unsupported";
}

StrategyLoader::StrategyLoader(std::string cache_dir) : cache_dir_(std::move(cache_dir)) {}

std::string StrategyLoader::compile_cpp(const std::string& source_path) const {
#if defined(_WIN32)
  (void)source_path;
  throw StrategyLoadError("on-the-fly C++ compilation is not supported on Windows; upload a .dll");
#else
  const std::string compiler = env_or("ALGO_CXX", ALGO_CXX_COMPILER);
  const std::string include_dir = env_or("ALGO_INCLUDE_DIR", ALGO_INCLUDE_DIR);
  const std::string source = read_file(source_path);

  // Cache key covers source, compiler and ABI so stale binaries are never reused.
  const std::size_t key = std::hash<std::string>{}(source + "|" + compiler + "|" +
                                                     std::to_string(ALGO_STRATEGY_ABI_VERSION));
  std::error_code ec;
  fs::create_directories(cache_dir_, ec);
  std::ostringstream name;
  name << fs::path(source_path).stem().string() << "-" << std::hex << key << ".so";
  const fs::path out = fs::absolute(fs::path(cache_dir_) / name.str());
  if (fs::exists(out)) return out.string();

  const fs::path tmp = out.string() + ".tmp";
  std::ostringstream cmd;
  cmd << shell_quote(compiler) << " -std=c++20 -O2 -fPIC -shared -fvisibility=hidden"
      << " -I" << shell_quote(include_dir) << " " << shell_quote(fs::absolute(source_path).string())
      << " -o " << shell_quote(tmp.string());
#if defined(__APPLE__)
  cmd << " -undefined dynamic_lookup";
#endif
  auto [code, output] = run_command(cmd.str());
  if (code != 0) {
    fs::remove(tmp, ec);
    throw StrategyLoadError("compilation of " + fs::path(source_path).filename().string() +
                            " failed:\n" + output);
  }
  fs::rename(tmp, out, ec);
  if (ec) throw StrategyLoadError("cannot move compiled library into cache: " + ec.message());
  return out.string();
#endif
}

StrategyPtr StrategyLoader::load(const std::string& path) const {
  if (!fs::exists(path)) throw StrategyLoadError("strategy file not found: " + path);
  switch (strategy_kind(path)) {
    case StrategyKind::Python:
      if (!python_strategies_supported()) {
        throw StrategyLoadError("this build has no Python support (rebuild with -DALGO_WITH_PYTHON=ON)");
      }
      return load_python_strategy(path);
    case StrategyKind::CppSource:
      return load_shared_library_strategy(compile_cpp(path));
    case StrategyKind::SharedLibrary:
      return load_shared_library_strategy(fs::absolute(path).string());
    case StrategyKind::Unsupported:
      break;
  }
  throw StrategyLoadError("unsupported strategy file type: " + path +
                          " (expected .py, .cpp, .cc, .so, .dylib or .dll)");
}

#if !defined(ALGO_WITH_PYTHON)
bool python_strategies_supported() { return false; }
StrategyPtr load_python_strategy(const std::string&) {
  throw StrategyLoadError("Python support not compiled in");
}
#endif

}  // namespace algo
