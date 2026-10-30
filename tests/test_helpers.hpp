#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "algo/core/types.hpp"
#include "algo/strategy/strategy_loader.hpp"

namespace algo::test {

namespace fs = std::filesystem;

/// Unique temporary directory removed at the end of the test.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> n{0};
    path_ = fs::temp_directory_path() /
            ("algo_test_" + std::to_string(::getpid()) + "_" + std::to_string(n++));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  [[nodiscard]] const fs::path& path() const { return path_; }
  [[nodiscard]] std::string str() const { return path_.string(); }
  std::string write(const std::string& name, const std::string& content) const {
    auto p = path_ / name;
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
    return p.string();
  }

 private:
  fs::path path_;
};

inline Bar bar(const std::string& sym, Timestamp t, double o, double h, double l, double c, double v = 1000) {
  Bar b;
  b.symbol = sym;
  b.time = t;
  b.open = o;
  b.high = h;
  b.low = l;
  b.close = c;
  b.volume = v;
  return b;
}

/// Daily bars with a deterministic random walk.
inline std::vector<Bar> random_walk(const std::string& sym, int n, double start = 100.0, unsigned seed = 7) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> z(0.0, 0.01);
  std::vector<Bar> out;
  double p = start;
  for (int i = 0; i < n; ++i) {
    const double o = p;
    const double c = p * std::exp(z(rng));
    out.push_back(bar(sym, 1'700'000'000 + i * 86'400, o, std::max(o, c) * 1.002, std::min(o, c) * 0.998, c));
    p = c;
  }
  return out;
}

inline std::string to_csv(const std::vector<Bar>& bars) {
  std::string s = "time,open,high,low,close,volume\n";
  for (const auto& b : bars) {
    s += std::to_string(b.time) + "," + std::to_string(b.open) + "," + std::to_string(b.high) + "," +
         std::to_string(b.low) + "," + std::to_string(b.close) + "," + std::to_string(b.volume) + "\n";
  }
  return s;
}

/// Strategy built from lambdas, for engine tests.
class LambdaStrategy : public Strategy {
 public:
  std::function<void(StrategyContext&)> start;
  std::function<void(StrategyContext&, const Bar&)> bar_fn;
  std::function<void(StrategyContext&, const Fill&)> fill_fn;
  std::function<void(StrategyContext&, const Order&)> reject_fn;
  void on_start(StrategyContext& c) override { if (start) start(c); }
  void on_bar(StrategyContext& c, const Bar& b) override { if (bar_fn) bar_fn(c, b); }
  void on_fill(StrategyContext& c, const Fill& f) override { if (fill_fn) fill_fn(c, f); }
  void on_order_rejected(StrategyContext& c, const Order& o) override { if (reject_fn) reject_fn(c, o); }
};

inline StrategyPtr own(Strategy* s) {
  return StrategyPtr(s, [](Strategy* p) { delete p; });
}

/// Polls `cond` until true or timeout.
inline bool eventually(const std::function<bool()>& cond, int timeout_ms = 10'000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return cond();
}

inline std::string source_path(const std::string& rel) { return std::string(ALGO_SOURCE_DIR) + "/" + rel; }

}  // namespace algo::test
