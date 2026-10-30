// SessionManager: strategy library on disk, backtest runs and a threaded
// "live" run against FakeLiveBroker (same code path as IBKR paper trading).
#include <gtest/gtest.h>

#include "algo/engine/session_manager.hpp"
#include "fake_live_broker.hpp"
#include "test_helpers.hpp"

using namespace algo;
using test::TempDir;

namespace {
AppConfig config_in(const TempDir& d) {
  AppConfig c;
  c.paths.strategies_dir = (d.path() / "strategies").string();
  c.paths.data_dir = (d.path() / "data").string();
  c.paths.cache_dir = (d.path() / "cache").string();
  return c;
}

const char* kSimplePy = R"(
import algo
class Flip(algo.Strategy):
    def on_bar(self, ctx, bar):
        ctx.target_position(bar.symbol, 10 if ctx.position(bar.symbol) == 0 else 0)
)";
}  // namespace

TEST(Filenames, OnlySafeNamesAreAccepted) {
  EXPECT_TRUE(is_valid_strategy_filename("my_strat-2.py"));
  EXPECT_TRUE(is_valid_strategy_filename("a.cpp"));
  EXPECT_FALSE(is_valid_strategy_filename("../evil.py"));
  EXPECT_FALSE(is_valid_strategy_filename("a/b.py"));
  EXPECT_FALSE(is_valid_strategy_filename(".hidden.py"));
  EXPECT_FALSE(is_valid_strategy_filename("run.sh"));
  EXPECT_FALSE(is_valid_strategy_filename("spaces in.py"));
  EXPECT_TRUE(is_valid_symbol("BRK B"));
  EXPECT_TRUE(is_valid_symbol("EUR.USD"));
  EXPECT_FALSE(is_valid_symbol("A;rm -rf"));
}

TEST(RunRequest, ParsesSymbolsStringAndMode) {
  auto r = run_request_from_json({{"strategy", "x.py"}, {"mode", "paper"}, {"symbols", " aapl, msft ,"}});
  EXPECT_EQ(r.mode, RunMode::Paper);
  EXPECT_EQ(r.symbols, (std::vector<std::string>{"AAPL", "MSFT"}));
  EXPECT_THROW(run_request_from_json({{"mode", "yolo"}}), std::invalid_argument);
}

TEST(Session, SaveListValidateDelete) {
  TempDir d;
  SessionManager sm(config_in(d));
  EXPECT_EQ(sm.save_strategy("flip.py", kSimplePy), "");
  EXPECT_NE(sm.save_strategy("broken.py", "def (:"), "");  // saved but invalid
  EXPECT_NE(sm.save_strategy("../x.py", kSimplePy), "");
  auto list = sm.list_strategies();
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list[0].name, "broken.py");
  EXPECT_EQ(list[1].kind, "python");
  EXPECT_EQ(sm.read_strategy("flip.py"), kSimplePy);
  EXPECT_TRUE(sm.delete_strategy("broken.py"));
  EXPECT_EQ(sm.list_strategies().size(), 1u);
}

TEST(Session, BacktestRunsInBackgroundAndReportsMetrics) {
  TempDir d;
  auto cfg = config_in(d);
  SessionManager sm(cfg);
  sm.save_strategy("flip.py", kSimplePy);
  d.write("data/A.csv", test::to_csv(test::random_walk("A", 60)));
  RunRequest r;
  r.strategy = "flip.py";
  r.symbols = {"A"};
  r.bar_seconds = 86'400;
  ASSERT_EQ(sm.start(r), "");
  EXPECT_NE(sm.start(r), "");  // only one run at a time
  sm.wait();
  auto st = sm.status();
  EXPECT_EQ(st["state"], "finished") << st.dump(2);
  EXPECT_GT(st["metrics"]["closed_trades"].get<int>(), 10);
  EXPECT_DOUBLE_EQ(st["progress"].get<double>(), 1.0);
}

TEST(Session, BacktestErrorsAreReported) {
  TempDir d;
  SessionManager sm(config_in(d));
  sm.save_strategy("flip.py", kSimplePy);
  RunRequest r;
  r.strategy = "flip.py";
  r.symbols = {"MISSING"};
  ASSERT_EQ(sm.start(r), "");
  sm.wait();
  auto st = sm.status();
  EXPECT_EQ(st["state"], "error");
  EXPECT_NE(st["error"].get<std::string>().find("MISSING.csv"), std::string::npos);
}

TEST(Session, ValidationBeforeStart) {
  TempDir d;
  SessionManager sm(config_in(d));
  sm.save_strategy("flip.py", kSimplePy);
  RunRequest r;
  r.strategy = "flip.py";
  EXPECT_NE(sm.start(r), "");  // no symbols
  r.symbols = {"A"};
  r.mode = RunMode::Live;
  EXPECT_NE(sm.start(r).find("allow_live"), std::string::npos);
  r.strategy = "nope.py";
  r.mode = RunMode::Backtest;
  EXPECT_NE(sm.start(r), "");
}

TEST(Session, WithoutIbkrSupportPaperModeFailsClearly) {
#if defined(ALGO_WITH_IBKR)
  GTEST_SKIP() << "built with IBKR";
#else
  TempDir d;
  SessionManager sm(config_in(d));
  sm.save_strategy("flip.py", kSimplePy);
  RunRequest r;
  r.strategy = "flip.py";
  r.symbols = {"A"};
  r.mode = RunMode::Paper;
  ASSERT_EQ(sm.start(r), "");
  sm.wait();
  EXPECT_NE(sm.status()["error"].get<std::string>().find("ALGO_WITH_IBKR"), std::string::npos);
#endif
}

TEST(Session, PaperRunTradesOnBrokerThreadAndStopsCleanly) {
  TempDir d;
  test::FakeLiveBroker* broker_ptr = nullptr;
  SessionManager sm(config_in(d), [&](const AppConfig&, RunMode) {
    auto b = std::make_unique<test::FakeLiveBroker>();
    broker_ptr = b.get();
    return b;
  });
  sm.save_strategy("flip.py", kSimplePy);
  RunRequest r;
  r.strategy = "flip.py";
  r.symbols = {"A"};
  r.mode = RunMode::Paper;
  ASSERT_EQ(sm.start(r), "");
  ASSERT_TRUE(test::eventually([&] {
    auto st = sm.status();
    return st.contains("engine") && st["engine"]["fills"].size() >= 6;
  })) << sm.status().dump(2);
  auto st = sm.status();
  EXPECT_EQ(st["state"], "running");
  EXPECT_TRUE(st["engine"]["connected"].get<bool>());
  sm.stop();
  sm.wait();
  st = sm.status();
  EXPECT_EQ(st["state"], "finished") << st.dump(2);
  // Unrelated account positions are ignored, warm-up history was received.
  bool ignored = false;
  for (const auto& l : st["engine"]["logs"]) {
    ignored |= l["message"].get<std::string>().find("ignoring account position in UNRELATED") != std::string::npos;
  }
  EXPECT_TRUE(ignored);
}

TEST(Session, KillSwitchFlattensLivePosition) {
  TempDir d;
  SessionManager sm(config_in(d), [](const AppConfig&, RunMode) { return std::make_unique<test::FakeLiveBroker>(); });
  sm.save_strategy("hold.py",
                   "import algo\nclass H(algo.Strategy):\n    def on_bar(self, ctx, bar):\n"
                   "        ctx.target_position(bar.symbol, 25)\n");
  RunRequest r;
  r.strategy = "hold.py";
  r.symbols = {"A"};
  r.mode = RunMode::Paper;
  ASSERT_EQ(sm.start(r), "");
  ASSERT_TRUE(test::eventually([&] {
    auto st = sm.status();
    return st.contains("engine") && !st["engine"]["positions"].empty() &&
           st["engine"]["positions"][0]["quantity"].get<double>() == 25.0;
  }));
  sm.kill("test kill");
  ASSERT_TRUE(test::eventually([&] {
    auto e = sm.status()["engine"];
    if (!e["halted"].get<bool>()) return false;
    for (const auto& p : e["positions"]) {
      if (p["quantity"].get<double>() != 0.0) return false;
    }
    return true;
  })) << sm.status().dump(2);
  sm.stop();
  sm.wait();
}
