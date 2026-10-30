# Architecture

## Goals

1. Let a user write a strategy once (Python or C++) and run it in backtest, paper and live without changes.
2. Talk to Interactive Brokers directly from C++ through the TWS API, no third-party middleware.
3. Be safe by default: every order goes through risk checks, live trading is opt-in, the dashboard is local only.
4. Be easy to use: a single binary that serves a dashboard, plus a CLI for headless runs.

Non-goals for v0.1: tick-level or HFT latency, options and futures chains, multi-strategy portfolios, multi-user hosting.

## Components

```
                      ┌──────────────────────── algoengine (one process) ───────────────────────┐
 browser ──HTTP──▶    │ WebServer (cpp-httplib)          CLI (main.cpp)                          │
                      │        │                              │                                  │
                      │        └──────────▶ SessionManager ◀──┘   strategy files on disk         │
                      │                     │  one active run, worker thread                     │
                      │                     ▼                                                    │
                      │   StrategyLoader ──▶ Strategy (C++ .so via dlopen | Python via pybind11) │
                      │                     │                                                    │
                      │                     ▼                                                    │
                      │                   Engine ──▶ RiskManager ──▶ Broker interface            │
                      │                   │  ▲          Portfolio      ├─ SimBroker (backtest)    │
                      │       event queue │  │ BrokerListener          └─ IbkrBroker ──TCP──▶ TWS / IB Gateway
                      └───────────────────┴──┴────────────────────────────────────────────────────┘
```

| Component | Responsibility |
|---|---|
| `Engine` | Single-threaded core. Drains broker events (bars, fills, order statuses, positions), updates `Portfolio`, calls strategy hooks, implements `StrategyContext`. |
| `RiskManager` | Pre-trade checks, rate limit, daily loss kill switch. |
| `Broker` | Interface implemented by `SimBroker` and `IbkrBroker`. Callbacks may arrive on any thread. |
| `Backtester` | Feeds CSV bars to `SimBroker` then the engine, one timestamp at a time. |
| `SessionManager` | Strategy library on disk, starts/stops runs on a worker thread, thread-safe status snapshots. |
| `StrategyLoader` | `.py` to embedded interpreter, `.cpp` compiled to a cached `.so`, `.so` loaded directly. |
| `WebServer` | JSON API and the dashboard (HTML embedded in the binary at build time). |

## Data flow of one bar

1. IBKR's reader thread receives a 5 second real-time bar. `IbkrBroker` aggregates it; when a bar of the requested size completes it calls `listener->on_bar()`.
2. `Engine::on_bar` only pushes the event into a mutex-protected queue and notifies a condition variable. Broker threads never run strategy code.
3. The run thread wakes in `wait_and_drain()`, marks positions to the close, records equity, updates the daily PnL (possibly tripping the kill switch), then calls `strategy.on_bar()`.
4. The strategy calls `ctx.target_position()`. The engine computes the delta (position plus pending orders), asks `RiskManager::check()`, assigns an order id and calls `Broker::submit()`.
5. Fills come back the same way (`on_fill` enqueued, applied to the portfolio, `strategy.on_fill()` called).

In a backtest, step 1 is the `Backtester` loop: for each timestamp it first lets `SimBroker` match orders from the previous step against the new bars, then delivers the bars. This ordering is what prevents look-ahead.

## Threading model

- **Run thread** (one per session): owns the engine, calls all strategy code. Strategies never need locks.
- **Broker threads**: IBKR's `EReader` thread. Only enqueues.
- **HTTP threads**: read `Engine::snapshot()` under the engine's state mutex, or post a kill request that the run thread applies on its next drain.
- **Python**: the interpreter is started once; the GIL is released and re-acquired only around calls into Python, so HTTP threads never wait on Python.

## Architecture decision records

### ADR-001: Engine in C++, strategies in Python or C++

**Status:** Accepted

**Context.** The user wants a C++ application but also wants to write strategies in Python, which is where most research code lives.

**Options.**

| | A. Embedded CPython (pybind11) | B. Python subprocess over IPC | C. C++ only |
|---|---|---|---|
| Complexity | Medium | High (protocol, lifecycle) | Low |
| Latency per call | ~µs | ~100 µs to ms | ns |
| Python libraries (numpy, pandas) | Yes | Yes | No |
| Crash isolation | No | Yes | No |

**Decision.** A. One process, the same `StrategyContext` exposed to both languages, and Python errors become C++ exceptions that halt the strategy cleanly.

**Consequences.** A segfault in a C extension takes the engine down (mitigated by IBKR's own server-side order state and the start-up position sync). Revisit with B if strategies become untrusted or heavy.

### ADR-002: C++ strategies compiled on the fly into shared libraries

**Status:** Accepted

**Context.** "Upload a C++ script" must work without a separate build step.

**Decision.** `.cpp` uploads are compiled with the same compiler that built the engine (`-std=c++20 -O2 -shared -fPIC`) into a cache keyed by a hash of the source, compiler and ABI version, then loaded with `dlopen`. A fixed C ABI (`algo_create_strategy`, `algo_destroy_strategy`, `algo_strategy_abi_version`) is exported by `ALGO_EXPORT_STRATEGY`.

**Trade-offs.** Requires a compiler on the machine running the engine. C++ types cross the library boundary, so plugins must use the same compiler and standard library (true by construction for on-the-fly builds; checked by ABI version for prebuilt `.so`).

### ADR-003: TWS API socket client instead of the Web API

**Status:** Accepted

**Context.** IBKR offers the TWS API (TCP socket to TWS or IB Gateway, official C++ client) and the Client Portal / Web API (REST + websocket).

**Decision.** TWS API. It is the route IBKR documents for trading applications, has an official C++ client, streams real-time bars and executions, and does not need a REST session that can be displaced by logging in elsewhere.

**Consequences.** TWS or IB Gateway must run next to the engine and restarts daily (see docs/ibkr-setup.md for IBC). The SDK is not redistributable, so the adapter is an optional build target and the rest of the project builds and tests without it.

### ADR-004: Shared event path for backtest and live

**Status:** Accepted

**Decision.** The backtester drives the real `Engine` through the `Broker` interface instead of having a separate vectorized backtest. Slower than a vectorized backtest (but fast enough for bar data), and strategy behaviour, risk checks and order handling are identical in both modes, which removes a whole class of "works in backtest, fails live" bugs.

### ADR-005: Local web dashboard with a hardened API

**Status:** Accepted

**Context.** Uploads are executable code with access to a brokerage session.

**Decision.** Bind to `127.0.0.1` by default; require an `X-Algo-Client` header on every state-changing request (a cross-site form or image cannot set it, and the server never answers CORS preflights); reject requests whose `Host` header is not local (DNS rebinding). Strategy file names are whitelisted (`[A-Za-z0-9_.-]`, known extensions). Live trading needs `allow_live: true` in the config file, which the API cannot change.

## What to revisit as it grows

- Multiple concurrent strategies sharing one IBKR connection (needs per-strategy order attribution and portfolio netting).
- Persisting orders, fills and equity to disk (SQLite) for restarts and audit.
- Out-of-process strategy sandbox (ADR-001 option B).
- Tick data and order book (`reqTickByTickData`, `reqMktDepth`) for intraday microstructure strategies.
