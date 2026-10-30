// Python strategies run inside an embedded CPython interpreter.
//
// The user file is executed in its own namespace; the engine picks the
// subclass of algo.Strategy it defines (or the class bound to STRATEGY).
// The GIL is released while the engine runs and re-acquired around every
// call into Python, so web-server threads never block on Python.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include "algo/strategy/strategy_loader.hpp"

namespace py = pybind11;

namespace algo {
namespace {

const char* kPythonPrelude = R"PY(
class Strategy:
    """Base class for Python strategies. Override the hooks you need.

    on_bar(ctx, bar) is required. Orders placed in on_bar are filled at the
    earliest on the next bar in backtests.
    """
    def on_start(self, ctx):
        pass
    def on_bar(self, ctx, bar):
        raise NotImplementedError("implement on_bar(self, ctx, bar)")
    def on_fill(self, ctx, fill):
        pass
    def on_order_rejected(self, ctx, order):
        pass
    def on_stop(self, ctx):
        pass
)PY";

}  // namespace
}  // namespace algo

PYBIND11_EMBEDDED_MODULE(algo, m) {
  using namespace algo;
  m.doc() = "IBKR algo engine strategy API";

  py::enum_<Side>(m, "Side").value("BUY", Side::Buy).value("SELL", Side::Sell);
  py::enum_<OrderType>(m, "OrderType").value("MARKET", OrderType::Market).value("LIMIT", OrderType::Limit);

  py::class_<Bar>(m, "Bar")
      .def(py::init<>())
      .def_readonly("symbol", &Bar::symbol)
      .def_readonly("time", &Bar::time)
      .def_readonly("open", &Bar::open)
      .def_readonly("high", &Bar::high)
      .def_readonly("low", &Bar::low)
      .def_readonly("close", &Bar::close)
      .def_readonly("volume", &Bar::volume)
      .def("__repr__", [](const Bar& b) {
        std::ostringstream os;
        os << "Bar(" << b.symbol << " t=" << b.time << " o=" << b.open << " h=" << b.high
           << " l=" << b.low << " c=" << b.close << " v=" << b.volume << ")";
        return os.str();
      });

  py::class_<Fill>(m, "Fill")
      .def_readonly("order_id", &Fill::order_id)
      .def_readonly("symbol", &Fill::symbol)
      .def_readonly("side", &Fill::side)
      .def_readonly("quantity", &Fill::quantity)
      .def_readonly("price", &Fill::price)
      .def_readonly("commission", &Fill::commission)
      .def_readonly("time", &Fill::time);

  py::class_<Order>(m, "Order")
      .def_readonly("id", &Order::id)
      .def_readonly("symbol", &Order::symbol)
      .def_readonly("side", &Order::side)
      .def_readonly("type", &Order::type)
      .def_readonly("quantity", &Order::quantity)
      .def_readonly("limit_price", &Order::limit_price)
      .def_readonly("reason", &Order::reason);

  py::class_<StrategyContext, std::unique_ptr<StrategyContext, py::nodelete>>(m, "Context")
      .def("buy", &StrategyContext::buy, py::arg("symbol"), py::arg("qty"))
      .def("sell", &StrategyContext::sell, py::arg("symbol"), py::arg("qty"))
      .def("buy_limit", &StrategyContext::buy_limit, py::arg("symbol"), py::arg("qty"), py::arg("price"))
      .def("sell_limit", &StrategyContext::sell_limit, py::arg("symbol"), py::arg("qty"), py::arg("price"))
      .def("target_position", &StrategyContext::target_position, py::arg("symbol"), py::arg("qty"))
      .def("cancel", &StrategyContext::cancel, py::arg("order_id"))
      .def("cancel_all", &StrategyContext::cancel_all, py::arg("symbol") = "")
      .def("position", &StrategyContext::position, py::arg("symbol"))
      .def("avg_price", &StrategyContext::avg_price, py::arg("symbol"))
      .def("pending_quantity", &StrategyContext::pending_quantity, py::arg("symbol"))
      .def_property_readonly("cash", &StrategyContext::cash)
      .def_property_readonly("equity", &StrategyContext::equity)
      .def_property_readonly("symbols", &StrategyContext::symbols)
      .def_property_readonly("now", &StrategyContext::now)
      .def("history", &StrategyContext::history, py::arg("symbol"), py::arg("n"))
      .def(
          "closes",
          [](const StrategyContext& c, const std::string& s, size_t n) {
            std::vector<double> out;
            for (const auto& b : c.history(s, n)) out.push_back(b.close);
            return out;
          },
          py::arg("symbol"), py::arg("n"), "Last n closes, oldest first")
      .def(
          "param",
          [](const StrategyContext& c, const std::string& name, py::object def) -> py::object {
            if (py::isinstance<py::str>(def)) return py::str(c.param_str(name, def.cast<std::string>()));
            const double v = c.param(name, def.is_none() ? 0.0 : def.cast<double>());
            if (py::isinstance<py::int_>(def) && !py::isinstance<py::bool_>(def)) {
              return py::int_(static_cast<long long>(v));
            }
            return py::float_(v);
          },
          py::arg("name"), py::arg("default") = py::none())
      .def("log", &StrategyContext::log, py::arg("message"));

  py::exec(algo::kPythonPrelude, m.attr("__dict__"));
}

namespace algo {
namespace {

/// Starts the interpreter once per process and releases the GIL.
/// The interpreter is intentionally never finalized: tearing it down with
/// live extension objects (numpy...) at exit is a common source of crashes.
void ensure_interpreter() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!Py_IsInitialized()) {
      py::initialize_interpreter();
      PyEval_SaveThread();  // release the GIL held by the initializing thread
    }
  });
}

std::string read_text(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw StrategyLoadError("cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

class PythonStrategy final : public Strategy {
 public:
  PythonStrategy(py::object instance, std::string name)
      : instance_(std::move(instance)), name_(std::move(name)) {}

  ~PythonStrategy() override {
    py::gil_scoped_acquire gil;
    ctx_obj_ = py::object();
    instance_ = py::object();
  }

  void on_start(StrategyContext& ctx) override { call("on_start", ctx); }
  void on_bar(StrategyContext& ctx, const Bar& bar) override { call("on_bar", ctx, bar); }
  void on_fill(StrategyContext& ctx, const Fill& fill) override { call("on_fill", ctx, fill); }
  void on_order_rejected(StrategyContext& ctx, const Order& o) override {
    call("on_order_rejected", ctx, o);
  }
  void on_stop(StrategyContext& ctx) override { call("on_stop", ctx); }

 private:
  template <typename... Args>
  void call(const char* hook, StrategyContext& ctx, const Args&... args) {
    py::gil_scoped_acquire gil;
    try {
      if (ctx_ptr_ != &ctx) {
        ctx_obj_ = py::cast(&ctx, py::return_value_policy::reference);
        ctx_ptr_ = &ctx;
      }
      instance_.attr(hook)(ctx_obj_, args...);
    } catch (py::error_already_set& e) {
      throw std::runtime_error(name_ + "." + hook + ": " + e.what());
    }
  }

  py::object instance_;
  py::object ctx_obj_;
  StrategyContext* ctx_ptr_ = nullptr;
  std::string name_;
};

}  // namespace

bool python_strategies_supported() { return true; }

StrategyPtr load_python_strategy(const std::string& py_path) {
  ensure_interpreter();
  const std::string code = read_text(py_path);
  static std::atomic<int> counter{0};
  py::gil_scoped_acquire gil;
  try {
    py::module_ algo_mod = py::module_::import("algo");
    py::object base = algo_mod.attr("Strategy");

    // Make sibling modules importable (helpers next to the strategy file).
    const std::string dir = std::filesystem::absolute(py_path).parent_path().string();
    py::list sys_path = py::module_::import("sys").attr("path");
    if (!sys_path.contains(dir)) sys_path.append(dir);

    py::dict ns;
    ns["__builtins__"] = py::module_::import("builtins");
    ns["__name__"] = "algo_user_strategy_" + std::to_string(counter++);
    ns["__file__"] = py_path;
    py::exec(py::str(code), ns);

    py::object cls;
    if (ns.contains("STRATEGY")) {
      cls = ns["STRATEGY"];
    } else {
      py::object builtins = py::module_::import("builtins");
      for (auto item : ns) {
        py::object v = py::reinterpret_borrow<py::object>(item.second);
        if (py::isinstance<py::type>(v) && !v.is(base) &&
            builtins.attr("issubclass")(v, base).cast<bool>() &&
            // only classes defined in this file, not imported helpers
            py::str(v.attr("__module__")).cast<std::string>() == ns["__name__"].cast<std::string>()) {
          cls = v;
        }
      }
    }
    if (!cls) {
      throw StrategyLoadError(py_path + ": no strategy found. Define `class MyStrategy(algo.Strategy)` "
                              "(import algo) or set STRATEGY = YourClass.");
    }
    py::object instance = cls();
    const std::string name = py::str(cls.attr("__name__")).cast<std::string>();
    auto* s = new PythonStrategy(std::move(instance), name);
    return StrategyPtr(s, [](Strategy* p) { delete p; });
  } catch (py::error_already_set& e) {
    throw StrategyLoadError(std::string("Python error while loading ") + py_path + ":\n" + e.what());
  }
}

}  // namespace algo
