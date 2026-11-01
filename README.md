# Low-Latency Limit Order Book Matching Engine

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![GCC 15.2](https://img.shields.io/badge/GCC-15.2-A42E2B?logo=gnu&logoColor=white)
![Tests](https://img.shields.io/badge/tests-3%2F3%20passing-brightgreen)

A price-time priority matching engine written in modern C++20, built for **deterministic
latency** rather than peak throughput alone. The hot path performs no heap allocation, no
locking, and no tree traversal — an order add or cancel is a handful of pointer swings
against pre-allocated memory.

**21.4 M events/sec · 46.7 ns average latency · 70 ns p50 add · 50 ns p50 cancel**

---

## Overview

The engine implements a single-instrument limit order book with full price-time priority:

- **Limit orders** match against the opposite side as far as the limit price allows, then
  rest whatever quantity remains.
- **Price priority across levels** — an aggressive order sweeps the book from the touch
  outwards, producing one fill per level at *each maker's* price, exactly as an exchange
  would print it.
- **Time priority within a level** — strict FIFO. A partially filled resting order keeps
  its position at the front of the queue rather than losing priority.
- **Cancels** are located by order ID and spliced out without traversing the level.

Everything runs on one thread and one core by design. Sharing a book across threads would
reintroduce the atomics the architecture exists to avoid.

---

## Performance Benchmarks

2,000,000 pre-generated events replayed through the engine. Event generation, warm-up, and
all accounting happen outside the timed region.

### Headline numbers

| Workload | Events | Throughput | Avg latency | Notes |
|---|---:|---:|---:|---|
| **Crossing flow** | 2,000,000 | **21,398,064 /sec** (21.40 M/s) | **46.7 ns** | Compute-bound; book stays thin |
| **Deep book** | 2,000,000 | 6,073,203 /sec (6.07 M/s) | 164.7 ns | Memory-bound; 49.6 MB working set |

### Latency distribution — crossing flow

| Operation | p50 | p90 | p99 |
|---|---:|---:|---:|
| **Add order** | 70 ns | 100 ns | 160 ns |
| **Cancel order** | 50 ns | 60 ns | 70 ns |

Matching is real work, not an empty book: **1,187,956 trades executed**, averaging
**0.74 fills per add**. 100% of cancel messages target a genuinely resting order.

### Impact of the flat-array refactor

Price levels were originally held in `std::map`. Replacing the tree with a directly indexed
flat array, measured on the same machine and build configuration:

| Workload | `std::map` | Flat array | Speedup |
|---|---:|---:|---:|
| Crossing flow | 68.6 ns/event | 46.7 ns/event | **1.47×** |
| Deep book | 171.6 ns/event | 164.7 ns/event | ~1.0× (within noise) |

The deep-book result is the more instructive one. With 812,451 resting orders the working
set is **49.6 MB** — far past L3 — so every cancel is a hash probe plus a random pointer
chase into a 64-byte `Order`. The level lookup was never the bottleneck there; removing it
changed a ~5 ns component of a ~480 ns operation. Optimisations were kept only where
measurement justified them.

### Methodology

Two independent passes are run per workload:

- **Bulk pass** — a single `steady_clock` reading either side of the whole replay loop, with
  no per-event instrumentation. This produces the throughput and average latency figures.
- **Instrumented pass** — one `RDTSC` pair per message to build the latency distribution.

The `RDTSC` pair costs ~20 ns on the test machine. That overhead is measured at startup,
reported by the benchmark, and left **inside** the percentile figures rather than subtracted,
so the percentile table is conservative.

> **Test environment:** GCC 15.2.0, `-O2 -DNDEBUG -march=native`, x86-64 @ 3.19 GHz (calibrated
> TSC), Windows 11. Single un-pinned run at unlocked clock frequency — p99.99 and max figures
> are dominated by OS scheduling, not by the engine.

---

## Architecture Highlights

### 1. Zero dynamic allocation on the hot path

A `OrderPool` carves out its entire slab at start-up and never calls the allocator again.
`allocate()` and `deallocate()` are pointer swings against an **intrusive free list** — a
free `Order` stores the next free one in its own `next` field, which costs no extra memory
and, more importantly, no extra cache miss: the line touched to pop the list is the same line
the caller is about to write into.

Compiled at `-O2 -DNDEBUG`, the entire allocator is:

```
allocate:    mov / test / je / mov / pxor / mov / movups / sub / ret
deallocate:  mov / mov / mov / add / mov / ret
```

No calls, no locks, no branches beyond a single exhaustion check. A `prefault()` pass touches
one byte per page at start-up so the first million allocations don't each pay a soft page
fault. Pool exhaustion returns `nullptr` and rejects the order rather than growing — growing
would mean a `malloc` and a multi-microsecond spike mid-session.

### 2. O(1) add, match, and cancel

- **Intrusive doubly-linked lists.** Each price level's FIFO queue runs through `Order::prev`
  and `Order::next`. There is no node allocation and no indirection — `std::list` would
  allocate a node per order and place the payload a pointer hop from the links.
- **Order ID → `Order*` index** makes a cancel O(1) to locate.
- **Level back-pointer.** Each `Order` carries a `PriceLevel*` so a cancel needs no price
  lookup at all. It occupies bytes that were already structure padding, so `sizeof(Order)`
  is unchanged. This makes `PriceLevel` a *pinned* type — its move constructor is deleted so
  the compiler enforces the invariant rather than leaving it to a comment.
- **Flat-array price levels.** `std::array<PriceLevel, 100'000>` per side, indexed directly
  by integer tick. Locating a level is one shift and one add — no tree descent, no hashing,
  and no allocation the first time a price is touched.
- **Cached top of book.** `best_bid` / `best_ask` are maintained incrementally and repaired
  by a bounded linear scan only when the touch level empties. A level counter short-circuits
  a fully drained side straight to its sentinel instead of scanning to the end of the array —
  measured at **1.55×** on the crossing-flow workload.

### 3. Cache-line efficiency

| Type | Size | Rationale |
|---|---:|---|
| `Order` | 64 B | Exactly one cache line, `alignas(64)`. Walking a level's FIFO costs one miss per hop, never two. |
| `PriceLevel` | 32 B | Two levels per cache line, halving the cost of the best-price scan. |
| `Event` (benchmark) | 24 B | Replaying the stream is a linear scan the prefetcher handles perfectly. |

All three sizes are locked in with `static_assert`, so a future field addition fails the build
instead of silently degrading the layout.

---

## Correctness

Performance claims are only meaningful if the book is right. The engine ships with a full
structural invariant check, `OrderBook::verify()`, which sweeps both 100,000-entry level
arrays end to end and asserts that:

- every level's forward and backward links agree, and its cached `total_volume` and order
  count match a fresh walk of the queue;
- every resting order's side, price, and level back-pointer agree with the slot it occupies;
- the order index holds exactly the resting orders — no leaks, no ghosts;
- the cached `best_bid` / `best_ask` equal what a full array sweep actually finds;
- the book is never crossed once an operation returns.

Three suites exercise the engine, including a randomised fuzz run that re-checks the complete
invariant after individual messages, and a churn test that repeatedly empties and refills the
touch level — the specific failure mode the flat-array design introduces.

```
pool_smoke    Passed
level_smoke   Passed
book_smoke    Passed
```

All suites pass in both release and debug builds, with assertions enabled, and under a
checked-STL build (`-D_GLIBCXX_DEBUG`). The engine compiles warning-free under
`-Wall -Wextra -Wpedantic`.

---

## Project Layout

```
LimitOrderBook/
├── include/lob/
│   ├── order.hpp          # Side, Order (64 B, intrusive links, level back-pointer)
│   ├── order_pool.hpp     # Fixed-capacity slab allocator, intrusive free list
│   ├── price_level.hpp    # Intrusive FIFO queue for one price (32 B)
│   ├── order_map.hpp      # order_id -> Order* index (OrderTracker)
│   └── order_book.hpp     # Flat-array book, matching, top-of-book cache
├── src/
│   ├── order_pool.cpp     # Slab construction, free-list build, prefault
│   ├── price_level.cpp    # Cold-path integrity walk
│   ├── order_book.cpp     # Matching loop, cancel, best-price repair, verify()
│   └── main.cpp           # Benchmark harness
├── tests/
│   ├── pool_smoke.cpp     # Allocator round trip, exhaustion, LIFO reuse
│   ├── level_smoke.cpp    # FIFO ordering, splices, partial fills, churn
│   └── book_smoke.cpp     # Matching, cancels, tick bounds, sentinels, fuzz
└── CMakeLists.txt
```

---

## TradeMind Phase 1 integration

Phase 1 adds a persistent native JSON-lines adapter at
`services/engine-adapter/main.cpp` and an Express gateway at
`services/api-gateway/`. The adapter links to the existing `lob` static
library; the matching algorithm and C++ hot path are unchanged.

See [services/api-gateway/README.md](services/api-gateway/README.md) for the
request/response protocol, REST endpoints, Windows startup commands, and
integration tests.

## TradeMind Phase 2 terminal

Phase 2 adds a React/Vite/Tailwind trading terminal in `frontend/`. It reads
the live Express/native-engine state, submits and cancels real limit orders,
shows depth and executions, and receives engine changes over Server-Sent
Events.

```powershell
Set-Location .\frontend
npm install
npm run dev
```

The Vite development server proxies `/api` to the Express gateway at
`http://127.0.0.1:3000`. Build the production bundle with
`npm run build`.

## TradeMind Phase 3 codebase assistant

Phase 3 adds a code-aware RAG service in `services/ai-service/`. It indexes
C++ headers and sources, CMake files, tests, and Markdown documentation using
Tree-sitter C++ AST chunks and LangChain Markdown chunks with file paths,
symbols, classes, namespaces, and line ranges. The service preserves complete
function implementations whenever practical and syntax-splits oversized
functions. Markdown uses `MarkdownHeaderTextSplitter` followed by
`RecursiveCharacterTextSplitter` only for oversized sections. All normalized
chunks are LangChain `Document` objects before persistence. The service
supports incremental SHA-256 ingestion, semantic retrieval through a
LangChain retriever adapter, grounded answers with citations, abstention when
evidence is unavailable, and SSE chat streaming. The frontend exposes the
assistant in the terminal, while Express proxies the AI routes:

| Method | Express route | Purpose |
|---|---|---|
| GET | `/api/ai/health` | AI service health and indexed chunk count |
| POST | `/api/ai/ingest` | Scan and incrementally index the repository |
| GET | `/api/ai/search?q=...` | Retrieve source snippets |
| POST | `/api/ai/chat` | Return a grounded answer and citations |
| POST | `/api/ai/chat/stream` | Stream the answer as SSE events |
| GET | `/api/ai/retrieval?q=...&strategy=hybrid` | Compare vector, keyword, hybrid, or reranked retrieval |
| POST | `/api/ai/evaluate` | Run the 30-question retrieval evaluation and persist results |

### Windows local setup

The service runs as a native process; Docker is not required.

```powershell
Set-Location .\services\ai-service
py -3.14 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
$env:REPOSITORY_ROOT = (Resolve-Path ..\..).Path
.\.venv\Scripts\python.exe -m uvicorn app.main:app --host 127.0.0.1 --port 8000
```

For local development, leave `DATABASE_URL` unset. The service then uses the
deterministic in-memory vector store and hash embeddings, which require no
external services. For persistent retrieval, configure PostgreSQL with the
pgvector extension and set:

```powershell
$env:DATABASE_URL = "postgresql://user:password@127.0.0.1:5432/trademind"
$env:EMBEDDING_API_URL = "https://your-provider.example/v1/embeddings"
$env:EMBEDDING_API_KEY = "..."
$env:EMBEDDING_MODEL = "text-embedding-3-small"
$env:EMBEDDING_DIMENSIONS = "1536"
$env:LLM_API_URL = "https://your-provider.example/v1/chat/completions"
$env:LLM_API_KEY = "..."
$env:LLM_MODEL = "..."
$env:CPP_CHUNK_SIZE = "12000"
$env:MARKDOWN_CHUNK_SIZE = "6000"
$env:MARKDOWN_CHUNK_OVERLAP = "200"
$env:RETRIEVAL_VECTOR_WEIGHT = "0.65"
$env:RETRIEVAL_KEYWORD_WEIGHT = "0.35"
$env:RETRIEVAL_CANDIDATE_K = "20"
$env:RERANK_ENABLED = "true"
$env:RERANK_API_URL = ""
$env:RERANK_API_KEY = ""
$env:RERANK_MODEL = ""
$env:EVALUATION_K = "5"
```

Embedding and LLM endpoints use OpenAI-compatible JSON contracts. The local
fallback is intended for development and tests; hosted embeddings and an LLM
are required for provider-backed production answers. When an embedding API key
is configured, `langchain-openai` provides the embedding integration; a
custom OpenAI-compatible URL without a key continues to use the service's
HTTP adapter. Adjust the chunk-size variables and evaluate ingestion/retrieval
quality for the repository before selecting production defaults. Start Express with
`AI_SERVICE_URL=http://127.0.0.1:8000` after the FastAPI service is running.

### Phase 4 retrieval and evaluation

The retrieval layer supports four measured strategies:

- `vector`: pgvector cosine similarity or the local vector store.
- `keyword`: PostgreSQL `tsvector`/GIN full-text search, with a matching
  in-memory implementation for tests.
- `hybrid`: weighted Reciprocal Rank Fusion of vector and keyword rankings.
- `hybrid+rereank`: hybrid candidates reordered by the configured reranker.

The default reranker is a deterministic lexical reranker for offline
development. Configure `RERANK_API_URL`, `RERANK_MODEL`, and optionally
`RERANK_API_KEY` for an external reranking provider. Exact symbols remain in
the keyword query and are not discarded during query expansion.

Run the actual evaluation dataset after ingestion:

```powershell
Invoke-RestMethod -Method Post http://127.0.0.1:8000/evaluate?k=5 |
  ConvertTo-Json -Depth 6
```

The dataset contains 30 repository-specific questions. Results compare
Recall@K, Precision@K, MRR, nDCG, citation correctness, answer
faithfulness, retrieval/generation latency, token count, and estimated cost.
Offline runs report measured zero provider tokens and zero provider cost;
they do not fabricate hosted-provider usage. PostgreSQL deployments persist
reports in `rag_evaluations`. The frontend RAG evaluation panel displays the
latest measured report and does not synthesize metrics.

Run the Phase 3 checks:

```powershell
Set-Location .\services\ai-service
.\.venv\Scripts\python.exe -m pytest -q

Set-Location ..\api-gateway
$env:ENGINE_ADAPTER_PATH = (Resolve-Path ..\..\build\trademind_engine_adapter.exe).Path
$env:AI_SERVICE_URL = "http://127.0.0.1:8000"
npm test
```

The PostgreSQL store creates `rag_documents` and `rag_chunks` and enables the
`vector` extension. Hosted provider credentials and PostgreSQL are intentionally
not required for the deterministic unit-test path.

### Phase 5 investigation agent

The controlled investigation agent is exposed through:

| Method | Route | Purpose |
|---|---|---|
| POST | `/api/ai/agent` | Run a bounded investigation and return tool results |
| POST | `/api/ai/agent/stream` | Stream summary, tool calls, result, and completion events |

The allowlisted tools are `get_order_book`, `get_order_details`,
`get_execution_history`, `get_recent_events`, `run_order_simulation`,
`search_codebase`, and `search_documentation`.

Read tools query the live Express/native-engine API and indexed RAG store.
Simulation accepts only 1–10 validated limit orders, never accepts shell
commands, and requires `approved: true`. Tool calls have argument validation,
timeouts, bounded retries, an iteration limit, and audit records. PostgreSQL
deployments persist audit records in `agent_tool_audit`; local mode retains
the most recent 1,000 records in memory.

```powershell
$body = @{
  question = "Why is order 88001 resting in the actual order book?"
  approved = $false
} | ConvertTo-Json
Invoke-RestMethod -Method Post http://127.0.0.1:3000/api/ai/agent `
  -ContentType "application/json" -Body $body
```

For a state-changing simulation, provide explicit approval and a bounded
scenario:

```powershell
$body = @{
  question = "Simulate two crossing orders and explain the result"
  approved = $true
  scenario = @(
    @{ orderId = 99001; side = "BUY"; price = 90; quantity = 2 },
    @{ orderId = 99002; side = "SELL"; price = 90; quantity = 2 }
  )
} | ConvertTo-Json -Depth 5
Invoke-RestMethod -Method Post http://127.0.0.1:3000/api/ai/agent `
  -ContentType "application/json" -Body $body
```

The frontend investigation panel displays the public reasoning summary, tool
statuses and outputs, retrieved citations, execution results, and approval
state. Private chain-of-thought is not exposed.

### Phase 6 benchmark intelligence

Phase 6 runs the existing `lob_bench` executable unchanged. The runner passes
only the benchmark's supported `--events`, `--seed`, and `--sigma` options,
captures stdout and stderr, rejects incomplete human-readable output, enforces
a timeout, and limits concurrent runs. Each workload record stores the
measured bulk throughput and average latency, per-event percentiles, working
set, compiler/assert configuration, machine metadata, inputs, timestamp, and
raw process output. PostgreSQL stores records in `benchmark_runs`; local
development uses an in-memory history.

Configure the executable and start the AI service:

```powershell
$env:BENCHMARK_EXECUTABLE = (Resolve-Path .\.phase6-build\lob_bench.exe).Path
$env:BENCHMARK_TIMEOUT_SECONDS = "120"
$env:BENCHMARK_MAX_CONCURRENT = "1"
Set-Location .\services\ai-service
python -m uvicorn app.main:app --host 127.0.0.1 --port 8000
```

Run and inspect a measured experiment through the gateway:

```powershell
$body = @{ events = 10000; seed = 42; sigma = 12 } | ConvertTo-Json
Invoke-RestMethod -Method Post http://127.0.0.1:3000/api/ai/benchmarks/run `
  -ContentType "application/json" -Body $body | ConvertTo-Json -Depth 8
Invoke-RestMethod http://127.0.0.1:3000/api/ai/benchmarks?limit=20 |
  ConvertTo-Json -Depth 8
```

The React benchmark panel displays latest workload cards and historical
experiments. Its analyst endpoint combines selected stored measurements with
RAG citations and labels profiling explanations as hypotheses. It never
creates numerical results: numerical claims come from persisted benchmark
records, while proving a bottleneck requires a separate profiling experiment.

## How to Build & Run

Requires a C++20 compiler (GCC 13+, Clang 16+, or MSVC 19.3+) and CMake 3.20+.

### With CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On Windows with MinGW, add the generator:

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
```

Run the test suite:

```bash
ctest --test-dir build --output-on-failure
```

Run the benchmark:

```bash
./build/lob_bench
```

### Directly with GCC

```bash
g++ -std=c++20 -O2 -DNDEBUG -march=native -Iinclude src/*.cpp -o lob_bench
```

```bash
./lob_bench
```

### Benchmark options

| Flag | Default | Description |
|---|---|---|
| `--events=N` | `2000000` | Number of events to generate and replay |
| `--seed=N` | fixed | RNG seed, for reproducible streams |
| `--sigma=F` | `12.0` | Std-dev of the price distribution, in ticks |

```bash
./lob_bench --events=5000000 --sigma=25
```

Build with `-march=native` to reproduce the published numbers, and **always** with `-DNDEBUG`
— the assertion-heavy debug build is roughly 8× slower (361 ns/event vs 47 ns) and is not
representative. The benchmark prints a warning at startup if it detects assertions are
enabled.

---

## Design Notes & Known Limits

Stated plainly, because an engine's constraints matter as much as its numbers:

- **Fixed tick grid.** Prices are integer ticks in `[1, 100'000)`. Off-grid prices are
  rejected, not clamped — with a flat array the bounds check is the only thing between a bad
  feed and a 6 MB out-of-bounds write. The two level arrays cost 6.1 MB per book.
- **Two allocation sites remain.** The `order_id → Order*` index is node-based
  (`std::unordered_map`), so registering a new order ID may allocate; and the trade output
  buffer can grow if a sweep exceeds any depth seen before. Both are reserved up front to push
  the cost out of the session. Replacing the index with an open-addressing table is the next
  meaningful optimisation.
- **Latency is memory-bound at depth.** As the deep-book workload shows, once the resting book
  exceeds L3 the dominant cost is the cache miss on the `Order`, not the engine's algorithms.
  The productive direction is a smaller footprint — 32-bit slot handles instead of 64-bit
  pointers — not a faster index.
- **Sparse books scan linearly.** A wide gap between populated ticks is walked one tick at a
  time. Dense books make this negligible; the fix when it matters is a 12.5 KB occupancy
  bitmap per side scanned with `__builtin_ctzll`.
- **`total_volume` is 32-bit** and can wrap on a level holding more than ~4.29 billion units.
- **Single-threaded by design.** One book, one thread, one core.
- **Not implemented:** market / IOC / FOK order types, order amend, and self-trade prevention.
  These are policy decisions rather than engine mechanics.

---

