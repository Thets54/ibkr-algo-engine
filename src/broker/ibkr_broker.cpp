// Interactive Brokers adapter (TWS API C++ client, socket to TWS / IB Gateway).
//
// Threading: the TWS EReader runs on its own thread and calls the EWrapper
// methods below; they translate messages and forward them to the engine's
// BrokerListener, which only enqueues. All EClientSocket requests are
// serialized by `client_mtx_`.

#include "algo/broker/ibkr_broker.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "algo/data/bar_aggregator.hpp"
#include "algo/data/csv_feed.hpp"

// TWS API headers (from TWS_API_DIR)
#include "Contract.h"
#include "Decimal.h"
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "Execution.h"
#include "Order.h"
#include "TagValue.h"
#include "bar.h"
#if defined(TWS_CANCEL_WITH_ORDERCANCEL)
#include "OrderCancel.h"
#endif

namespace algo {

namespace {

bool is_forex(const std::string& s) {
  return s.size() == 7 && s[3] == '.';  // EUR.USD
}

Contract make_contract(const IbkrConfig& cfg, const std::string& symbol) {
  Contract c;
  if (is_forex(symbol)) {
    c.symbol = symbol.substr(0, 3);
    c.currency = symbol.substr(4, 3);
    c.secType = "CASH";
    c.exchange = "IDEALPRO";
  } else {
    c.symbol = symbol;
    c.secType = "STK";
    c.exchange = cfg.exchange;
    c.currency = cfg.currency;
  }
  return c;
}

std::string symbol_of(const Contract& c) {
  if (c.secType == "CASH") return c.symbol + "." + c.currency;
  return c.symbol;
}

double to_double(Decimal d) { return DecimalFunctions::decimalToDouble(d); }

}  // namespace

class IbkrBroker::Impl final : public DefaultEWrapper {
 public:
  Impl(IbkrConfig cfg, bool expect_paper)
      : cfg_(std::move(cfg)), expect_paper_(expect_paper), signal_(2000), client_(this, &signal_) {}

  ~Impl() override { disconnect(); }

  // ------------------------------------------------------------ control
  bool connect(std::string& error) {
    disconnect();
    {
      std::lock_guard lock(client_mtx_);
      if (!client_.eConnect(cfg_.host.c_str(), cfg_.port, cfg_.client_id, false)) {
        error = "eConnect to " + cfg_.host + ":" + std::to_string(cfg_.port) +
                " failed. Is TWS / IB Gateway running with the API enabled on this port?";
        return false;
      }
    }
    next_valid_id_ = -1;
    accounts_.clear();
    reader_ = std::make_unique<EReader>(&client_, &signal_);
    reader_->start();
    running_ = true;
    reader_thread_ = std::thread([this] {
      while (running_ && client_.isConnected()) {
        signal_.waitForSignal();
        reader_->processMsgs();
      }
    });

    // nextValidId and managedAccounts are sent right after the handshake.
    std::unique_lock lk(state_mtx_);
    const bool ok = cv_.wait_for(lk, std::chrono::seconds(10),
                                 [&] { return next_valid_id_ >= 0 && !accounts_.empty(); });
    if (!ok) {
      lk.unlock();
      error = "connected but no handshake from TWS within 10s (check clientId " +
              std::to_string(cfg_.client_id) + " is not already in use)";
      disconnect();
      return false;
    }
    if (expect_paper_) {
      for (const auto& acc : accounts_) {
        if (acc.empty() || acc[0] != 'D') {
          lk.unlock();
          error = "paper mode refused: account " + acc +
                  " is not a paper account. Connect to the paper TWS/Gateway port instead.";
          disconnect();
          return false;
        }
      }
    }
    lk.unlock();
    {
      std::lock_guard lock(client_mtx_);
      client_.reqMarketDataType(cfg_.market_data_type);
    }
    if (auto* l = listener()) l->on_connection(true, "connected to IBKR " + cfg_.host + ":" + std::to_string(cfg_.port));
    return true;
  }

  void disconnect() {
    running_ = false;
    {
      std::lock_guard lock(client_mtx_);
      if (client_.isConnected()) client_.eDisconnect();
    }
    signal_.issueSignal();
    if (reader_thread_.joinable()) reader_thread_.join();
    reader_.reset();
  }

  bool is_connected() const { return client_.isConnected(); }

  void submit(const algo::Order& o) {
    ::Order t;
    t.action = o.side == Side::Buy ? "BUY" : "SELL";
    t.totalQuantity = DecimalFunctions::doubleToDecimal(o.quantity);
    t.orderType = o.type == OrderType::Market ? "MKT" : "LMT";
    if (o.type == OrderType::Limit) t.lmtPrice = o.limit_price;
    t.tif = "DAY";
    t.transmit = true;

    long tws_id = 0;
    {
      std::lock_guard lock(state_mtx_);
      if (next_valid_id_ < 0) {
        if (auto* l = listener()) l->on_order_status(o.id, OrderStatus::Rejected, "not connected");
        return;
      }
      tws_id = next_valid_id_++;
      tws_to_engine_[tws_id] = o.id;
      engine_to_tws_[o.id] = tws_id;
      order_symbol_[o.id] = o.symbol;
    }
    {
      std::lock_guard lock(client_mtx_);
      client_.placeOrder(tws_id, make_contract(cfg_, o.symbol), t);
    }
    if (auto* l = listener()) l->on_order_status(o.id, OrderStatus::Submitted, "");
  }

  void cancel(OrderId id) {
    long tws_id = -1;
    {
      std::lock_guard lock(state_mtx_);
      auto it = engine_to_tws_.find(id);
      if (it == engine_to_tws_.end()) return;
      tws_id = it->second;
    }
    std::lock_guard lock(client_mtx_);
#if defined(TWS_CANCEL_WITH_ORDERCANCEL)
    OrderCancel oc;
    client_.cancelOrder(tws_id, oc);
#else
    client_.cancelOrder(tws_id, "");
#endif
  }

  void subscribe_bars(const std::vector<std::string>& symbols, int bar_seconds) {
    if (bar_seconds % 5 != 0) throw std::invalid_argument("live bar size must be a multiple of 5 seconds");
    {
      std::lock_guard lock(state_mtx_);
      aggregator_ = std::make_unique<BarAggregator>(bar_seconds, [this](const algo::Bar& b) {
        if (auto* l = listener()) l->on_bar(b, false);
      });
    }
    for (const auto& s : symbols) {
      const int req = next_req_id_++;
      {
        std::lock_guard lock(state_mtx_);
        rt_req_symbol_[req] = s;
      }
      std::lock_guard lock(client_mtx_);
      client_.reqRealTimeBars(req, make_contract(cfg_, s), 5, is_forex(s) ? "MIDPOINT" : "TRADES",
                              false, TagValueListSPtr());
    }
  }

  void request_history(const std::string& symbol, int count, int bar_seconds) {
    const int req = next_req_id_++;
    {
      std::lock_guard lock(state_mtx_);
      hist_req_[req] = {symbol, bar_seconds};
    }
    std::lock_guard lock(client_mtx_);
    client_.reqHistoricalData(req, make_contract(cfg_, symbol), "", ibkr_duration(count, bar_seconds),
                              ibkr_bar_size(bar_seconds), is_forex(symbol) ? "MIDPOINT" : "TRADES",
                              /*useRTH=*/1, /*formatDate=*/2, /*keepUpToDate=*/false, TagValueListSPtr());
  }

  void request_positions() {
    std::lock_guard lock(client_mtx_);
    client_.reqPositions();
  }

  void set_listener(BrokerListener* l) { listener_.store(l); }
  BrokerListener* listener() const { return listener_.load(); }

  // ------------------------------------------------------------ EWrapper
  void nextValidId(OrderId orderId) override {
    {
      std::lock_guard lock(state_mtx_);
      if (orderId > next_valid_id_) next_valid_id_ = orderId;
    }
    cv_.notify_all();
  }

  void managedAccounts(const std::string& accountsList) override {
    {
      std::lock_guard lock(state_mtx_);
      accounts_.clear();
      size_t start = 0;
      while (start <= accountsList.size()) {
        size_t end = accountsList.find(',', start);
        if (end == std::string::npos) end = accountsList.size();
        if (end > start) accounts_.push_back(accountsList.substr(start, end - start));
        start = end + 1;
      }
    }
    cv_.notify_all();
  }

  void realtimeBar(TickerId reqId, long time, double open, double high, double low, double close,
                   Decimal volume, Decimal /*wap*/, int /*count*/) override {
    std::lock_guard lock(state_mtx_);
    auto it = rt_req_symbol_.find(static_cast<int>(reqId));
    if (it == rt_req_symbol_.end() || !aggregator_) return;
    algo::Bar b;
    b.symbol = it->second;
    b.time = time;
    b.open = open;
    b.high = high;
    b.low = low;
    b.close = close;
    const double v = to_double(volume);
    b.volume = v > 0 ? v : 0.0;  // IBKR sends -1 when volume is unavailable
    aggregator_->add(b);
  }

  void historicalData(TickerId reqId, const ::Bar& bar) override {
    std::string symbol;
    int bar_seconds = 0;
    {
      std::lock_guard lock(state_mtx_);
      auto it = hist_req_.find(static_cast<int>(reqId));
      if (it == hist_req_.end()) return;
      symbol = it->second.first;
      bar_seconds = it->second.second;
    }
    algo::Bar b;
    b.symbol = symbol;
    try {
      b.time = parse_timestamp(bar.time.substr(0, bar.time.find(' ')));
    } catch (...) {
      return;
    }
    // Skip the bar still being built: it would leak a partial bar into history.
    if (b.time + bar_seconds > static_cast<Timestamp>(std::time(nullptr))) return;
    b.open = bar.open;
    b.high = bar.high;
    b.low = bar.low;
    b.close = bar.close;
    const double v = to_double(bar.volume);
    b.volume = v > 0 ? v : 0.0;
    if (auto* l = listener()) l->on_bar(b, true);
  }

  void historicalDataEnd(int reqId, const std::string&, const std::string&) override {
    std::string symbol;
    {
      std::lock_guard lock(state_mtx_);
      auto it = hist_req_.find(reqId);
      if (it == hist_req_.end()) return;
      symbol = it->second.first;
      hist_req_.erase(it);
    }
    if (auto* l = listener()) l->on_broker_message(0, "history loaded for " + symbol);
  }

  void execDetails(int /*reqId*/, const Contract& contract, const Execution& e) override {
    Fill f;
    {
      std::lock_guard lock(state_mtx_);
      if (!seen_exec_ids_.insert(e.execId).second) return;  // replayed after reconnect
      auto it = tws_to_engine_.find(e.orderId);
      f.order_id = it == tws_to_engine_.end() ? -e.orderId : it->second;
      auto sym = it == tws_to_engine_.end() ? order_symbol_.end() : order_symbol_.find(it->second);
      f.symbol = sym == order_symbol_.end() ? symbol_of(contract) : sym->second;
    }
    f.side = e.side == "BOT" ? Side::Buy : Side::Sell;
    f.quantity = to_double(e.shares);
    f.price = e.price;
    f.commission = 0.0;  // commissions arrive separately, see docs/tech-debt.md
    f.time = static_cast<Timestamp>(std::time(nullptr));
    if (auto* l = listener(); l && f.quantity > 0) l->on_fill(f);
  }

  void position(const std::string& /*account*/, const Contract& contract, Decimal pos,
                double avgCost) override {
    // avgCost is per unit for stocks and forex (it includes commissions).
    if (auto* l = listener()) l->on_position(symbol_of(contract), to_double(pos), avgCost);
  }

  void positionEnd() override {
    // One snapshot is enough: later changes are tracked through fills.
    std::lock_guard lock(client_mtx_);
    client_.cancelPositions();
  }

#if defined(TWS_ERROR_WITH_TIME)
  void error(int id, time_t /*errorTime*/, int code, const std::string& msg,
             const std::string& /*advancedOrderRejectJson*/) override {
    handle_error(id, code, msg);
  }
#else
  void error(int id, int code, const std::string& msg,
             const std::string& /*advancedOrderRejectJson*/) override {
    handle_error(id, code, msg);
  }
#endif

  void connectionClosed() override {
    if (auto* l = listener()) l->on_connection(false, "IBKR connection closed");
  }

 private:
  void handle_error(int id, int code, const std::string& msg) {
    BrokerListener* l = listener();
    if (!l) return;
    if (id > 0) {
      OrderId engine_id = 0;
      {
        std::lock_guard lock(state_mtx_);
        auto it = tws_to_engine_.find(id);
        if (it != tws_to_engine_.end()) engine_id = it->second;
      }
      if (engine_id > 0) {
        if (code == 202) {
          l->on_order_status(engine_id, OrderStatus::Cancelled, msg);
        } else if (code == 201 || code == 203 || code == 103 || code == 104 || code == 105 ||
                   code == 110 || code == 135 || code == 161 || (code >= 380 && code <= 399)) {
          l->on_order_status(engine_id, OrderStatus::Rejected, msg);
        } else {
          l->on_broker_message(code, "order " + std::to_string(engine_id) + ": " + msg);
        }
        return;
      }
    }
    if (code == 1100) {
      l->on_connection(false, "IBKR connectivity lost (1100): " + msg);
    } else if (code == 1101 || code == 1102) {
      l->on_connection(true, "IBKR connectivity restored (" + std::to_string(code) + ")");
    } else {
      l->on_broker_message(code, msg);
    }
  }

  IbkrConfig cfg_;
  bool expect_paper_;
  EReaderOSSignal signal_;
  EClientSocket client_;
  std::unique_ptr<EReader> reader_;
  std::thread reader_thread_;
  std::atomic<bool> running_{false};
  std::atomic<BrokerListener*> listener_{nullptr};
  std::atomic<int> next_req_id_{1000};

  std::mutex client_mtx_;
  std::mutex state_mtx_;
  std::condition_variable cv_;
  long next_valid_id_ = -1;
  std::vector<std::string> accounts_;
  std::map<long, OrderId> tws_to_engine_;
  std::map<OrderId, long> engine_to_tws_;
  std::map<OrderId, std::string> order_symbol_;
  std::map<int, std::string> rt_req_symbol_;
  std::map<int, std::pair<std::string, int>> hist_req_;
  std::set<std::string> seen_exec_ids_;
  std::unique_ptr<BarAggregator> aggregator_;
};

IbkrBroker::IbkrBroker(IbkrConfig cfg, bool expect_paper)
    : impl_(std::make_unique<Impl>(std::move(cfg), expect_paper)) {}
IbkrBroker::~IbkrBroker() = default;
void IbkrBroker::set_listener(BrokerListener* l) { impl_->set_listener(l); }
bool IbkrBroker::connect(std::string& error) { return impl_->connect(error); }
void IbkrBroker::disconnect() { impl_->disconnect(); }
bool IbkrBroker::is_connected() const { return impl_->is_connected(); }
void IbkrBroker::submit(const Order& o) { impl_->submit(o); }
void IbkrBroker::cancel(OrderId id) { impl_->cancel(id); }
void IbkrBroker::subscribe_bars(const std::vector<std::string>& s, int b) { impl_->subscribe_bars(s, b); }
void IbkrBroker::request_history(const std::string& s, int c, int b) { impl_->request_history(s, c, b); }
void IbkrBroker::request_positions() { impl_->request_positions(); }

}  // namespace algo
