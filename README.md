# orderBookC++

A low-latency limit-order book and matching engine in modern C++23, plus a
live paper-trader that consumes **Binance USDT-M Futures** (or Spot) market
data through the same SPSC pipeline as the core engine. Every paper fill is
timestamped and a per-run latency summary (`min / p50 / p99 / max / mean`)
prints at exit, so the same binary doubles as a live-data latency probe.

Throughput on Apple M-series, single thread, `-O3 -march=native`:

| Workload                  | Rate        | ns / op |
| ------------------------- | ----------- | ------- |
| Insert Limit (no match)   | ~9.2 M/s    | ~108 ns |
| Aggressive sweep (match)  | ~22 M/s     | ~46 ns  |
| Insert + Cancel           | ~23 M/s     | ~42 ns  |

## Build & run

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/demo                       # scripted matcher example
./build/test_order_book            # core unit tests (11)
./build/test_live                  # strategy + engine + synthetic feed tests (5)
./build/bench_throughput           # Google Benchmark
./build/live_trade                                                          # default: synthetic
./build/live_trade --source synthetic --sigma 30                            # noisier random walk
./build/live_trade --source binance --market futures --symbol BTCUSDT       # WebSocket feed (default), USDT-M perpetual
./build/live_trade --source binance --market spot    --symbol BTCUSDT       # WebSocket feed, spot
./build/live_trade --source binance --feed rest --market futures --symbol BTCUSDT --poll-ms 250  # REST polling instead
./build/live_trade --source binance --feed ws   --market futures --symbol BTCUSDT --seconds 60   # WS, auto-stop
```

### Building on Windows (Git Bash + MSYS2)

The same tree builds on Windows with the MSYS2 MinGW64 toolchain (gcc, cmake,
ninja, libcurl, openssl all from `mingw-w64-x86_64-*`). From **Git Bash**, put
MSYS2's MinGW64 bin on PATH and use the Ninja generator:

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -G Ninja -S . -B build-win
cmake --build build-win
./build-win/test_order_book.exe && ./build-win/test_live.exe
./build-win/live_trade.exe --source binance --feed ws --market futures --symbol BTCUSDT
```

IXWebSocket defaults to mbedtls on Windows, so `CMakeLists.txt` pins the OpenSSL
backend there (`USE_OPEN_SSL`); Linux/macOS keep their native defaults.

Flags for `live_trade`:
- `--source binance|synthetic`   data source (default: `synthetic`)
- `--feed ws|rest`               Binance transport: WebSocket or REST poll (default: `ws`)
- `--market futures|spot`        Binance venue when `--source binance` (default: `futures`)
- `--symbol BTCUSDT[,ETHUSDT…]`  comma-separated symbol list; one independent pipeline per symbol
- `--poll-ms 250`                REST feed tick interval in ms (`--feed rest` only)
- `--seconds 0`                  auto-stop after N seconds (0 = until Ctrl-C)
- `--seed 42`                    RNG seed (synthetic, reproducible runs)
- `--start-price 80000`          synthetic initial mid
- `--sigma 5.0`                  synthetic per-tick stddev USD
- `--drift 0.0`                  synthetic per-tick drift USD
- `--max-pos 0`                  risk: max abs position in BTC (0 = off)
- `--max-notional 0`             risk: max abs position notional in USD (0 = off)
- `--max-drawdown 0`             risk: halt trading after equity drops USD from peak (0 = off)

Synthetic mode runs the whole pipeline offline against a Gaussian random
walk — useful for reproducible demos and stress-testing the engine without
hitting the internet.

## Latency telemetry

Every paper fill prints its **event→fill latency** — the time from when the
feed thread finished parsing the Binance response to when the engine booked
the fill. At exit, a percentile summary covers the whole run:

```
[FILL]   BUY   px=$80520.30  qty=0.001000 BTC  lat=105.5µs
[FILL]   BUY   px=$80521.10  qty=0.001000 BTC  lat=81.5µs
...
[STATS]  fills=11  event→fill latency µs: min=14.0  p50=170.7  p99=234.0  max=234.0  mean=146.6
```

What the number does *not* include: the TLS+TCP roundtrip to Binance (that's
upstream of `ev.ts`). What it *does* include: SPSC pop → strategy on_tick →
position update → callback invocation.

**REST vs WebSocket.** Both feeds stamp `ev.ts` with `now_ns()` at the same
point — right after the JSON parse — so the latency numbers are directly
comparable across `--feed rest` and `--feed ws`. The difference is *what the
events are*: REST `bookTicker` is edge-cached and, in quiet markets, returns
identical back-to-back snapshots, so consecutive polls carry no new
information. The WebSocket feed is pushed tick-by-tick, so the telemetry
reflects genuine order-book updates rather than re-stamped duplicates. A/B them
in the same binary: run with `--feed rest` then `--feed ws` against the same
symbol and compare the `[STATS]` summaries.

Measured numbers on Apple M-series,
single thread, busy-spin engine: **min ~5 µs, p50 ~10–20 µs**. With the
default 200 µs engine back-off sleep enabled, p50 climbs to ~150 µs because
events arriving mid-sleep wait out the remainder. Toggle by uncommenting the
`sleep_for` line in `src/trading_engine.cpp` to compare both regimes.

## Layout

```
include/order_book/
  cache.hpp            kCacheLine, OB_LIKELY/UNLIKELY, OB_PREFETCH, OB_ALWAYS_INLINE
  types.hpp            Order, Side, OrderType, Trade
  pool_allocator.hpp   Fixed-size object pool (free-list)
  spsc_queue.hpp       Wait-free SPSC ring buffer
  price_level.hpp      Intrusive FIFO at one price
  order_book.hpp       SideBook + OrderBook + match engine declarations
  feed.hpp             Feed base + BinanceFeed (REST) + SyntheticFeed (RNG)
  strategy.hpp         Strategy interface + MeanReversion
  trading_engine.hpp   Paper-trading engine + Position
src/
  order_book.cpp       Matching engine
  feed.cpp             libcurl + nlohmann/json client
  trading_engine.cpp   SPSC consumer, strategy dispatch, paper fills
  main.cpp             Scripted demo
  live_trade.cpp       Live paper-trading entry point
tests/  bench/         GoogleTest + Google Benchmark
```

## Architecture

```
Binance Futures ──[WSS]───> WebSocketBinanceFeed thread  (IXWebSocket + nlohmann/json)
fstream.binance.com    └──[HTTPS, --feed rest]──> BinanceFeed thread  (libcurl)
                                  │  stamps ev.ts = now_ns() after parse
                                  ▼  try_push()
                          SPSCQueue<MarketEvent>         (lock-free, cache-padded)
                                  │
                                  ▼  try_pop()
                           TradingEngine thread
                            ├─ updates last_mid
                            ├─ MeanReversion.on_tick → optional StrategyAction
                            └─ apply_action → updates Position (cash/btc)
                                                       │  stamps fill.ts = now_ns()
                                                       ▼
                                                  on_fill callback
                                                       │  lat = fill.ts - ev.ts
                                                       ▼
                                                console log + [STATS] summary
```

The same SPSC queue from the core library powers the live data path —
producer (REST feed) and consumer (engine) sit on separate cache lines and
exchange `MarketEvent`s without locks.

## Design choices

### Core engine
- **Intrusive FIFO** at each price level — links inside `Order`, no per-order
  list-node allocation.
- **`std::vector<PriceLevel>`** kept sorted so best price is `levels_.back()`
  for both sides (`flat_map` semantics by hand).
- **`ObjectPool<Order>`** with a free-list — no `malloc` on the hot path
  after construction.
- **SPSC ring buffer** with cache-line-padded indices and locally cached
  remote indices (rigtorp / LMAX Disruptor pattern).
- C++23: `std::span`, `[[nodiscard]]`, designated initializers,
  `std::hardware_destructive_interference_size`.
- Compiler hygiene: `-O3 -march=native` on the hot library; sanitizer build
  available via `-DCMAKE_BUILD_TYPE=Debug`.

### Live path
- **WebSocket feed (default)** via [IXWebSocket](https://github.com/machinezone/IXWebSocket),
  pulled in by FetchContent like `nlohmann/json`. Subscribes to the
  `<symbol>@bookTicker` stream (symbol lowercased as Binance requires):
  - Futures (default): `wss://fstream.binance.com/ws/<symbol>@bookTicker`
  - Spot:              `wss://stream.binance.com:9443/ws/<symbol>@bookTicker`

  Chosen over Boost.Beast+OpenSSL for its one-line CMake integration and
  built-in TLS (OpenSSL on Linux, SecureTransport on macOS — no separate TLS
  wiring). The client runs on its own producer thread; the on-message handler
  parses each frame, stamps `ev.ts`, and `try_push`es into the SPSC queue —
  same contract as the REST producer. IXWebSocket auto-replies pong to
  Binance's ping frames (RFC6455) and reconnects with exponential backoff
  (1s → 30s cap) on disconnect.
- **REST polling with libcurl** (`--feed rest`). Endpoint chosen at
  construction time:
  - Futures (default): `fapi.binance.com/fapi/v1/ticker/bookTicker`
  - Spot:              `api.binance.com/api/v3/ticker/bookTicker`

  macOS system libcurl ships with SecureTransport TLS — no extra system
  dependency. Response schema is identical between the two venues, so the
  JSON parser is shared.
- **`nlohmann/json`** via FetchContent — header-only. Shared by both feeds; the
  WS stream uses the compact `b`/`B`/`a`/`A` keys, REST the verbose
  `bidPrice`/`bidQty`/`askPrice`/`askQty`.
- **Matching through the real `OrderBook`**: each tick refreshes a synthetic
  top-of-book (one resting bid + ask at the displayed best prices/sizes, with
  stable OrderIds so state stays bounded). Strategy actions are submitted as
  marketable **IOC** orders through `OrderBook::submit()`, so fills come from
  real `Trade`s — including realistic **partial fills** when the action size
  exceeds the resting top-of-book. No real account, money, or API keys; no
  orders sent anywhere.
- **Risk module** (`risk.hpp`): an optional `RiskManager` gates every action
  before it reaches the book — clamps size to a max abs **position** and max
  **notional**, and permanently **halts** trading once equity drops more than
  the drawdown limit from its peak. All integer math; enabled per engine via
  `--max-pos` / `--max-notional` / `--max-drawdown` (0 = off).
- **Multi-symbol**: `--symbol A,B,C` spawns one independent pipeline per
  symbol — its own SPSC queue, feed, strategy, and engine — with per-symbol
  `[FILL]` / `[STATUS]` / `[STATS]` lines.
- **Price scaling**: prices are stored as integer "ticks" of $0.01,
  quantities as integer μBTC (1e-6 BTC). All math is integer; no floats on
  the hot path.
- **Latency stamping**: feed sets `MarketEvent::ts` with `now_ns()` right
  after the parse; engine stamps `Fill::ts` at the moment of book update.
  `Fill::event_ts` carries the feed timestamp through so the on_fill callback
  can compute end-to-end latency without extra plumbing. Samples are
  accumulated into a fixed-memory `LatencyHistogram` (64 log2 buckets, exact
  min/max/mean) so long runs don't grow an unbounded sample vector.

### Strategy: Mean Reversion
- Rolling N-tick window of mid-prices.
- Z-equivalent: deviation from mean expressed in basis points.
- Buy when current mid is `entry_bps` below mean, sell when above.
- Cool-down period between actions to avoid flapping.
- Live defaults in `src/live_trade.cpp`: `window=20`, `entry_bps=0.5`,
  `trade_qty=0.001 BTC`, `cool_down=5` — chosen so short demos against
  quiet BTC markets still produce visible fills, not as a viable strategy.

## Open next steps
- Pin one engine per core (CPU affinity) for the multi-symbol path; today the
  per-symbol pipelines run on the OS scheduler.
- Deeper synthetic L2 (more than top-of-book) so partial fills walk multiple
  levels.
- Persisting fills + reconciliation against a real exchange (paper account
  on Binance Testnet would be the first step).

## Research

- arXiv:2309.04259 — *C++ Design Patterns for Low-latency Applications
  Including High-frequency Trading.*
- ScienceDirect S0743731524001643 — *Semi-static conditions in low-latency
  C++ for high frequency trading.*
- rigtorp/SPSCQueue — reference SPSC implementation.
- Sourav Ghosh, *Building Low Latency Applications with C++* (Packt).
- Meeting C++ 2025 — Quasar Chunawala, *Designing an SPSC Lock-free Queue.*

## Disclaimer

This is an educational project. The strategy is naive and will lose money
in any non-trivial market regime. Do not point it at a real exchange
account. The live trader uses only Binance's **public** market-data endpoints
(spot and USDT-M futures, over WebSocket or REST) — no API keys, no signed
requests, no orders are ever sent to the exchange.
