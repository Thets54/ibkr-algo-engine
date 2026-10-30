# Known limitations and tech debt

Scored with Priority = (Impact + Risk) x (6 - Effort), each on 1 to 5. Highest first.

| # | Item | Type | Impact | Risk | Effort | Priority |
|---|---|---|---|---|---|---|
| 1 | Live commissions are not recorded (fills carry `commission = 0`; IBKR sends them in a separate callback renamed in API 10.33) | Code | 3 | 4 | 2 | 28 |
| 2 | Adapter not compiled in CI (SDK not redistributable) | Test | 3 | 4 | 3 | 21 |
| 3 | No persistence: orders, fills and equity are lost on restart | Architecture | 4 | 3 | 3 | 21 |
| 4 | Live bars are emitted when the next 5 second bar arrives, so the last bar of the session is only emitted at the next session open | Code | 2 | 2 | 1 | 20 |
| 6 | Order status relies on execDetails and error codes, not `orderStatus` (signature varies by SDK version) | Code | 2 | 3 | 2 | 20 |
| 5 | Kill switch flattens from the local position; if an open order fills between the cancel and the flatten, a residual position can remain | Code | 2 | 4 | 3 | 18 |
| 9 | Only stocks and forex contracts; no futures, options or combos | Code | 3 | 1 | 3 | 12 |
| 10 | Daily loss resets on the UTC day, not the exchange calendar | Code | 1 | 2 | 2 | 12 |
| 11 | No dashboard JS tests | Test | 1 | 2 | 2 | 12 |
| 7 | One run at a time; no multi-strategy netting | Architecture | 3 | 1 | 4 | 8 |
| 8 | Strategies run in-process: a crash in a Python C extension stops the engine | Architecture | 2 | 3 | 5 | 5 |

## Remediation plan

**Phase 1 (before trusting live PnL):** 1, 4, 6. Subscribe to commission reports and `orderStatus` behind the same version switches as `error()`; add a timer in `IbkrBroker` that closes the current bar `bar_seconds` after it opened.

**Phase 2 (operational robustness):** 3 and 5. SQLite journal of orders/fills/equity; reconcile with `reqPositions` and `reqOpenOrders` after a kill and on reconnect.

**Phase 3 (scope):** 2 (a self-hosted CI runner with the SDK), 9, 7, 8.
