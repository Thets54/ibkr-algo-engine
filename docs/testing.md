# Testing strategy

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j
cd build && ctest --output-on-failure -j
```

73 tests, about 3 seconds. Each GoogleTest case runs in its own process (`gtest_discover_tests`), so the embedded Python interpreter and temporary folders never leak between tests.

## Pyramid

| Level | Files | What it proves |
|---|---|---|
| Unit | `test_portfolio`, `test_risk`, `test_metrics`, `test_data`, `test_sim_broker` | Accounting (average price, partial close, flips, shorts), every risk rule and the kill switch, Sharpe and drawdown on hand-computed curves, CSV parsing edge cases, bar aggregation, fill rules, commission schedule, IBKR bar size and duration strings |
| Integration | `test_engine`, `test_strategy_loader` | No look-ahead (fills at next open, history never contains future bars), multi-symbol synchronisation, idempotent `target_position`, determinism, risk rejections, kill switch flattening, strategy exceptions; C++ compile, cache, readable compile errors, missing export; Python examples, the full Python context API, tracebacks, load errors |
| System | `test_session`, `test_web_api` | Background runs, one-run-at-a-time, error reporting, live gating; a threaded fake broker exercising the paper/live code path (fills on another thread, stop, kill switch); HTTP upload (JSON and multipart), templates compile, path traversal refused, CSRF header and Host checks, a backtest driven entirely through the API |

## What is not covered automatically

- **The real IBKR adapter.** The TWS SDK cannot be redistributed, so CI builds without it. `FakeLiveBroker` reproduces its threading contract (callbacks on a foreign thread, asynchronous fills) to test everything above the adapter. Validate the adapter itself with a paper account: run `examples/strategies/sma_crossover.py` on one liquid stock with 1 minute bars for a session and compare the Orders and Fills tabs with TWS.
- **The dashboard JavaScript.** It is exercised manually and by the HTTP tests for every endpoint it calls. A Playwright smoke test would be the next step.

## Sanitizers

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan -j && cd build-asan
ASAN_OPTIONS=detect_leaks=0 ctest -E "CppStrategy|CppTemplate"
```

The C++ plugin tests are excluded because on-the-fly plugins are compiled without sanitizers. Leak detection is off because the embedded interpreter is intentionally never finalized.

## Adding tests for a new strategy

Strategy research belongs in your own notebooks, but two cheap checks catch most engine-level mistakes:

1. Run it twice on the same data: results must be identical (no hidden randomness or wall-clock use).
2. Shift the data by one bar and confirm performance changes in the expected direction (a strategy that does as well with future data removed is fine; one that collapses may have been relying on it).
