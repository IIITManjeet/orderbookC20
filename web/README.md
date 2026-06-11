# Order Book Telemetry Dashboard

A fully static, single-page dashboard for the C++ low-latency crypto paper-trader.
No build step required — open `index.html` directly or serve the `web/` directory
from any static host (GitHub Pages, Netlify, local `python3 -m http.server`).

---

## Data Schema (JSONL)

Each WebSocket message (live mode) or line in a `.jsonl` file (replay mode) is a
single JSON object. Three record types:

### `meta` — session metadata (sent once at startup)
```json
{"t":"meta","ts_ns":1718000000000000000,"source":"binance","feed":"ws","symbols":["BTCUSDT","ETHUSDT"]}
```
| Field | Type | Description |
|-------|------|-------------|
| `ts_ns` | u64 | Wall-clock nanoseconds (Unix epoch) |
| `source` | string | `"binance"` or `"synthetic"` |
| `feed` | string | `"ws"` or `"rest"` |
| `symbols` | string[] | Active trading symbols |

### `status` — per-symbol book snapshot (sent on every tick)
```json
{"t":"status","ts_ns":1718000000050000000,"sym":"BTCUSDT","mid":67450.25,"bid":67449.00,"ask":67451.50,"pos_btc":0.05,"cash":96627.10,"equity":100034.22,"fills":1}
```
| Field | Type | Description |
|-------|------|-------------|
| `sym` | string | Trading symbol |
| `mid` | float | Mid price |
| `bid` / `ask` | float | Best bid / ask |
| `pos_btc` | float | Current position in base asset |
| `cash` | float | Cash balance (USD) |
| `equity` | float | Total equity: cash + mark-to-market |
| `fills` | u64 | Cumulative fill count |

### `fill` — individual trade execution
```json
{"t":"fill","ts_ns":1718000000400000000,"sym":"BTCUSDT","side":"BUY","px":67458.00,"qty":0.05,"lat_us":3.2}
```
| Field | Type | Description |
|-------|------|-------------|
| `side` | string | `"BUY"` or `"SELL"` |
| `px` | float | Fill price |
| `qty` | float | Fill quantity (base asset) |
| `lat_us` | float | Order round-trip latency in microseconds |

---

## URL Modes

### Replay (default — works with no backend)

Opens `./sample.jsonl` (or a custom file) and replays records in chronological
order, looping at the end.

```
index.html                         # default: sample.jsonl at 20× speed
index.html?speed=5                 # 5× replay speed
index.html?speed=100               # 100× fast-forward
index.html?file=./my_session.jsonl # custom recorded session
index.html?file=https://example.com/session.jsonl&speed=50
```

The `speed` parameter is a multiplier on wall-clock time relative to the recorded
timestamps. At `speed=20` (default), one second of real time plays 20 seconds of
recorded data.

### Live (requires running backend)

If `?ws=` is present the dashboard opens a WebSocket to that address and processes
each incoming message as one JSON record.

```
index.html?ws=ws://localhost:8080
index.html?ws=wss://my-trader.example.com/telemetry
```

The WebSocket server must send newline-terminated JSON records (one object per
message) matching the schema above. On disconnect the dashboard automatically
retries every 3 seconds.

---

## Connecting the C++ Trader

The trader's existing feed infrastructure (see `src/feed.cpp`) emits status and
fill data. To feed this dashboard:

1. Add a thin WebSocket broadcast layer in the trading engine that serialises
   `status` and `fill` structs into the JSON schema above and sends them to all
   connected dashboard clients.
2. Point the dashboard at the server:
   ```
   index.html?ws=ws://localhost:8080
   ```
3. Alternatively, log records to a `.jsonl` file during a session and replay it:
   ```
   index.html?file=./session_2024-06-10.jsonl&speed=10
   ```

---

## Visuals

| Panel | Description |
|-------|-------------|
| **Symbols** | Live stat cards per symbol: mid/bid/ask, position, cash, equity, PnL, fills |
| **Mid Price** | Scrolling line chart, one series per symbol, last 500 points |
| **Recent Fills** | Scrolling table, newest-first, BUY=green, SELL=red, latency column |
| **Fill Latency** | Min/P50/P99/Max labels + histogram bucketed over all fills in session |

---

## Graceful CDN Degradation

Chart.js is loaded from `cdn.jsdelivr.net`. If the CDN is unreachable:
- The chart containers display a plain-text message.
- All stat cards, fills table, and latency statistics continue to work.
- No JS errors are thrown — `app.js` guards every Chart.js call with
  `if (typeof Chart !== 'undefined')`.

---

## Local Development

```bash
cd web/
python3 -m http.server 8000
# open http://localhost:8000
```

For live mode, start the C++ trader's WS server then:
```
http://localhost:8000/index.html?ws=ws://localhost:8080
```
