# orderBookC++

A low-latency limit-order book and matching engine in modern C++23.
Designed for single-instrument, single-thread hot-path performance with a
research-backed feature set: price-time FIFO, Limit / Market / IOC / FOK,
cancels, and a Disruptor-style SPSC ring buffer for feeding the engine from
another thread.

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
./build/demo                       # tiny scripted example
./build/test_order_book            # GoogleTest correctness suite
./build/bench_throughput           # Google Benchmark
```

## Layout

```
include/order_book/
  cache.hpp            kCacheLine, OB_LIKELY/UNLIKELY, OB_PREFETCH, OB_ALWAYS_INLINE
  types.hpp            POD Order, Side, OrderType, Trade
  pool_allocator.hpp   Fixed-size object pool (free-list, no malloc on hot path)
  spsc_queue.hpp       Wait-free SPSC ring buffer (cache-line-aligned indices)
  price_level.hpp      Intrusive FIFO at one price (links live inside Order)
  order_book.hpp       SideBook + OrderBook + matching engine declarations
src/order_book.cpp     Matching engine implementation
src/main.cpp           Demo program
tests/                 GoogleTest correctness tests
bench/                 Google Benchmark throughput tests
```

## Design choices, with the research that drove them

### 1. Intrusive FIFO at each price level
Each `Order` carries its own `prev/next` pointers — no per-order list-node
allocation, and traversal is one cache miss per maker instead of two
(node + payload). Standard practice in HFT order books, see Sourav Ghosh's
*Building Low Latency Applications with C++* (Packt, IEEE Xplore #10251189).

### 2. `std::vector`-of-levels instead of `std::map`
For a single instrument, the active band of price levels is small (tens to a
few thousand). A sorted contiguous vector outperforms `std::map` on every
operation that matters here:

- Best price is `levels_.back()` — O(1), and stays in cache because that's
  where most matching happens.
- `lower_bound` over a few hundred contiguous `PriceLevel`s is faster than
  a red-black tree traversal (no per-node allocation, prefetcher-friendly).

This is essentially `std::flat_map` (C++23) hand-rolled to expose iteration.

### 3. Pool allocator for `Order`
Every `Order` lives in a pre-sized `ObjectPool<Order>`. Acquire/release is
free-list O(1); the hot path never calls `malloc`. Capacity is set at
construction so all memory is touched up front (cache warming —
arXiv:2309.04259 reports cache warming and `constexpr` as the highest-impact
techniques in their experiments).

### 4. SPSC ring buffer (Disruptor-style)
`include/order_book/spsc_queue.hpp` implements a bounded wait-free SPSC queue
following the rigtorp pattern (an evolution of the LMAX Disruptor referenced
in arXiv:2309.04259):

- Producer and consumer indices live on separate cache lines (`alignas(kCacheLine)`)
  to avoid false sharing.
- Each side caches the *other's* index locally so the hot path doesn't load
  the remote atomic when there's clearly room / data.
- Capacity rounded up to a power of two — index modulo becomes a bit-mask.

This lets a feed-handler thread push order events to the matching thread
without locks or kernel involvement, and was the highest-throughput pattern
reported in the arXiv paper.

### 5. C++23 features actually used
- `std::span` for read-only views of price-level storage (FOK dry-run scan).
- `std::hardware_destructive_interference_size` via `<new>` (C++17 added,
  C++23 well-supported) for the cache-line constant.
- `[[nodiscard]]` on push/pop and `acquire` so dropped capacity isn't a
  silent bug.
- `if (OB_UNLIKELY(...))` macros wrapping `__builtin_expect` — a
  conservative cousin of the *semi-static conditions* technique described
  in Chunawala-style talks and the ScienceDirect paper
  "Semi-static conditions in low-latency C++ for high frequency trading"
  (10.1016/j.jpdc.2024.103022).

### 6. Things deliberately NOT done (yet)
- **Array-indexed price levels** (one slot per tick + bitmap for best-price
  scan). This wins when the price range is bounded; for unconstrained ranges
  the `std::vector` fallback is correct. Add this as a second `SideBook`
  implementation behind a strategy template.
- **Multi-symbol, multi-threaded matching**. Shard one engine per symbol,
  pin to a core, deliver via SPSC. Out of scope for the v1 single-instrument
  design.
- **Self-trade prevention, iceberg, stop, post-only**.
- **NUMA-aware allocation, huge pages, `mlock`, kernel-bypass NIC**. These
  are the next 10× and require host-specific tuning.

## Research / further reading

- **arXiv:2309.04259** — *C++ Design Patterns for Low-latency Applications
  Including High-frequency Trading*. Lock-free queue (Disruptor), cache
  warming, `constexpr`, statistical benchmarking methodology.
- **ScienceDirect S0743731524001643** — *Semi-static conditions in
  low-latency C++ for high frequency trading: Better than branch prediction
  hints*.
- **Meeting C++ 2025** — Quasar Chunawala, *Designing an SPSC Lock-free
  Queue*. Memory-order deep dive.
- **C++Online 2025** — Sarthak Sehgal, *Optimizing SPSC Lockfree Queue*.
- **rigtorp/SPSCQueue** — reference C++11 implementation, faster than
  `boost::lockfree::spsc` and `folly::ProducerConsumerQueue`.
- **Sourav Ghosh** — *Building Low Latency Applications with C++*, Packt
  (IEEE Xplore #10251189).
- **CppCon 2025** — *Contemporary C++ for Low-Latency Systems* class.

## Sources

- [C++ Design Patterns for Low-latency Applications](https://arxiv.org/abs/2309.04259)
- [Semi-static conditions in low-latency C++ for HFT](https://www.sciencedirect.com/science/article/pii/S0743731524001643)
- [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue)
- [Meeting C++ 2025 — Designing an SPSC Lock-free Queue](https://meetingcpp.com/mcpp/schedule/talkview.php?th=f91eee2a5ca4f23792b67f4ad37c90f2bdcf8a59)
- [Contemporary C++ for Low-Latency Systems 2025 — CppCon](https://cppcon.org/class-2025-low-latency/)
- [Building Low Latency Applications with C++ — IEEE Xplore](https://ieeexplore.ieee.org/document/10251189/)
