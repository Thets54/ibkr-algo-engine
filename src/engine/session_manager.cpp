#include "algo/engine/session_manager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "algo/data/csv_feed.hpp"
#include "algo/engine/backtester.hpp"
#include "algo/engine/engine.hpp"

#if defined(ALGO_WITH_IBKR)
#include "algo/broker/ibkr_broker.hpp"
#endif

namespace algo {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {
std::int64_t unix_now() { return static_cast<std::int64_t>(std::time(nullptr)); }
}  // namespace

const char* to_string(RunMode m) {
  switch (m) {
    case RunMode::Backtest: return "backtest";
    case RunMode::Paper: return "paper";
    case RunMode::Live: return "live";
  }
  return "backtest";
}

RunMode run_mode_from_string(const std::string& s) {
  if (s == "backtest") return RunMode::Backtest;
  if (s == "paper") return RunMode::Paper;
  if (s == "live") return RunMode::Live;
  throw std::invalid_argument("mode must be backtest, paper or live");
}

RunRequest run_request_from_json(const json& j) {
  RunRequest r;
  r.strategy = j.value("strategy", "");
  r.mode = run_mode_from_string(j.value("mode", "backtest"));
  if (j.contains("symbols")) {
    if (j["symbols"].is_string()) {
      std::stringstream ss(j["symbols"].get<std::string>());
      std::string s;
      while (std::getline(ss, s, ',')) {
        s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
        if (!s.empty()) r.symbols.push_back(s);
      }
    } else {
      r.symbols = j["symbols"].get<std::vector<std::string>>();
    }
  }
  r.bar_seconds = j.value("bar_seconds", 60);
  if (j.contains("params") && j["params"].is_object()) r.params = j["params"];
  r.data_dir = j.value("data_dir", "");
  r.starting_cash = j.value("starting_cash", 0.0);
  return r;
}

bool is_valid_strategy_filename(const std::string& name) {
  if (name.empty() || name.size() > 128 || name.front() == '.') return false;
  for (unsigned char c : name) {
    if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
  }
  return strategy_kind(name) != StrategyKind::Unsupported;
}

bool is_valid_symbol(const std::string& s) {
  if (s.empty() || s.size() > 24) return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ' ';
  });
}

std::unique_ptr<Broker> make_default_broker(const AppConfig& cfg, RunMode mode) {
#if defined(ALGO_WITH_IBKR)
  return std::make_unique<IbkrBroker>(cfg.ibkr, /*expect_paper=*/mode == RunMode::Paper);
#else
  (void)cfg;
  (void)mode;
  throw std::runtime_error(
      "this build has no IBKR support. Rebuild with -DALGO_WITH_IBKR=ON -DTWS_API_DIR=<TWS API "
      "source/cppclient/client> (see docs/ibkr-setup.md)");
#endif
}

SessionManager::SessionManager(AppConfig cfg, BrokerFactory factory)
    : cfg_(std::move(cfg)), factory_(std::move(factory)), loader_(cfg_.paths.cache_dir) {
  std::error_code ec;
  fs::create_directories(cfg_.paths.strategies_dir, ec);
  fs::create_directories(cfg_.paths.data_dir, ec);
}

SessionManager::~SessionManager() {
  stop();
  wait();
}

std::string SessionManager::strategy_path(const std::string& name) const {
  return (fs::path(cfg_.paths.strategies_dir) / name).string();
}

std::vector<StrategyInfo> SessionManager::list_strategies() const {
  std::vector<StrategyInfo> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(cfg_.paths.strategies_dir, ec)) {
    if (!e.is_regular_file()) continue;
    const std::string name = e.path().filename().string();
    if (!is_valid_strategy_filename(name)) continue;
    StrategyInfo info;
    info.name = name;
    info.kind = to_string(strategy_kind(name));
    info.size = e.file_size(ec);
    const auto ftime = e.last_write_time(ec);
    info.modified = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::file_clock::to_sys(ftime).time_since_epoch())
                        .count();
    out.push_back(info);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  return out;
}

std::string SessionManager::save_strategy(const std::string& name, const std::string& content) {
  if (!is_valid_strategy_filename(name)) {
    return "invalid file name: use letters, digits, '_', '-', '.' and a .py/.cpp/.cc/.so extension";
  }
  {
    std::lock_guard lock(mtx_);
    if (running_ && current_.strategy == name) return "cannot overwrite a strategy that is running";
  }
  const fs::path tmp = strategy_path(name) + ".upload";
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) return "cannot write to " + cfg_.paths.strategies_dir;
    out << content;
  }
  std::error_code ec;
  fs::rename(tmp, strategy_path(name), ec);
  if (ec) return "cannot save strategy: " + ec.message();
  return validate_strategy(name);
}

std::string SessionManager::validate_strategy(const std::string& name) const {
  if (!is_valid_strategy_filename(name)) return "invalid strategy name";
  try {
    auto s = loader_.load(strategy_path(name));
    return "";
  } catch (const std::exception& e) {
    return e.what();
  }
}

bool SessionManager::delete_strategy(const std::string& name) {
  if (!is_valid_strategy_filename(name)) return false;
  {
    std::lock_guard lock(mtx_);
    if (running_ && current_.strategy == name) return false;
  }
  std::error_code ec;
  return fs::remove(strategy_path(name), ec);
}

std::string SessionManager::read_strategy(const std::string& name) const {
  if (!is_valid_strategy_filename(name)) throw std::invalid_argument("invalid strategy name");
  std::ifstream in(strategy_path(name), std::ios::binary);
  if (!in) throw std::runtime_error("strategy not found: " + name);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string SessionManager::start(const RunRequest& req) {
  if (!is_valid_strategy_filename(req.strategy)) return "choose a strategy";
  if (!fs::exists(strategy_path(req.strategy))) return "strategy not found: " + req.strategy;
  if (req.symbols.empty()) return "enter at least one symbol";
  for (const auto& s : req.symbols) {
    if (!is_valid_symbol(s)) return "invalid symbol: " + s;
  }
  if (req.bar_seconds < 1) return "bar size must be at least 1 second";
  if (req.mode == RunMode::Live && !cfg_.allow_live) {
    return "live trading is disabled. Set \"allow_live\": true in the config file to enable it "
           "(paper trade first).";
  }
  std::lock_guard lock(mtx_);
  if (running_) return "a run is already active; stop it first";
  if (worker_.joinable()) worker_.join();
  running_ = true;
  stop_flag_ = false;
  progress_ = 0.0;
  state_ = "starting";
  error_.clear();
  current_ = req;
  started_at_ = unix_now();
  finished_at_ = 0;
  last_snapshot_ = json();
  last_metrics_ = json();
  worker_ = std::thread([this, req] {
    if (req.mode == RunMode::Backtest) run_backtest(req);
    else run_live(req);
  });
  return "";
}

void SessionManager::finish(const std::string& error, json final_snapshot) {
  std::lock_guard lock(mtx_);
  engine_ = nullptr;
  error_ = error;
  state_ = error.empty() ? "finished" : "error";
  finished_at_ = unix_now();
  if (!final_snapshot.is_null()) {
    if (final_snapshot.contains("metrics")) last_metrics_ = final_snapshot["metrics"];
    last_snapshot_ = std::move(final_snapshot);
  }
  running_ = false;
}

void SessionManager::run_backtest(RunRequest req) {
  json final_snapshot;
  std::string error;
  try {
    const std::string dir = req.data_dir.empty() ? cfg_.paths.data_dir : req.data_dir;
    auto bars = load_csv_dataset(dir, req.symbols);
    if (bars.empty()) throw std::runtime_error("no bars found for the selected symbols in " + dir);

    EngineOptions opts;
    opts.symbols = req.symbols;
    opts.bar_seconds = req.bar_seconds;
    opts.params = req.params;
    opts.starting_cash = req.starting_cash > 0 ? req.starting_cash : cfg_.starting_cash;
    opts.risk = cfg_.risk;
    Backtester bt(opts, cfg_.commission, loader_.load(strategy_path(req.strategy)));
    {
      std::lock_guard lock(mtx_);
      engine_ = &bt.engine();
      state_ = "running";
    }
    auto res = bt.run(bars, &stop_flag_, [this](double p) { progress_ = p; });
    final_snapshot = res.snapshot;
    final_snapshot["bars_processed"] = res.bars_processed;
    final_snapshot["completed"] = res.completed;
    if (bt.engine().strategy_failed()) error = "strategy failed, see logs";
    {
      std::lock_guard lock(mtx_);
      engine_ = nullptr;  // before bt (and its engine) is destroyed
    }
  } catch (const std::exception& e) {
    std::lock_guard lock(mtx_);
    engine_ = nullptr;
    error = e.what();
  }
  finish(error, std::move(final_snapshot));
}

void SessionManager::run_live(RunRequest req) {
  json final_snapshot;
  std::string error;
  try {
    auto broker = factory_(cfg_, req.mode);
    EngineOptions opts;
    opts.symbols = req.symbols;
    opts.bar_seconds = req.bar_seconds;
    opts.params = req.params;
    opts.starting_cash = req.starting_cash > 0 ? req.starting_cash : cfg_.starting_cash;
    opts.risk = cfg_.risk;
    opts.wall_clock = true;
    auto engine = std::make_unique<Engine>(opts, *broker, loader_.load(strategy_path(req.strategy)));

    std::string err;
    if (!broker->connect(err)) throw std::runtime_error("cannot connect to " + broker->name() + ": " + err);
    broker->request_positions();
    for (const auto& s : req.symbols) broker->request_history(s, cfg_.ibkr.warmup_bars, req.bar_seconds);
    broker->subscribe_bars(req.symbols, req.bar_seconds);
    {
      std::lock_guard lock(mtx_);
      engine_ = engine.get();
      state_ = "running";
    }
    engine->drain();  // apply positions + history received during start-up
    engine->start();

    auto last_reconnect = std::chrono::steady_clock::now();
    while (!stop_flag_) {
      engine->wait_and_drain(200);
      if (!broker->is_connected()) {
        const auto t = std::chrono::steady_clock::now();
        if (t - last_reconnect > std::chrono::seconds(10)) {
          last_reconnect = t;
          engine->log("broker disconnected, trying to reconnect");
          if (broker->connect(err)) broker->subscribe_bars(req.symbols, req.bar_seconds);
        }
      }
    }
    engine->stop();
    engine->drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let cancels reach the broker
    engine->drain();
    final_snapshot = engine->snapshot();
    {
      std::lock_guard lock(mtx_);
      engine_ = nullptr;
    }
    broker->disconnect();  // stop broker callbacks before the engine goes away
    engine.reset();
  } catch (const std::exception& e) {
    std::lock_guard lock(mtx_);
    engine_ = nullptr;
    error = e.what();
  }
  finish(error, std::move(final_snapshot));
}

void SessionManager::stop() { stop_flag_ = true; }

void SessionManager::kill(const std::string& reason) {
  std::lock_guard lock(mtx_);
  if (engine_) engine_->request_kill(reason);
}

void SessionManager::wait() {
  std::thread t;
  {
    std::lock_guard lock(mtx_);
    t.swap(worker_);
  }
  if (t.joinable()) t.join();
}

json SessionManager::status() const {
  std::lock_guard lock(mtx_);
  json j;
  j["state"] = state_;
  j["running"] = running_.load();
  j["error"] = error_;
  j["progress"] = progress_.load();
  j["started_at"] = started_at_;
  j["finished_at"] = finished_at_;
  j["allow_live"] = cfg_.allow_live;
  if (!current_.strategy.empty()) {
    j["run"] = {{"strategy", current_.strategy},
                {"mode", to_string(current_.mode)},
                {"symbols", current_.symbols},
                {"bar_seconds", current_.bar_seconds},
                {"params", current_.params}};
  }
  if (engine_) {
    j["engine"] = engine_->snapshot();
  } else if (!last_snapshot_.is_null()) {
    j["engine"] = last_snapshot_;
  }
  if (!last_metrics_.is_null()) j["metrics"] = last_metrics_;
  return j;
}

}  // namespace algo
