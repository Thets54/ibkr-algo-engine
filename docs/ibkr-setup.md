# Interactive Brokers setup

## 1. Account

- The TWS API requires an **IBKR Pro** account (IBKR lists this in its TWS API requirements).
- Every IBKR account comes with a **paper trading** account (ids start with `DU`). Use it first: paper mode in this engine refuses to connect to anything else.
- To get your live market data subscriptions in the paper account, enable market data sharing with the paper account in the Client Portal settings.

## 2. Market data

What you get without paying (per IBKR's market data pricing page at the time of writing):

- free real-time streaming data on US-listed stocks and ETFs from Cboe One and IEX, **non-consolidated** (no NBBO);
- free delayed data (usually 10 to 20 minutes) on other products where available;
- 100 free snapshot quotes per month.

The engine's live bars come from `reqRealTimeBars`, which needs real-time data permissions for each instrument. If you see errors about market data permissions in the Logs tab, subscribe to the relevant feed or test with US stocks first. Historical warm-up bars (`reqHistoricalData`) are subject to IBKR's pacing limits; keep `warmup_bars` reasonable when trading many symbols.

Check current prices and rules on interactivebrokers.com: they change.

## 3. TWS or IB Gateway

IB Gateway is lighter (no trading UI) and is the better choice for a bot. In **Configure > Settings > API > Settings**:

- tick **Enable ActiveX and Socket Clients**;
- note the **Socket port** and put it in `config.json` (`ibkr.port`);
- keep **Allow connections from localhost only** ticked if the engine runs on the same machine;
- untick **Read-Only API**, otherwise orders are refused.

IBKR announced that TWS / IB Gateway 10.45 clears "Enable ActiveX and Socket Clients" and "Allow connections from localhost only", so re-check these after upgrading.

Default ports:

| | Live | Paper |
|---|---|---|
| TWS | 7496 | 7497 |
| IB Gateway | 4001 | 4002 |

TWS / Gateway log out once a day. For unattended running use [IBC](https://github.com/IbcAlpha/IBC) to handle login and restarts; the engine retries the connection every 10 seconds while running.

## 4. Build the IBKR adapter

1. Download the TWS API for your OS from IBKR's official API download page and unpack it.
2. The C++ client sources are in `source/cppclient/client`. Since API 10.x it uses the Intel Decimal Floating-Point Math Library (`Decimal`); the SDK includes its sources. Build `libbid.a` (Linux/macOS) following the SDK's instructions.
3. Configure:

```bash
cmake -S . -B build -DALGO_WITH_IBKR=ON \
      -DTWS_API_DIR=$HOME/twsapi/source/cppclient/client \
      -DTWS_DECIMAL_LIB=$HOME/twsapi/.../libbid.a
cmake --build build -j
```

4. Check: `curl -s 127.0.0.1:8080/api/health` should report `"ibkr": true` once the dashboard runs.

### SDK version differences

TWS API 10.33 changed two signatures used by the adapter (IBKR changelog, December 2024): `EWrapper::error()` gained an `errorTime` argument and `EClient::cancelOrder()` takes an `OrderCancel` object. Both are handled by CMake options that default to the new API:

```bash
# SDK older than 10.33
cmake ... -DTWS_ERROR_WITH_TIME=OFF -DTWS_CANCEL_WITH_ORDERCANCEL=OFF
```

If a newer SDK changes other callbacks, the compiler reports "marked override, but does not override" on the exact method; adjust it in `src/broker/ibkr_broker.cpp`. Some recent SDK releases also add dependencies of their own (check the SDK's own build files); add them to `TWS_DECIMAL_LIB` or the `twsapi` target in `CMakeLists.txt`.

## 5. First paper session

```bash
./build/algoengine run -s examples/strategies/sma_crossover.py --symbols AAPL \
    --mode paper --bar-seconds 60 --param fast=5 --param slow=20
```

or choose **Paper** in the dashboard. You should see, in order: `connected to IBKR`, `history loaded for AAPL`, `strategy started`, then bars every minute during market hours.

## Symbols

| Input | Contract |
|---|---|
| `AAPL` | Stock, `SMART` routing, `USD` (change `ibkr.exchange` / `ibkr.currency` in `config.json` for other markets) |
| `EUR.USD` | Forex pair on IDEALPRO, midpoint bars |

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `eConnect ... failed` | Gateway not running, wrong port, API not enabled |
| `no handshake within 10s` | Another program uses the same `client_id` |
| `paper mode refused` | Connected to the live port; use 7497 / 4002 |
| Error 354 / 10167 / 10168 in logs | No market data permission for that instrument |
| Error 162 / 366 | Historical data pacing or no data for that bar size |
| Orders rejected with 201 | Check the reason: permissions, margin, or Read-Only API |
