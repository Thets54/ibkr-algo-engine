// Loading user strategies: C++ compiled on the fly, Python embedded.
#include <gtest/gtest.h>

#include "algo/engine/backtester.hpp"
#include "algo/strategy/strategy_loader.hpp"
#include "test_helpers.hpp"

using namespace algo;
using test::TempDir;

namespace {
EngineOptions opts() {
  EngineOptions o;
  o.symbols = {"A"};
  o.risk.max_position_notional = 1e9;
  o.risk.max_order_notional = 1e9;
  o.risk.max_gross_exposure = 1e9;
  o.risk.max_position_qty = 1e9;
  return o;
}

PerformanceMetrics run(StrategyPtr s, int n = 120, nlohmann::json params = nlohmann::json::object()) {
  auto o = opts();
  o.params = std::move(params);
  Backtester bt(o, CommissionConfig{}, std::move(s));
  auto r = bt.run(test::random_walk("A", n));
  EXPECT_TRUE(r.completed);
  return r.metrics;
}
}  // namespace

TEST(StrategyKind, DetectsByExtension) {
  EXPECT_EQ(strategy_kind("x.py"), StrategyKind::Python);
  EXPECT_EQ(strategy_kind("x.CPP"), StrategyKind::CppSource);
  EXPECT_EQ(strategy_kind("lib.so"), StrategyKind::SharedLibrary);
  EXPECT_EQ(strategy_kind("x.txt"), StrategyKind::Unsupported);
}

TEST(CppStrategy, ExampleCompilesLoadsAndTrades) {
  TempDir cache;
  StrategyLoader loader(cache.str());
  auto m = run(loader.load(test::source_path("examples/strategies/donchian_breakout.cpp")), 300,
               {{"entry", 10}, {"exit", 5}});
  EXPECT_GT(m.closed_trades, 0);
}

TEST(CppStrategy, CompiledLibraryIsCached) {
  TempDir cache;
  StrategyLoader loader(cache.str());
  const auto src = test::source_path("examples/strategies/donchian_breakout.cpp");
  const auto lib1 = loader.compile_cpp(src);
  const auto t1 = std::filesystem::last_write_time(lib1);
  const auto lib2 = loader.compile_cpp(src);
  EXPECT_EQ(lib1, lib2);
  EXPECT_EQ(t1, std::filesystem::last_write_time(lib2));
}

TEST(CppStrategy, CompileErrorIsReadable) {
  TempDir d;
  auto p = d.write("bad.cpp", "#include <algo/strategy/strategy.hpp>\nint main( {\n");
  StrategyLoader loader((d.path() / "cache").string());
  try {
    loader.load(p);
    FAIL() << "expected a compile error";
  } catch (const StrategyLoadError& e) {
    EXPECT_NE(std::string(e.what()).find("compilation of bad.cpp failed"), std::string::npos) << e.what();
  }
}

TEST(CppStrategy, MissingExportIsExplained) {
  TempDir d;
  auto p = d.write("noexport.cpp",
                   "#include <algo/strategy/strategy.hpp>\n"
                   "struct S : algo::Strategy { void on_bar(algo::StrategyContext&, const algo::Bar&) override {} };\n");
  StrategyLoader loader((d.path() / "cache").string());
  try {
    loader.load(p);
    FAIL();
  } catch (const StrategyLoadError& e) {
    EXPECT_NE(std::string(e.what()).find("ALGO_EXPORT_STRATEGY"), std::string::npos) << e.what();
  }
}

TEST(Loader, RejectsUnsupportedAndMissingFiles) {
  TempDir d;
  StrategyLoader loader(d.str());
  EXPECT_THROW(loader.load(d.write("x.txt", "hi")), StrategyLoadError);
  EXPECT_THROW(loader.load((d.path() / "nope.py").string()), StrategyLoadError);
}

#if defined(ALGO_WITH_PYTHON)
TEST(PythonStrategy, ExamplesLoadAndTrade) {
  StrategyLoader loader;
  auto m1 = run(loader.load(test::source_path("examples/strategies/sma_crossover.py")), 200,
                {{"fast", 5}, {"slow", 20}});
  EXPECT_GT(m1.closed_trades, 0);
  auto m2 = run(loader.load(test::source_path("examples/strategies/zscore_mean_reversion.py")), 300);
  EXPECT_GE(m2.closed_trades, 0);
}

TEST(PythonStrategy, ContextApiRoundTrip) {
  TempDir d;
  auto p = d.write("probe.py", R"(
import algo
class Probe(algo.Strategy):
    def on_start(self, ctx):
        assert ctx.symbols == ["A"]
        assert ctx.param("n", 3) == 3 and isinstance(ctx.param("n", 3), int)
        assert ctx.param("label", "x") == "x"
        self.bars = 0
    def on_bar(self, ctx, bar):
        self.bars += 1
        assert isinstance(bar, algo.Bar) and bar.symbol == "A"
        assert len(ctx.closes("A", 3)) == min(self.bars, 3)
        if self.bars == 1:
            assert ctx.buy("A", 2) > 0
            assert ctx.pending_quantity("A") == 2
        if self.bars == 3:
            assert ctx.position("A") == 2, ctx.position("A")
            ctx.log("probe ok")
    def on_fill(self, ctx, fill):
        assert fill.side == algo.Side.BUY and fill.quantity == 2
)");
  StrategyLoader loader;
  auto o = opts();
  Backtester bt(o, CommissionConfig{}, loader.load(p));
  auto r = bt.run(test::random_walk("A", 5));
  ASSERT_TRUE(r.completed) << bt.engine().snapshot()["logs"].dump(2);
  bool ok = false;
  for (const auto& l : bt.engine().logs()) ok |= l.message == "probe ok";
  EXPECT_TRUE(ok);
}

TEST(PythonStrategy, PythonExceptionStopsStrategyWithTraceback) {
  TempDir d;
  auto p = d.write("crash.py", "import algo\nclass C(algo.Strategy):\n    def on_bar(self, ctx, bar):\n        1/0\n");
  StrategyLoader loader;
  Backtester bt(opts(), CommissionConfig{}, loader.load(p));
  auto r = bt.run(test::random_walk("A", 5));
  EXPECT_FALSE(r.completed);
  bool found = false;
  for (const auto& l : bt.engine().logs()) found |= l.message.find("ZeroDivisionError") != std::string::npos;
  EXPECT_TRUE(found);
}

TEST(PythonStrategy, SyntaxErrorAndMissingClassAreLoadErrors) {
  TempDir d;
  StrategyLoader loader;
  EXPECT_THROW(loader.load(d.write("syntax.py", "def (:\n")), StrategyLoadError);
  try {
    loader.load(d.write("empty.py", "x = 1\n"));
    FAIL();
  } catch (const StrategyLoadError& e) {
    EXPECT_NE(std::string(e.what()).find("no strategy found"), std::string::npos);
  }
}

TEST(PythonStrategy, ExplicitStrategyVariableWins) {
  TempDir d;
  auto p = d.write("pick.py", R"(
import algo
class A(algo.Strategy):
    def on_bar(self, ctx, bar): raise RuntimeError("wrong class")
class B(algo.Strategy):
    def on_bar(self, ctx, bar): pass
STRATEGY = B
class C(algo.Strategy):
    def on_bar(self, ctx, bar): raise RuntimeError("wrong class")
)");
  StrategyLoader loader;
  Backtester bt(opts(), CommissionConfig{}, loader.load(p));
  EXPECT_TRUE(bt.run(test::random_walk("A", 3)).completed);
}

#endif
