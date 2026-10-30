# Strategy API

A strategy is a class with a few hooks. The engine calls them on a single thread, in time order.

| Hook | When |
|---|---|
| `on_start(ctx)` | Once, before the first live bar. Read parameters here. |
| `on_bar(ctx, bar)` | Each completed bar of each symbol. **Required.** |
| `on_fill(ctx, fill)` | Each execution (partial fills arrive separately). |
| `on_order_rejected(ctx, order)` | The broker rejected an order (`order.reason` says why). |
| `on_stop(ctx)` | Once, when the run stops. |

If a hook raises (Python) or throws (C++), the strategy is halted, the kill switch trips and the error with its traceback appears in the logs.

## Context methods

| Python | C++ | Notes |
|---|---|---|
| `ctx.buy(sym, qty)` / `ctx.sell(sym, qty)` | same | Market order. Returns the order id, or `0` if rejected by risk. |
| `ctx.buy_limit(sym, qty, px)` / `ctx.sell_limit(...)` | same | Day limit order. |
| `ctx.target_position(sym, qty)` | same | Trades the difference between `qty` and position plus open orders. Idempotent: safe to call on every bar. |
| `ctx.cancel(order_id)` / `ctx.cancel_all(sym="")` | same | |
| `ctx.position(sym)` | same | Signed quantity. |
| `ctx.avg_price(sym)` | same | |
| `ctx.pending_quantity(sym)` | same | Signed quantity of open orders. |
| `ctx.cash`, `ctx.equity` | `ctx.cash()`, `ctx.equity()` | |
| `ctx.history(sym, n)` | same | Last `n` bars, oldest first, current bar last. |
| `ctx.closes(sym, n)` | (use `history`) | List of the last `n` closes. |
| `ctx.symbols`, `ctx.now` | `ctx.symbols()`, `ctx.now()` | `now` is the bar time in backtests, wall clock live (Unix seconds, UTC). |
| `ctx.param(name, default)` | `ctx.param(name, def)` / `ctx.param_str(...)` | Python returns the type of `default` (int, float or str). |
| `ctx.log(msg)` | same | Shows in the dashboard Logs tab. |

`Bar` fields: `symbol, time, open, high, low, close, volume`. `Fill` fields: `order_id, symbol, side, quantity, price, commission, time`.

## Python specifics

- `import algo` and subclass `algo.Strategy`. If the file defines several subclasses, set `STRATEGY = MyClass`.
- Files next to the strategy can be imported (its folder is added to `sys.path`). Installed packages such as numpy work if they are installed for the Python the engine was built with.
- The interpreter is shared across runs in one process: avoid relying on module-level state between runs.

## C++ specifics

- Include only `<algo/strategy/strategy.hpp>` and end the file with `ALGO_EXPORT_STRATEGY(YourClass)`.
- Uploaded `.cpp` files are compiled with `-std=c++20 -O2`. Set `ALGO_CXX` to use another compiler and `ALGO_INCLUDE_DIR` if the engine is installed away from its source tree.
- Prebuilt `.so` files must be built with the same compiler and standard library as the engine.

## Execution semantics

**Backtest.** For each timestamp, orders from the previous step are matched first:

- market: next bar open, plus or minus `commission.slippage_bps`;
- limit buy: if next low <= limit, at `min(open, limit)`; limit sell: if next high >= limit, at `max(open, limit)`;
- commission: `max(min_per_order, per_share * qty)`, capped at `max_pct_of_value`.

Then positions are marked to the close, equity is recorded and `on_bar` is called. No intrabar information leaks into decisions.

**Live.** Orders go to IBKR immediately as DAY orders with SMART routing. Fills arrive asynchronously, so check `pending_quantity` or use `target_position` rather than assuming an order filled.

## Risk limits

Every strategy order is checked before it is sent (see `risk` in `config.json`). Orders that only reduce an existing position are always allowed, except by the rate limit. When the daily loss limit is breached, or a strategy error occurs, the kill switch cancels open orders and, if `flatten_on_kill` is true, closes all positions with market orders. The strategy receives no more bars until the run is restarted.

## Checklist before going live

1. Backtest on real data you trust, with realistic commissions, and check the out-of-sample period separately.
2. Run on a paper account for several sessions; compare fills with the backtest assumptions.
3. Set risk limits that would make a bug cheap, not a strategy profitable.
4. Live: set `"allow_live": true`, use a dedicated `client_id`, watch the first session.
