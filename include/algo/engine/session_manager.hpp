#pragma once
// Owns the strategy library on disk and the single active run (backtest,
// paper or live). Used by both the web server and the CLI. Thread-safe.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "algo/broker/broker.hpp"
#include "algo/core/config.hpp"
#include "algo/strategy/strategy_loader.hpp"

namespace algo {

class Engine;

enum class RunMode { Backtest, Paper, Live };
const char* to_string(RunMode m);
RunMode run_mode_from_string(const std::string& s);  ///< throws std::invalid_argument

struct RunRequest {
  std::string strategy;  ///< file name inside the strategies directory
  RunMode mode = RunMode::Backtest;
  std::vector<std::string> symbols;
  int bar_seconds = 60;
  nlohmann::json params = nlohmann::json::object();
  std::string data_dir;        ///< backtest only; defaults to config paths.data_dir
  double starting_cash = 0.0;  ///< 0 = use config
};

RunRequest run_request_from_json(const nlohmann::json& j);

struct StrategyInfo {
  std::string name;
  std::string kind;
  std::uintmax_t size = 0;
  std::int64_t modified = 0;
};

using BrokerFactory = std::function<std::unique_ptr<Broker>(const AppConfig&, RunMode)>;

/// Default factory: IbkrBroker when built with ALGO_WITH_IBKR, else throws.
std::unique_ptr<Broker> make_default_broker(const AppConfig& cfg, RunMode mode);

/// True if `name` is a safe file name with a supported strategy extension.
bool is_valid_strategy_filename(const std::string& name);
bool is_valid_symbol(const std::string& symbol);

class SessionManager {
 public:
  explicit SessionManager(AppConfig cfg, BrokerFactory factory = make_default_broker);
  ~SessionManager();

  // ---- strategy library -------------------------------------------------
  std::vector<StrategyInfo> list_strategies() const;
  /// Saves the file, then validates it by loading it. On a load error the
  /// file is kept (so the user can fix it) and the error is returned.
  std::string save_strategy(const std::string& name, const std::string& content);
  std::string validate_strategy(const std::string& name) const;  ///< "" if OK
  bool delete_strategy(const std::string& name);
  std::string read_strategy(const std::string& name) const;     ///< throws if missing

  // ---- runs ---------------------------------------------------------------
  /// Starts a run in the background. Returns "" or an error message.
  std::string start(const RunRequest& req);
  void stop();             ///< graceful: stops the strategy, cancels orders
  void kill(const std::string& reason);  ///< kill switch: cancel + flatten
  void wait();             ///< blocks until the current run thread exits
  [[nodiscard]] bool running() const { return running_.load(); }
  [[nodiscard]] nlohmann::json status() const;

  [[nodiscard]] const AppConfig& config() const { return cfg_; }
  [[nodiscard]] std::string strategy_path(const std::string& name) const;

 private:
  void run_backtest(RunRequest req);
  void run_live(RunRequest req);
  void finish(const std::string& error, nlohmann::json final_snapshot);

  AppConfig cfg_;
  BrokerFactory factory_;
  StrategyLoader loader_;

  mutable std::mutex mtx_;  // guards everything below except atomics
  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_flag_{false};
  std::atomic<double> progress_{0.0};
  Engine* engine_ = nullptr;  // valid while a run is active
  std::string state_ = "idle";  // idle | starting | running | finished | error
  std::string error_;
  RunRequest current_;
  std::int64_t started_at_ = 0;
  std::int64_t finished_at_ = 0;
  nlohmann::json last_snapshot_;
  nlohmann::json last_metrics_;
};

}  // namespace algo
