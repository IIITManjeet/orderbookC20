/**
 * Telemetry Dashboard — app.js
 *
 * Modes:
 *   REPLAY (default) — fetches ./sample.jsonl (or ?file=<url>), replays
 *                      records in time order, then HOLDS the full session on
 *                      screen. Speed via ?speed=N (default 4); ?loop=1 repeats.
 *   LIVE             — if ?ws=ws://host:port present, opens a WebSocket and
 *                      snapshots to sessionStorage so a refresh restores it
 *                      (cleared when the tab closes — nothing kept long-term).
 *
 * Records follow the JSONL schema from the C++ trader:
 *   {"t":"meta",   "ts_ns":..., "source":..., "feed":..., "symbols":[...]}
 *   {"t":"status", "ts_ns":..., "sym":..., "mid":..., "bid":..., "ask":...,
 *                  "pos_btc":..., "cash":..., "equity":..., "fills":...}
 *   {"t":"fill",   "ts_ns":..., "sym":..., "side":"BUY|SELL", "px":...,
 *                  "qty":..., "lat_us":...}
 */

'use strict';

/* ─────────────────────── CONFIG ─────────────────────── */
const MAX_PRICE_POINTS = 500;   // per symbol, ring-buffer
const MAX_FILLS_ROWS   = 200;
const MAX_LAT_SAMPLES  = 1000;
const LAT_BUCKETS      = 20;    // histogram buckets

/* ─────────────────────── PARSE QUERY PARAMS ─────────────────────── */
const params    = new URLSearchParams(location.search);
const WS_URL    = params.get('ws')    || null;
const FILE_URL  = params.get('file')  || './sample.jsonl';
const SPEED     = Math.max(0.1, parseFloat(params.get('speed') || '4'));
const LOOP      = params.get('loop') === '1';   // replay: repeat (wipes each cycle) vs play-once-and-hold
const MODE      = WS_URL ? 'live' : 'replay';

// Ephemeral persistence: snapshot the LIVE session to sessionStorage so a page
// refresh restores it. sessionStorage (not localStorage) auto-clears when the
// tab closes — refresh-proof, but nothing is kept long-term or on disk.
const STORE_KEY = 'ob-dash-session-v1';

/* ─────────────────────── STATE ─────────────────────── */
const symColors = ['#58a6ff','#3fb950','#ffa657','#bc8cff','#f85149','#d29922'];

const state = {
  symbols:    [],          // ordered list of known syms
  symIndex:   {},          // sym -> palette index
  stats:      {},          // sym -> latest status record
  priceHist:  {},          // sym -> [{ts_ms, mid}]  ring-buffer
  fills:      [],          // fill records, newest-first
  latSamples: [],          // lat_us values
  meta:       null,
};

/* ─────────────────────── DOM REFS ─────────────────────── */
const $statusPill   = document.getElementById('status-pill');
const $modeBadge    = document.getElementById('mode-badge');
const $cardsRow     = document.getElementById('cards-row');
const $fillsTbody   = document.getElementById('fills-tbody');
const $latMin       = document.getElementById('lat-min');
const $latP50       = document.getElementById('lat-p50');
const $latP99       = document.getElementById('lat-p99');
const $latMax       = document.getElementById('lat-max');
const $chartLegend  = document.getElementById('chart-legend');

/* ─────────────────────── CHART HANDLES ─────────────────────── */
let priceChart = null;   // Chart.js instance or null if CDN blocked
let latChart   = null;

function colorForSym(sym) {
  return symColors[state.symIndex[sym] % symColors.length];
}

/* ─────────────────────── CHART INIT ─────────────────────── */
function initCharts() {
  if (typeof Chart === 'undefined') return;  // CDN blocked — graceful degrade

  Chart.defaults.color      = '#8b949e';
  Chart.defaults.borderColor = '#30363d';

  const priceCtx = document.getElementById('price-chart-canvas');
  if (priceCtx) {
    priceChart = new Chart(priceCtx.getContext('2d'), {
      type: 'line',
      data: { datasets: [] },
      options: {
        animation: false,
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'index', intersect: false },
        plugins: {
          legend: { display: false },
          tooltip: {
            callbacks: {
              // y is % change from each series' first point; show % and the
              // absolute price (stashed on the point as .px).
              label: ctx => {
                const pct = ctx.parsed.y;
                const px  = ctx.raw && ctx.raw.px != null ? ` ($${ctx.raw.px.toFixed(2)})` : '';
                return ` ${ctx.dataset.label}: ${pct >= 0 ? '+' : ''}${pct.toFixed(3)}%${px}`;
              },
            }
          }
        },
        scales: {
          x: {
            type: 'linear',
            ticks: {
              maxTicksLimit: 8,
              callback: v => new Date(v).toLocaleTimeString(),
            },
            grid: { color: '#21262d' },
          },
          y: {
            grid: { color: '#21262d' },
            // % change from each symbol's first point, so symbols at very
            // different price levels ($62k BTC vs $1.6k ETH) are comparable.
            ticks: { callback: v => (v >= 0 ? '+' : '') + v.toFixed(2) + '%' },
          }
        }
      }
    });
  }

  const latCtx = document.getElementById('lat-hist-canvas');
  if (latCtx) {
    latChart = new Chart(latCtx.getContext('2d'), {
      type: 'bar',
      data: {
        labels: [],
        datasets: [{
          label: 'fills',
          data: [],
          backgroundColor: 'rgba(88,166,255,0.5)',
          borderColor: '#58a6ff',
          borderWidth: 1,
        }]
      },
      options: {
        animation: false,
        responsive: true,
        maintainAspectRatio: false,
        plugins: { legend: { display: false } },
        scales: {
          x: {
            grid: { color: '#21262d' },
            ticks: { font: { size: 10 } }
          },
          y: {
            grid: { color: '#21262d' },
            ticks: { stepSize: 1, font: { size: 10 } }
          }
        }
      }
    });
  }
}

/* ─────────────────────── STATUS PILL ─────────────────────── */
function setPill(kind, text) {
  $statusPill.className = 'status-pill ' + kind;
  const dot = $statusPill.querySelector('.dot');
  dot.className = 'dot' + (kind === 'live' || kind === 'replay' ? ' blink' : '');
  $statusPill.querySelector('.pill-text').textContent = text;
}

/* ─────────────────────── REGISTER SYMBOL ─────────────────────── */
function ensureSym(sym) {
  if (sym in state.symIndex) return;
  const idx = state.symbols.length;
  state.symIndex[sym] = idx;
  state.symbols.push(sym);
  state.priceHist[sym] = [];
  state.stats[sym]     = null;
  updateLegend();
  if (priceChart) {
    priceChart.data.datasets.push({
      label: sym,
      data: [],
      borderColor: colorForSym(sym),
      backgroundColor: 'transparent',
      borderWidth: 1.5,
      pointRadius: 0,
      tension: 0.2,
    });
    priceChart.update('none');
  }
}

function updateLegend() {
  $chartLegend.innerHTML = '';
  state.symbols.forEach(sym => {
    const li = document.createElement('div');
    li.className = 'legend-item';
    li.innerHTML = `<span class="legend-dot" style="background:${colorForSym(sym)}"></span>${sym}`;
    $chartLegend.appendChild(li);
  });
}

/* ─────────────────────── PROCESS RECORD ─────────────────────── */
function processRecord(rec) {
  switch (rec.t) {
    case 'meta':   handleMeta(rec);   break;
    case 'status': handleStatus(rec); break;
    case 'fill':   handleFill(rec);   break;
  }
  saveState();
}

/* ─────────────────────── EPHEMERAL PERSISTENCE (sessionStorage) ─────────────────────── */
let _lastSave = 0;
function saveState() {
  if (MODE !== 'live') return;            // replay's source file is already its persistence
  const now = Date.now();
  if (now - _lastSave < 1000) return;     // throttle to ~1/s
  _lastSave = now;
  try {
    sessionStorage.setItem(STORE_KEY, JSON.stringify({
      symbols: state.symbols, symIndex: state.symIndex, stats: state.stats,
      priceHist: state.priceHist, fills: state.fills,
      latSamples: state.latSamples, meta: state.meta,
    }));
  } catch (e) { /* quota/disabled — non-fatal */ }
}

function loadState() {
  try {
    const raw = sessionStorage.getItem(STORE_KEY);
    if (!raw) return false;
    const o = JSON.parse(raw);
    if (!o || !Array.isArray(o.symbols) || o.symbols.length === 0) return false;
    state.symbols    = o.symbols;
    state.symIndex   = o.symIndex   || {};
    state.stats      = o.stats      || {};
    state.priceHist  = o.priceHist  || {};
    state.fills      = o.fills      || [];
    state.latSamples = o.latSamples || [];
    state.meta       = o.meta       || null;
    return true;
  } catch (e) { return false; }
}

// Rebuild all views (chart datasets, legend, tables) from restored state.
function rebuildFromState() {
  if (priceChart) {
    priceChart.data.datasets = state.symbols.map(sym => ({
      label: sym, data: [], borderColor: colorForSym(sym),
      backgroundColor: 'transparent', borderWidth: 1.5, pointRadius: 0, tension: 0.2,
    }));
  }
  updateLegend();
  updatePriceChart();
  renderCards();
  renderFillsTable();
  renderLatency();
}

function handleMeta(rec) {
  state.meta = rec;
  (rec.symbols || []).forEach(s => ensureSym(s));
}

function handleStatus(rec) {
  ensureSym(rec.sym);
  state.stats[rec.sym] = rec;

  // push to price history ring-buffer
  const hist = state.priceHist[rec.sym];
  hist.push({ ts_ms: rec.ts_ns / 1e6, mid: rec.mid });
  if (hist.length > MAX_PRICE_POINTS) hist.shift();

  renderCards();
  updatePriceChart();
}

function handleFill(rec) {
  ensureSym(rec.sym);
  state.fills.unshift(rec);
  if (state.fills.length > MAX_FILLS_ROWS) state.fills.pop();

  state.latSamples.push(rec.lat_us);
  if (state.latSamples.length > MAX_LAT_SAMPLES) state.latSamples.shift();

  renderFillsTable();
  renderLatency();
}

/* ─────────────────────── RENDER: SYMBOL CARDS ─────────────────────── */
function renderCards() {
  $cardsRow.innerHTML = '';
  if (state.symbols.length === 0) {
    $cardsRow.innerHTML = '<div class="empty-state">Waiting for data…</div>';
    return;
  }
  state.symbols.forEach(sym => {
    const s    = state.stats[sym];
    const col  = colorForSym(sym);
    const card = document.createElement('div');
    card.className = 'sym-card';
    card.style.borderLeftColor = col;
    if (!s) {
      card.innerHTML = `<div class="sym-name" style="color:${col}">${sym}</div>
        <div style="color:var(--muted);font-size:11px">Awaiting status…</div>`;
    } else {
      // The paper-trader starts from ZERO cash — cash/equity are deltas, not an
      // account balance. So net P&L is simply the equity (cash + mark-to-market
      // position), not equity minus a starting balance.
      const pnlRaw = s.equity;
      const pnlCol = pnlRaw >= 0 ? 'var(--green)' : 'var(--red)';
      const pnlSign = pnlRaw >= 0 ? '+' : '';
      card.innerHTML = `
        <div class="sym-name" style="color:${col}">${sym}</div>
        <div class="mid-val">$${fmtNum(s.mid, 2)}</div>
        <div class="stat-grid">
          <span class="stat-label">Bid</span>
          <span class="stat-val" style="color:var(--green)">$${fmtNum(s.bid,2)}</span>
          <span class="stat-label">Ask</span>
          <span class="stat-val" style="color:var(--red)">$${fmtNum(s.ask,2)}</span>
          <span class="stat-label">Position</span>
          <span class="stat-val">${fmtNum(s.pos_btc,4)}</span>
          <span class="stat-label">Cash</span>
          <span class="stat-val">$${fmtNum(s.cash,2)}</span>
          <span class="stat-label">Equity</span>
          <span class="stat-val">$${fmtNum(s.equity,2)}</span>
          <span class="stat-label">PnL</span>
          <span class="stat-val" style="color:${pnlCol}">${pnlSign}$${fmtNum(pnlRaw,2)}</span>
          <span class="stat-label">Fills</span>
          <span class="stat-val">${s.fills}</span>
        </div>`;
    }
    $cardsRow.appendChild(card);
  });
}

/* ─────────────────────── RENDER: PRICE CHART ─────────────────────── */
function updatePriceChart() {
  if (!priceChart) return;
  state.symbols.forEach((sym, i) => {
    const ds = priceChart.data.datasets[i];
    if (!ds) return;
    const hist = state.priceHist[sym];
    // Plot percent change from each symbol's first observed mid, so multiple
    // symbols at different price levels share one readable axis. Keep the raw
    // price on the point (.px) for the tooltip.
    const base = hist.length ? hist[0].mid : 0;
    ds.data = hist.map(p => ({
      x: p.ts_ms,
      y: base > 0 ? (p.mid / base - 1) * 100 : 0,
      px: p.mid,
    }));
  });
  priceChart.update('none');
}

/* ─────────────────────── RENDER: FILLS TABLE ─────────────────────── */
function renderFillsTable() {
  if (state.fills.length === 0) {
    $fillsTbody.innerHTML = '<tr><td colspan="6" class="empty-state">No fills yet.</td></tr>';
    return;
  }
  $fillsTbody.innerHTML = '';
  const frag = document.createDocumentFragment();
  state.fills.forEach(f => {
    const tr   = document.createElement('tr');
    const col  = colorForSym(f.sym);
    const time = fmtTime(f.ts_ns);
    tr.innerHTML = `
      <td>${time}</td>
      <td><span class="sym-label" style="color:${col};border:1px solid ${col}">${f.sym}</span></td>
      <td class="side-${f.side.toLowerCase()}">${f.side}</td>
      <td>$${fmtNum(f.px,2)}</td>
      <td>${fmtNum(f.qty,4)}</td>
      <td>${fmtNum(f.lat_us,1)} µs</td>`;
    frag.appendChild(tr);
  });
  $fillsTbody.appendChild(frag);
}

/* ─────────────────────── RENDER: LATENCY ─────────────────────── */
function renderLatency() {
  const s = state.latSamples;
  if (s.length === 0) return;

  const sorted = [...s].sort((a, b) => a - b);
  const mn  = sorted[0];
  const mx  = sorted[sorted.length - 1];
  const p50 = sorted[Math.floor(sorted.length * 0.50)];
  const p99 = sorted[Math.floor(sorted.length * 0.99)];

  $latMin.textContent = fmtNum(mn,  1) + 'µs';
  $latP50.textContent = fmtNum(p50, 1) + 'µs';
  $latP99.textContent = fmtNum(p99, 1) + 'µs';
  $latMax.textContent = fmtNum(mx,  1) + 'µs';

  if (!latChart) return;

  // build histogram
  const range = mx - mn || 1;
  const step  = range / LAT_BUCKETS;
  const counts = new Array(LAT_BUCKETS).fill(0);
  s.forEach(v => {
    let b = Math.floor((v - mn) / step);
    if (b >= LAT_BUCKETS) b = LAT_BUCKETS - 1;
    counts[b]++;
  });
  const labels = counts.map((_, i) => fmtNum(mn + i * step, 1));
  latChart.data.labels                = labels;
  latChart.data.datasets[0].data     = counts;
  latChart.update('none');
}

/* ─────────────────────── HELPERS ─────────────────────── */
function fmtNum(n, dec) {
  if (n === undefined || n === null || isNaN(n)) return '—';
  return Number(n).toLocaleString(undefined, {
    minimumFractionDigits: dec,
    maximumFractionDigits: dec,
  });
}

function fmtTime(ts_ns) {
  const ms = ts_ns / 1e6;
  const d  = new Date(ms);
  return d.toLocaleTimeString([], { hour12: false,
    hour:'2-digit', minute:'2-digit', second:'2-digit' }) +
    '.' + String(d.getMilliseconds()).padStart(3,'0');
}

/* ─────────────────────── REPLAY ENGINE ─────────────────────── */
async function startReplay(fileUrl) {
  setPill('connecting', 'FETCHING…');
  let text;
  try {
    const resp = await fetch(fileUrl);
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    text = await resp.text();
  } catch (e) {
    setPill('error', 'FETCH FAILED');
    console.error('Replay fetch error:', e);
    return;
  }

  const lines = text.split('\n').map(l => l.trim()).filter(l => l.length > 0);
  const records = [];
  lines.forEach((l, i) => {
    try { records.push(JSON.parse(l)); }
    catch(e) { console.warn(`JSONL line ${i+1} parse error:`, e); }
  });
  if (records.length === 0) {
    setPill('error', 'EMPTY FILE');
    return;
  }

  $modeBadge.textContent =
    `REPLAY  ·  ${records.length} records  ·  ${SPEED}× speed  ·  ${fileUrl}`;
  setPill('replay', 'REPLAY');

  // Sort by ts_ns to be safe
  records.sort((a, b) => (a.ts_ns < b.ts_ns ? -1 : a.ts_ns > b.ts_ns ? 1 : 0));

  async function runLoop() {
    const t0_real = performance.now();
    const t0_rec  = records[0].ts_ns;

    for (let i = 0; i < records.length; i++) {
      const rec         = records[i];
      const recElapsed  = (rec.ts_ns - t0_rec) / 1e6;          // ms in record-time
      const realElapsed = (performance.now() - t0_real) * SPEED; // ms in record-time

      if (recElapsed > realElapsed) {
        // need to wait
        const waitMs = (recElapsed - realElapsed) / SPEED;
        await sleep(waitMs);
      }
      processRecord(rec);
    }

    if (!LOOP) {
      // Play once and HOLD the completed session on screen — no destructive
      // wipe. Add ?loop=1 to repeat instead.
      setPill('replay', 'COMPLETE');
      $modeBadge.textContent =
        `REPLAY  ·  ${records.length} records  ·  complete (add ?loop=1 to repeat)  ·  ${fileUrl}`;
      return;
    }

    // ?loop=1: restart after a brief pause with a clean slate.
    await sleep(1500);
    state.symbols    = [];
    state.symIndex   = {};
    state.stats      = {};
    state.priceHist  = {};
    state.fills      = [];
    state.latSamples = [];
    state.meta       = null;
    if (priceChart) { priceChart.data.datasets = []; priceChart.update('none'); }
    $chartLegend.innerHTML = '';
    $cardsRow.innerHTML    = '';
    $fillsTbody.innerHTML  = '';
    runLoop();
  }

  runLoop();
}

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, Math.max(0, ms)));
}

/* ─────────────────────── LIVE WEBSOCKET ─────────────────────── */
function startLive(wsUrl) {
  $modeBadge.textContent = `LIVE  ·  ${wsUrl}`;
  setPill('connecting', 'CONNECTING…');

  function connect() {
    let ws;
    try {
      ws = new WebSocket(wsUrl);
    } catch (e) {
      setPill('error', 'WS ERROR');
      console.error('WebSocket error:', e);
      return;
    }

    ws.onopen  = () => setPill('live', 'LIVE');
    ws.onclose = () => {
      setPill('connecting', 'RECONNECTING…');
      setTimeout(connect, 3000);
    };
    ws.onerror = () => setPill('error', 'WS ERROR');
    ws.onmessage = evt => {
      try {
        const rec = JSON.parse(evt.data);
        processRecord(rec);
      } catch(e) {
        console.warn('WS parse error:', e);
      }
    };
  }
  connect();
}

/* ─────────────────────── BOOTSTRAP ─────────────────────── */
window.addEventListener('DOMContentLoaded', () => {
  // Show CDN fallback message if Chart.js not loaded
  if (typeof Chart === 'undefined') {
    const containers = ['chart-container', 'lat-hist-container'];
    containers.forEach(id => {
      const el = document.getElementById(id);
      if (el) {
        el.innerHTML = '<div class="chart-fallback">Chart.js unavailable — data visible in tables above</div>';
      }
    });
  }

  initCharts();
  renderCards();
  renderFillsTable();

  if (MODE === 'live') {
    // Restore the prior session (if this is a refresh) before streaming resumes.
    if (loadState()) rebuildFromState();
    startLive(WS_URL);
  } else {
    startReplay(FILE_URL);
  }
});
