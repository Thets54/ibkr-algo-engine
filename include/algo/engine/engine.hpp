#pragma once
// The engine wires a strategy to a broker through the risk manager.
//
//   broker threads ──(BrokerListener: enqueue only)──▶ event queue
//                                                          │
//   engine thread  ◀── drain(): bars, fills, statuses ─────┘
//        │  updates Portfolio, RiskManager, calls Strategy hooks
//        └─▶ StrategyContext orders ──▶ RiskManager::check ──▶ Broker::submit
//
// Backtests use the same code path: the Backtester feeds bars to the
// SimBroker, then calls drain() synchronously after each time step.

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "algo/broker/broker.hpp"
#include "algo/core/config.hpp"
#include "algo/core/portfolio.hpp"
#include "algo/risk/risk_manager.hpp"
#include "algo/strategy/strategy_loader.hpp"

namespace algo {

struct LogEntry {
  Timestamp time = 0;
  std::string level;  ///< "info", "warn", "error", "strategy"
  std::string message;
};

struct EngineOptions {
  std::vector<std::string> symbols;
  int bar_seconds = 60;
  nlohmann::json params = nlohmann::json::object();
  double starting_cash = 100'000.0;
  RiskConfig risk;
  /// Max bars kept per symbol for ctx.history().
  size_t history_capacity = 5'000;
  /// Use wall-clock time (live) instead of the latest bar time (backtest).
  bool wall_clock = false;
};

class Engine final : public StrategyContext, public BrokerListener {
 public:
  Engine(EngineOptions options, Broker& broker, StrategyPtr strategy);
  ~Engine() override;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /// Calls strategy.on_start. Must be called once before drain().
  void start();
  /// Processes all queued events. Returns the number processed.
  size_t drain();
  /// Blocks until events arrive (or timeout) then drains. For live loops.
  size_t wait_and_drain(int timeout_ms);
  /// Calls strategy.on_stop and cancels open orders.
  void stop();

  /// Trips the kill switch: cancels orders and (optionally) flattens.
  /// Thread-safe: may be called from the web server.
  void request_kill(const std::string& reason);

  [[nodiscard]] bool halted() const;
  [[nodiscard]] bool strategy_failed() const { return strategy_failed_; }
  /// Thread-safe snapshot for the dashboard.
  [[nodiscard]] nlohmann::json snapshot(size_t max_curve_points = 500) const;
  /// Direct access for the backtester (engine thread only).
  [[nodiscard]] const Portfolio& portfolio() const { return portfolio_; }
  [[nodiscard]] std::vector<LogEntry> logs() const;
  [[nodiscard]] std::vector<Fill> fills() const;

  // ---- BrokerListener (any thread; enqueue only) ------------------------
  void on_bar(const Bar& bar, bool historical) override;
  void on_fill(const Fill& fill) override;
  void on_order_status(OrderId id, OrderStatus status, const std::string& reason) override;
  void on_position(const std::string& symbol, double quantity, double avg_price) override;
  void on_connection(bool connected, const std::string& message) override;
  void on_broker_message(int code, const std::string& message) override;

  // ---- StrategyContext (engine thread only) -----------------------------
  OrderId buy(const std::string& symbol, double qty) override;
  OrderId sell(const std::string& symbol, double qty) override;
  OrderId buy_limit(const std::string& symbol, double qty, double price) override;
  OrderId sell_limit(const std::string& symbol, double qty, double price) override;
  OrderId target_position(const std::string& symbol, double target_qty) override;
  bool cancel(OrderId id) override;
  void cancel_all(const std::string& symbol) override;
  [[nodiscard]] double position(const std::string& symbol) const override;
  [[nodiscard]] double avg_price(const std::string& symbol) const override;
  [[nodiscard]] double pending_quantity(const std::string& symbol) const override;
  [[nodiscard]] double cash() const override;
  [[nodiscard]] double equity() const override;
  [[nodiscard]] std::vector<Bar> history(const std::string& symbol, size_t n) const override;
  [[nodiscard]] const std::vector<std::string>& symbols() const override { return opts_.symbols; }
  [[nodiscard]] Timestamp now() const override;
  [[nodiscard]] double param(const std::string& name, double default_value) const override;
  [[nodiscard]] std::string param_str(const std::string& name, const std::string& def) const override;
  void log(const std::string& message) override;

 private:
  struct BarEvent { Bar bar; bool historical; };
  struct StatusEvent { OrderId id; OrderStatus status; std::string reason; };
  struct PositionEvent { std::string symbol; double qty; double avg; };
  struct ConnectionEvent { bool connected; std::string message; };
  struct MessageEvent { int code; std::string message; };
  using Event = std::variant<BarEvent, Fill, StatusEvent, PositionEvent, ConnectionEvent, MessageEvent>;

  void enqueue(Event e);
  void handle(BarEvent& e);
  void handle(Fill& f);
  void handle(StatusEvent& e);
  void handle(PositionEvent& e);
  void handle(ConnectionEvent& e);
  void handle(MessageEvent& e);

  OrderId place(const std::string& symbol, Side side, OrderType type, double qty, double limit,
                bool bypass_risk);
  void kill_locked(const std::string& reason);
  void add_log(const std::string& level, const std::string& msg);
  template <typename Fn>
  void guard_strategy(const char* hook, Fn&& fn);

  EngineOptions opts_;
  Broker& broker_;
  StrategyPtr strategy_;
  Portfolio portfolio_;
  RiskManager risk_;

  // event queue (any thread)
  mutable std::mutex queue_mtx_;
  std::condition_variable queue_cv_;
  std::deque<Event> queue_;
  std::string pending_kill_;  // set by request_kill, applied on the engine thread

  // engine state, guarded by state_mtx_ for readers on other threads
  mutable std::recursive_mutex state_mtx_;
  std::map<std::string, std::deque<Bar>> history_;
  std::map<std::string, double> last_price_;
  std::map<OrderId, Order> orders_;
  std::set<OrderId> open_orders_;
  std::deque<LogEntry> logs_;
  std::deque<Fill> fills_;
  OrderId next_order_id_ = 1;
  Timestamp sim_time_ = 0;
  bool started_ = false;
  bool connected_ = false;
  bool strategy_failed_ = false;
  bool kill_executed_ = false;
  std::string last_broker_message_;
};

}  // namespace algo
