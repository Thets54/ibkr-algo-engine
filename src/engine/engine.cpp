#include "algo/engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <optional>

namespace algo {

using nlohmann::json;

namespace {
constexpr size_t kMaxLogs = 1'000;
constexpr size_t kMaxFills = 2'000;
constexpr double kQtyEps = 1e-9;

json order_json(const Order& o) {
  return json{{"id", o.id},
              {"symbol", o.symbol},
              {"side", to_string(o.side)},
              {"type", to_string(o.type)},
              {"quantity", o.quantity},
              {"limit_price", o.limit_price},
              {"status", to_string(o.status)},
              {"filled", o.filled_quantity},
              {"avg_fill_price", o.avg_fill_price},
              {"time", o.created_at},
              {"reason", o.reason}};
}
}  // namespace

Engine::Engine(EngineOptions options, Broker& broker, StrategyPtr strategy)
    : opts_(std::move(options)),
      broker_(broker),
      strategy_(std::move(strategy)),
      portfolio_(opts_.starting_cash),
      risk_(opts_.risk) {
  if (!strategy_) throw std::invalid_argument("engine needs a strategy");
  if (opts_.symbols.empty()) throw std::invalid_argument("engine needs at least one symbol");
  if (!opts_.params.is_object()) opts_.params = json::object();
  broker_.set_listener(this);
}

Engine::~Engine() {
  broker_.set_listener(nullptr);
  // Destroy the strategy before the rest of the engine: its destructor may
  // still reference the context (Python wrappers).
  strategy_.reset();
}

// ---------------------------------------------------------------- lifecycle

void Engine::start() {
  std::lock_guard lock(state_mtx_);
  if (started_) return;
  started_ = true;
  add_log("info", "strategy started on " + std::to_string(opts_.symbols.size()) + " symbol(s), broker=" +
                      broker_.name());
  guard_strategy("on_start", [&] { strategy_->on_start(*this); });
}

void Engine::stop() {
  std::lock_guard lock(state_mtx_);
  if (!started_) return;
  guard_strategy("on_stop", [&] { strategy_->on_stop(*this); });
  for (OrderId id : std::set<OrderId>(open_orders_)) broker_.cancel(id);
  started_ = false;
  add_log("info", "strategy stopped");
}

void Engine::request_kill(const std::string& reason) {
  {
    std::lock_guard lock(queue_mtx_);
    pending_kill_ = reason.empty() ? "manual kill switch" : reason;
  }
  queue_cv_.notify_all();
}

size_t Engine::drain() {
  std::deque<Event> batch;
  std::string kill;
  {
    std::lock_guard lock(queue_mtx_);
    batch.swap(queue_);
    kill.swap(pending_kill_);
  }
  std::lock_guard lock(state_mtx_);
  if (!kill.empty()) kill_locked(kill);
  for (auto& e : batch) {
    std::visit([this](auto& ev) { handle(ev); }, e);
  }
  return batch.size();
}

size_t Engine::wait_and_drain(int timeout_ms) {
  {
    std::unique_lock lock(queue_mtx_);
    queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                       [&] { return !queue_.empty() || !pending_kill_.empty(); });
  }
  return drain();
}

// ---------------------------------------------------------------- listener

void Engine::enqueue(Event e) {
  {
    std::lock_guard lock(queue_mtx_);
    queue_.push_back(std::move(e));
  }
  queue_cv_.notify_one();
}

void Engine::on_bar(const Bar& bar, bool historical) { enqueue(BarEvent{bar, historical}); }
void Engine::on_fill(const Fill& fill) { enqueue(fill); }
void Engine::on_order_status(OrderId id, OrderStatus st, const std::string& reason) {
  enqueue(StatusEvent{id, st, reason});
}
void Engine::on_position(const std::string& s, double q, double a) { enqueue(PositionEvent{s, q, a}); }
void Engine::on_connection(bool c, const std::string& m) { enqueue(ConnectionEvent{c, m}); }
void Engine::on_broker_message(int code, const std::string& m) { enqueue(MessageEvent{code, m}); }

// ---------------------------------------------------------------- handlers

void Engine::handle(BarEvent& e) {
  const Bar& bar = e.bar;
  auto& h = history_[bar.symbol];
  if (!h.empty() && bar.time <= h.back().time) return;  // duplicate or stale
  h.push_back(bar);
  while (h.size() > opts_.history_capacity) h.pop_front();
  last_price_[bar.symbol] = bar.close;
  portfolio_.mark(bar.symbol, bar.close);
  if (e.historical) return;  // warm-up only, never trade on it

  sim_time_ = std::max(sim_time_, bar.time);
  portfolio_.record_equity(bar.time);
  if (risk_.update_equity(portfolio_.equity(), now())) kill_locked(risk_.halt_reason());

  if (started_ && !risk_.halted() && !strategy_failed_) {
    guard_strategy("on_bar", [&] { strategy_->on_bar(*this, bar); });
  }
}

void Engine::handle(Fill& f) {
  auto it = orders_.find(f.order_id);
  if (it != orders_.end()) {
    Order& o = it->second;
    const double new_filled = o.filled_quantity + f.quantity;
    o.avg_fill_price = (o.avg_fill_price * o.filled_quantity + f.price * f.quantity) / new_filled;
    o.filled_quantity = new_filled;
    if (o.filled_quantity + kQtyEps >= o.quantity) {
      o.status = OrderStatus::Filled;
      open_orders_.erase(o.id);
    }
  } else {
    add_log("warn", "fill for unknown order " + std::to_string(f.order_id) + " (placed outside the engine?)");
  }
  portfolio_.apply_fill(f);
  last_price_.try_emplace(f.symbol, f.price);
  fills_.push_back(f);
  while (fills_.size() > kMaxFills) fills_.pop_front();
  add_log("info", std::string("fill ") + to_string(f.side) + " " + std::to_string(f.quantity) + " " +
                      f.symbol + " @ " + std::to_string(f.price));
  if (started_ && !strategy_failed_) guard_strategy("on_fill", [&] { strategy_->on_fill(*this, f); });
}

void Engine::handle(StatusEvent& e) {
  auto it = orders_.find(e.id);
  if (it == orders_.end()) return;
  Order& o = it->second;
  if (o.status == OrderStatus::Filled) return;  // late status after the fill
  o.status = e.status;
  if (!e.reason.empty()) o.reason = e.reason;
  if (e.status == OrderStatus::Cancelled || e.status == OrderStatus::Rejected) {
    open_orders_.erase(e.id);
  }
  if (e.status == OrderStatus::Rejected) {
    add_log("warn", "order " + std::to_string(e.id) + " rejected by broker: " + e.reason);
    if (started_ && !strategy_failed_) {
      guard_strategy("on_order_rejected", [&] { strategy_->on_order_rejected(*this, o); });
    }
  }
}

void Engine::handle(PositionEvent& e) {
  if (std::find(opts_.symbols.begin(), opts_.symbols.end(), e.symbol) == opts_.symbols.end()) {
    if (std::abs(e.qty) > kQtyEps) {
      add_log("info", "ignoring account position in " + e.symbol + " (not traded by this session)");
    }
    return;
  }
  portfolio_.set_position(e.symbol, e.qty, e.avg);
  add_log("info", "synced broker position " + e.symbol + " " + std::to_string(e.qty) + " @ " +
                      std::to_string(e.avg));
}

void Engine::handle(ConnectionEvent& e) {
  connected_ = e.connected;
  add_log(e.connected ? "info" : "error", e.message);
}

void Engine::handle(MessageEvent& e) {
  last_broker_message_ = e.message;
  add_log(e.code >= 2100 && e.code < 2200 ? "info" : "warn",
          "broker [" + std::to_string(e.code) + "] " + e.message);
}

// ---------------------------------------------------------------- orders

OrderId Engine::place(const std::string& symbol, Side side, OrderType type, double qty, double limit,
                      bool bypass_risk) {
  std::lock_guard lock(state_mtx_);
  Order o;
  o.id = next_order_id_++;
  o.symbol = symbol;
  o.side = side;
  o.type = type;
  o.quantity = qty;
  o.limit_price = limit;
  o.created_at = now();

  std::optional<std::string> reject;
  if (std::find(opts_.symbols.begin(), opts_.symbols.end(), symbol) == opts_.symbols.end()) {
    reject = "symbol " + symbol + " is not in this session's symbol list";
  } else if (!bypass_risk && strategy_failed_) {
    reject = "strategy halted after an error";
  } else if (!bypass_risk) {
    const auto lp = last_price_.find(symbol);
    reject = risk_.check(o, portfolio_, lp == last_price_.end() ? 0.0 : lp->second, now());
  }
  if (reject) {
    o.status = OrderStatus::Rejected;
    o.reason = *reject;
    orders_[o.id] = o;
    add_log("warn", "risk rejected " + std::string(to_string(side)) + " " + std::to_string(qty) + " " +
                        symbol + ": " + *reject);
    return 0;
  }
  risk_.record_submit(now());
  orders_[o.id] = o;
  open_orders_.insert(o.id);
  broker_.submit(o);
  return o.id;
}

OrderId Engine::buy(const std::string& s, double q) { return place(s, Side::Buy, OrderType::Market, q, 0, false); }
OrderId Engine::sell(const std::string& s, double q) { return place(s, Side::Sell, OrderType::Market, q, 0, false); }
OrderId Engine::buy_limit(const std::string& s, double q, double p) {
  return place(s, Side::Buy, OrderType::Limit, q, p, false);
}
OrderId Engine::sell_limit(const std::string& s, double q, double p) {
  return place(s, Side::Sell, OrderType::Limit, q, p, false);
}

OrderId Engine::target_position(const std::string& symbol, double target_qty) {
  const double diff = target_qty - position(symbol) - pending_quantity(symbol);
  if (std::abs(diff) < kQtyEps) return 0;
  return diff > 0 ? buy(symbol, diff) : sell(symbol, -diff);
}

bool Engine::cancel(OrderId id) {
  std::lock_guard lock(state_mtx_);
  if (!open_orders_.count(id)) return false;
  broker_.cancel(id);
  return true;
}

void Engine::cancel_all(const std::string& symbol) {
  std::lock_guard lock(state_mtx_);
  for (OrderId id : std::set<OrderId>(open_orders_)) {
    if (symbol.empty() || orders_[id].symbol == symbol) broker_.cancel(id);
  }
}

void Engine::kill_locked(const std::string& reason) {
  if (!risk_.halted()) risk_.trip(reason);
  if (kill_executed_) return;
  kill_executed_ = true;
  add_log("error", "KILL SWITCH: " + reason);
  for (OrderId id : std::set<OrderId>(open_orders_)) broker_.cancel(id);
  if (!opts_.risk.flatten_on_kill) return;
  for (const auto& [sym, pos] : portfolio_.positions()) {
    if (std::abs(pos.quantity) < kQtyEps) continue;
    const Side side = pos.quantity > 0 ? Side::Sell : Side::Buy;
    place(sym, side, OrderType::Market, std::abs(pos.quantity), 0, /*bypass_risk=*/true);
    add_log("error", "flattening " + sym + " " + std::to_string(pos.quantity));
  }
}

// ---------------------------------------------------------------- state

double Engine::position(const std::string& s) const {
  std::lock_guard lock(state_mtx_);
  return portfolio_.position_qty(s);
}

double Engine::avg_price(const std::string& s) const {
  std::lock_guard lock(state_mtx_);
  auto it = portfolio_.positions().find(s);
  return it == portfolio_.positions().end() ? 0.0 : it->second.avg_price;
}

double Engine::pending_quantity(const std::string& s) const {
  std::lock_guard lock(state_mtx_);
  double q = 0.0;
  for (OrderId id : open_orders_) {
    const Order& o = orders_.at(id);
    if (o.symbol == s) q += signed_qty(o.side, o.quantity - o.filled_quantity);
  }
  return q;
}

double Engine::cash() const {
  std::lock_guard lock(state_mtx_);
  return portfolio_.cash();
}

double Engine::equity() const {
  std::lock_guard lock(state_mtx_);
  return portfolio_.equity();
}

std::vector<Bar> Engine::history(const std::string& symbol, size_t n) const {
  std::lock_guard lock(state_mtx_);
  auto it = history_.find(symbol);
  if (it == history_.end()) return {};
  const auto& h = it->second;
  const size_t k = std::min(n, h.size());
  return std::vector<Bar>(h.end() - static_cast<std::ptrdiff_t>(k), h.end());
}

Timestamp Engine::now() const {
  if (opts_.wall_clock) return static_cast<Timestamp>(std::time(nullptr));
  return sim_time_;
}

double Engine::param(const std::string& name, double def) const {
  if (!opts_.params.contains(name)) return def;
  const auto& v = opts_.params.at(name);
  if (v.is_number()) return v.get<double>();
  if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
  if (v.is_string()) {
    try {
      return std::stod(v.get<std::string>());
    } catch (...) {
      return def;
    }
  }
  return def;
}

std::string Engine::param_str(const std::string& name, const std::string& def) const {
  if (!opts_.params.contains(name)) return def;
  const auto& v = opts_.params.at(name);
  return v.is_string() ? v.get<std::string>() : v.dump();
}

void Engine::log(const std::string& message) {
  std::lock_guard lock(state_mtx_);
  add_log("strategy", message);
}

void Engine::add_log(const std::string& level, const std::string& msg) {
  logs_.push_back({now(), level, msg});
  while (logs_.size() > kMaxLogs) logs_.pop_front();
}

template <typename Fn>
void Engine::guard_strategy(const char* hook, Fn&& fn) {
  try {
    fn();
  } catch (const std::exception& e) {
    strategy_failed_ = true;
    add_log("error", std::string("strategy error in ") + hook + ": " + e.what());
    kill_locked(std::string("strategy error in ") + hook);
  } catch (...) {
    strategy_failed_ = true;
    add_log("error", std::string("unknown strategy error in ") + hook);
    kill_locked(std::string("strategy error in ") + hook);
  }
}

bool Engine::halted() const {
  std::lock_guard lock(state_mtx_);
  return risk_.halted();
}

std::vector<LogEntry> Engine::logs() const {
  std::lock_guard lock(state_mtx_);
  return {logs_.begin(), logs_.end()};
}

std::vector<Fill> Engine::fills() const {
  std::lock_guard lock(state_mtx_);
  return {fills_.begin(), fills_.end()};
}

json Engine::snapshot(size_t max_curve_points) const {
  std::lock_guard lock(state_mtx_);
  json j;
  j["time"] = now();
  j["broker"] = broker_.name();
  j["connected"] = broker_.is_connected();
  j["halted"] = risk_.halted();
  j["halt_reason"] = risk_.halt_reason();
  j["strategy_failed"] = strategy_failed_;
  j["symbols"] = opts_.symbols;
  j["bar_seconds"] = opts_.bar_seconds;
  j["params"] = opts_.params;
  j["starting_cash"] = portfolio_.starting_cash();
  j["equity"] = portfolio_.equity();
  j["cash"] = portfolio_.cash();
  j["realized_pnl"] = portfolio_.realized_pnl();
  j["unrealized_pnl"] = portfolio_.unrealized_pnl();
  j["daily_pnl"] = risk_.daily_pnl();
  j["gross_exposure"] = portfolio_.gross_exposure();
  j["commission"] = portfolio_.total_commission();

  json pos = json::array();
  for (const auto& [s, p] : portfolio_.positions()) {
    if (std::abs(p.quantity) < kQtyEps && std::abs(p.realized_pnl) < kQtyEps) continue;
    pos.push_back({{"symbol", s},
                   {"quantity", p.quantity},
                   {"avg_price", p.avg_price},
                   {"last_price", p.last_price},
                   {"market_value", p.market_value()},
                   {"unrealized_pnl", p.unrealized_pnl()},
                   {"realized_pnl", p.realized_pnl}});
  }
  j["positions"] = pos;

  json open = json::array();
  for (OrderId id : open_orders_) open.push_back(order_json(orders_.at(id)));
  j["open_orders"] = open;

  json recent = json::array();
  size_t count = 0;
  for (auto it = orders_.rbegin(); it != orders_.rend() && count < 100; ++it, ++count) {
    recent.push_back(order_json(it->second));
  }
  j["orders"] = recent;

  json fl = json::array();
  const size_t f0 = fills_.size() > 100 ? fills_.size() - 100 : 0;
  for (size_t i = fills_.size(); i-- > f0;) {
    const auto& f = fills_[i];
    fl.push_back({{"order_id", f.order_id}, {"symbol", f.symbol}, {"side", to_string(f.side)},
                  {"quantity", f.quantity}, {"price", f.price}, {"commission", f.commission},
                  {"time", f.time}});
  }
  j["fills"] = fl;

  json lg = json::array();
  const size_t l0 = logs_.size() > 200 ? logs_.size() - 200 : 0;
  for (size_t i = logs_.size(); i-- > l0;) {
    lg.push_back({{"time", logs_[i].time}, {"level", logs_[i].level}, {"message", logs_[i].message}});
  }
  j["logs"] = lg;

  json px = json::object();
  for (const auto& [s, p] : last_price_) px[s] = p;
  j["last_prices"] = px;

  const auto& curve = portfolio_.equity_curve();
  json eq = json::array();
  const size_t step = curve.size() > max_curve_points && max_curve_points > 0
                          ? (curve.size() + max_curve_points - 1) / max_curve_points
                          : 1;
  for (size_t i = 0; i < curve.size(); i += step) eq.push_back({curve[i].time, curve[i].equity});
  if (!curve.empty() && (curve.size() - 1) % step != 0) eq.push_back({curve.back().time, curve.back().equity});
  j["equity_curve"] = eq;
  return j;
}

}  // namespace algo
