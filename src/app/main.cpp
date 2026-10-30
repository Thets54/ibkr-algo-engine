// algoengine: command-line entry point.
//
//   algoengine init                         write a default config.json
//   algoengine serve [--config F]           dashboard on http://127.0.0.1:8080
//   algoengine backtest -s FILE --symbols A,B [--data DIR] [--bar-seconds N] [--param k=v]
//   algoengine run -s FILE --symbols A,B --mode paper|live [--bar-seconds N] [--param k=v]
//   algoengine validate FILE

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
#include <thread>

#include <nlohmann/json.hpp>

#include "algo/core/config.hpp"
#include "algo/engine/session_manager.hpp"
#include "algo/server/web_server.hpp"
#include "algo/strategy/strategy_loader.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace algo;

namespace {

std::atomic<bool> g_interrupted{false};
extern "C" void on_signal(int) { g_interrupted = true; }

constexpr const char* kVersion = "0.1.0";

void usage() {
  std::cout << R"(algoengine )" << kVersion << R"( - algorithmic trading engine for Interactive Brokers

Usage:
  algoengine init [--config config.json]
      Write a default config file and create the strategies/ and data/ folders.

  algoengine serve [--config config.json] [--host 127.0.0.1] [--port 8080]
      Start the web dashboard: upload strategies, run backtests, paper or live trading.

  algoengine backtest --strategy FILE --symbols AAPL,MSFT [--data DIR]
                      [--bar-seconds 86400] [--cash 100000] [--param name=value ...]
                      [--json results.json] [--config config.json]
      Run a strategy on CSV bars (DIR/SYMBOL.csv) and print performance metrics.

  algoengine run --strategy FILE --symbols AAPL --mode paper|live
                 [--bar-seconds 60] [--param name=value ...] [--config config.json]
      Trade through TWS / IB Gateway until Ctrl+C. Live requires "allow_live": true.

  algoengine validate FILE
      Load (and compile, for C++) a strategy without running it.

Strategy files: .py (Python), .cpp/.cc (compiled automatically), .so (prebuilt).
)";
}

struct Args {
  std::string command;
  std::vector<std::string> positional;
  std::map<std::string, std::vector<std::string>> opts;

  [[nodiscard]] std::string get(const std::string& k, const std::string& def = "") const {
    auto it = opts.find(k);
    return it == opts.end() || it->second.empty() ? def : it->second.back();
  }
  [[nodiscard]] bool has(const std::string& k) const { return opts.count(k) > 0; }
};

Args parse_args(int argc, char** argv) {
  static const std::map<std::string, std::string> aliases = {{"-s", "--strategy"}, {"-c", "--config"},
                                                             {"-p", "--port"}, {"-m", "--mode"}};
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string t = argv[i];
    if (auto al = aliases.find(t); al != aliases.end()) t = al->second;
    if (t.rfind("--", 0) == 0) {
      std::string key = t, value;
      if (auto eq = t.find('='); eq != std::string::npos && t.find("--param") != 0) {
        key = t.substr(0, eq);
        value = t.substr(eq + 1);
      } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        value = argv[++i];
      }
      a.opts[key].push_back(value);
    } else if (a.command.empty()) {
      a.command = t;
    } else {
      a.positional.push_back(t);
    }
  }
  return a;
}

AppConfig load_config_or_default(const Args& a) {
  const std::string path = a.get("--config", "config.json");
  if (fs::exists(path)) return load_config(path);
  if (a.has("--config")) throw std::runtime_error("config file not found: " + path);
  return AppConfig{};
}

json parse_params(const Args& a) {
  json p = json::object();
  auto it = a.opts.find("--param");
  if (it == a.opts.end()) return p;
  for (const auto& kv : it->second) {
    auto eq = kv.find('=');
    if (eq == std::string::npos) throw std::runtime_error("--param expects name=value, got '" + kv + "'");
    const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    try {
      size_t pos = 0;
      double d = std::stod(v, &pos);
      if (pos == v.size()) { p[k] = d; continue; }
    } catch (...) {
    }
    p[k] = v;
  }
  return p;
}

RunRequest request_from_args(const Args& a, AppConfig& cfg) {
  const std::string strategy = a.get("--strategy");
  if (strategy.empty()) throw std::runtime_error("--strategy FILE is required");
  if (!fs::exists(strategy)) throw std::runtime_error("strategy file not found: " + strategy);
  // Run the file in place: its folder becomes the strategies directory.
  cfg.paths.strategies_dir = fs::absolute(strategy).parent_path().string();
  json j = {{"strategy", fs::path(strategy).filename().string()},
            {"mode", a.get("--mode", "backtest")},
            {"symbols", a.get("--symbols")},
            {"bar_seconds", std::stoi(a.get("--bar-seconds", "0"))},
            {"params", parse_params(a)},
            {"data_dir", a.get("--data", "")},
            {"starting_cash", std::stod(a.get("--cash", "0"))}};
  RunRequest r = run_request_from_json(j);
  if (r.bar_seconds == 0) r.bar_seconds = r.mode == RunMode::Backtest ? 86'400 : 60;
  return r;
}

void print_metrics(const json& m) {
  auto pct = [](double v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << v * 100 << "%";
    return os.str();
  };
  std::cout << std::fixed << std::setprecision(2)
            << "\n  Start equity        " << m.value("start_equity", 0.0)
            << "\n  End equity          " << m.value("end_equity", 0.0)
            << "\n  Total return        " << pct(m.value("total_return", 0.0))
            << "\n  Annualized return   " << pct(m.value("annualized_return", 0.0))
            << "\n  Annualized vol      " << pct(m.value("annualized_vol", 0.0))
            << "\n  Sharpe (rf=0)       " << m.value("sharpe", 0.0)
            << "\n  Max drawdown        " << pct(m.value("max_drawdown", 0.0))
            << "\n  Closed trades       " << m.value("closed_trades", 0)
            << "\n  Win rate            " << pct(m.value("win_rate", 0.0))
            << "\n  Commissions         " << m.value("total_commission", 0.0) << "\n\n";
}

void print_logs(const json& status, size_t max_lines) {
  if (!status.contains("engine") || !status["engine"].contains("logs")) return;
  const auto& logs = status["engine"]["logs"];  // newest first
  const size_t n = std::min(max_lines, logs.size());
  for (size_t i = n; i-- > 0;) {
    std::cout << "  [" << logs[i].value("level", "") << "] " << logs[i].value("message", "") << "\n";
  }
}

int cmd_init(const Args& a) {
  const std::string path = a.get("--config", "config.json");
  if (fs::exists(path)) {
    std::cout << path << " already exists, leaving it untouched.\n";
  } else {
    std::ofstream(path) << config_to_json(AppConfig{}).dump(2) << "\n";
    std::cout << "wrote " << path << "\n";
  }
  AppConfig cfg = load_config(path);
  fs::create_directories(cfg.paths.strategies_dir);
  fs::create_directories(cfg.paths.data_dir);
  std::cout << "strategies folder: " << cfg.paths.strategies_dir << "\ndata folder:       " << cfg.paths.data_dir
            << "\n\nNext: algoengine serve, then open http://" << cfg.server.host << ":" << cfg.server.port << "\n";
  return 0;
}

int cmd_serve(const Args& a) {
  AppConfig cfg = load_config_or_default(a);
  if (a.has("--host")) cfg.server.host = a.get("--host");
  if (a.has("--port")) cfg.server.port = std::stoi(a.get("--port"));
  SessionManager sessions(cfg);
  WebServer server(sessions, cfg.server.host, cfg.server.port);
  if (!server.bind()) {
    std::cerr << "cannot bind " << cfg.server.host << ":" << cfg.server.port << " (port in use?)\n";
    return 1;
  }
  if (cfg.server.host != "127.0.0.1" && cfg.server.host != "localhost") {
    std::cerr << "WARNING: dashboard bound to " << cfg.server.host
              << ". Anyone who can reach it can upload and run code with your IBKR session.\n";
  }
  std::cout << "algoengine " << kVersion << " dashboard: http://" << cfg.server.host << ":" << server.port()
            << "\n  strategies: " << fs::absolute(cfg.paths.strategies_dir).string()
            << "\n  data:       " << fs::absolute(cfg.paths.data_dir).string()
            << "\n  IBKR:       " << cfg.ibkr.host << ":" << cfg.ibkr.port << " (clientId " << cfg.ibkr.client_id
            << ")" << (cfg.allow_live ? "  LIVE TRADING ENABLED" : "") << "\nPress Ctrl+C to quit.\n";
  server.start_background();
  while (!g_interrupted) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::cout << "\nshutting down...\n";
  sessions.stop();
  sessions.wait();
  server.stop();
  return 0;
}

int cmd_run_or_backtest(const Args& a, bool backtest) {
  AppConfig cfg = load_config_or_default(a);
  Args args = a;
  if (backtest) args.opts["--mode"] = {"backtest"};
  else if (!a.has("--mode")) args.opts["--mode"] = {"paper"};
  RunRequest req = request_from_args(args, cfg);
  SessionManager sessions(cfg);
  if (auto err = sessions.start(req); !err.empty()) {
    std::cerr << "error: " << err << "\n";
    return 1;
  }
  std::cout << (backtest ? "backtesting " : "trading ") << req.strategy << " (" << to_string(req.mode)
            << ") on " << req.symbols.size() << " symbol(s). Ctrl+C to stop.\n";
  int last_pct = -1;
  std::string last_line;
  while (sessions.running()) {
    if (g_interrupted) {
      sessions.stop();
      g_interrupted = false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(backtest ? 50 : 1000));
    json st = sessions.status();
    if (backtest) {
      const int pct = static_cast<int>(st.value("progress", 0.0) * 100);
      if (pct / 10 != last_pct / 10) {
        std::cout << "  " << pct << "%\n" << std::flush;
        last_pct = pct;
      }
    } else if (st.contains("engine")) {
      // Logs come newest first: print everything newer than the last line shown.
      const auto& logs = st["engine"]["logs"];
      std::vector<std::string> fresh;
      for (const auto& l : logs) {
        std::string line = "  [" + l.value("level", "") + "] " + l.value("message", "");
        if (line == last_line || fresh.size() >= 20) break;
        fresh.push_back(std::move(line));
      }
      for (auto it = fresh.rbegin(); it != fresh.rend(); ++it) std::cout << *it << "\n";
      if (!fresh.empty()) last_line = fresh.front();
    }
  }
  sessions.wait();
  json st = sessions.status();
  if (!st.value("error", std::string()).empty()) {
    print_logs(st, 15);
    std::cerr << "error: " << st["error"].get<std::string>() << "\n";
    return 1;
  }
  if (st.contains("metrics")) print_metrics(st["metrics"]);
  else if (st.contains("engine")) print_logs(st, 10);
  if (a.has("--json")) {
    std::ofstream(a.get("--json")) << st.dump(2) << "\n";
    std::cout << "full results written to " << a.get("--json") << "\n";
  }
  return 0;
}

int cmd_validate(const Args& a) {
  if (a.positional.empty()) {
    std::cerr << "usage: algoengine validate FILE\n";
    return 2;
  }
  AppConfig cfg = load_config_or_default(a);
  StrategyLoader loader(cfg.paths.cache_dir);
  try {
    auto s = loader.load(a.positional[0]);
    std::cout << "OK: " << a.positional[0] << " (" << to_string(strategy_kind(a.positional[0])) << ")\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "INVALID: " << e.what() << "\n";
    return 1;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  const Args a = parse_args(argc, argv);
  try {
    if (a.command.empty() || a.command == "help" || a.has("--help")) {
      usage();
      return a.command.empty() ? 2 : 0;
    }
    if (a.command == "version" || a.has("--version")) {
      std::cout << kVersion << "\n";
      return 0;
    }
    if (a.command == "init") return cmd_init(a);
    if (a.command == "serve") return cmd_serve(a);
    if (a.command == "backtest") return cmd_run_or_backtest(a, true);
    if (a.command == "run") return cmd_run_or_backtest(a, false);
    if (a.command == "validate") return cmd_validate(a);
    std::cerr << "unknown command '" << a.command << "'\n\n";
    usage();
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
