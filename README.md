# TradeMind

TradeMind is a Windows-native trading intelligence platform built around a
low-latency C++20 limit order book. The original matching engine remains the
source of truth for order processing; the surrounding services provide an API,
trading terminal, codebase RAG, controlled investigation tools, and benchmark
analysis.

## What it provides

- Price-time priority limit-order matching.
- Fixed-capacity order pool and flat-array price levels.
- Intrusive FIFO queues and order-ID lookup for fast cancellation.
- Native C++ engine adapter using newline-delimited JSON.
- Express API gateway for orders, cancellations, book snapshots, executions,
  events, benchmarks, authentication, and metrics.
- React/Vite/Tailwind trading terminal with live server-sent events.
- FastAPI codebase assistant using Tree-sitter C++ parsing, LangChain
  documents, hybrid retrieval, citations, and incremental ingestion.
- Controlled AI investigation tools for live engine state and approved
  simulations.
- Benchmark execution, parsing, storage, comparison, and performance analysis.
- Native PowerShell lifecycle scripts. Docker is not required or supported.

## Architecture

```text
React/Vite frontend
        │ REST + SSE
        ▼
Express API gateway ──────► FastAPI AI service
        │                         │
        ▼                         ├─ RAG index
Native C++ adapter                ├─ optional PostgreSQL/pgvector
        │                         └─ optional Redis worker
        ▼
      lob::OrderBook
```

The matching hot path is single-threaded and contains no LLM calls. AI
features are isolated from order matching and may use live engine data only
through controlled backend tools.

## Requirements

- Windows 11 and PowerShell 5.1 or 7+
- CMake 3.20+
- C++20 compiler (MinGW/GCC, Clang, or MSVC)
- Node.js 20+
- Python 3.11+
- PostgreSQL with pgvector and Redis are optional for local development and
  required for persistent production-style deployments
- An OpenAI-compatible embedding and chat provider is optional; local
  deterministic fallbacks support development and tests

## Installation

From the repository root:

```powershell
Set-Location C:\Users\ADMIN\Desktop\Projects\LimitOrderBook
.\scripts\setup.ps1
```

The setup script installs JavaScript dependencies, creates the Python
environment, and builds the native targets in `.trademind-build`.

Copy and review the environment templates if needed:

```powershell
Copy-Item .env.example .env
Copy-Item services\api-gateway\.env.example services\api-gateway\.env
Copy-Item services\ai-service\.env.example services\ai-service\.env
```

Do not commit real secrets. For local development, authentication is disabled
by default and the AI service can run without PostgreSQL, Redis, or provider
API keys.

## Start, check, and stop

```powershell
Set-Location C:\Users\ADMIN\Desktop\Projects\LimitOrderBook
.\scripts\start-all.ps1
.\scripts\health-check.ps1
```

Open the terminal at:

```text
http://127.0.0.1:5173
```

Stop only processes started by TradeMind:

```powershell
.\scripts\stop-all.ps1
```

Logs and PID files are stored in `.trademind\`.

The services use these local endpoints:

| Service | URL |
|---|---|
| Frontend | `http://127.0.0.1:5173` |
| Express gateway | `http://127.0.0.1:3000` |
| FastAPI AI service | `http://127.0.0.1:8000` |

## Build and test

Build the native engine:

```powershell
cmake -S . -B .trademind-build -DCMAKE_BUILD_TYPE=Release
cmake --build .trademind-build --parallel
```

Run the C++ tests:

```powershell
ctest --test-dir .trademind-build --output-on-failure
```

Run gateway tests:

```powershell
Set-Location services\api-gateway
$env:ENGINE_ADAPTER_PATH = (Resolve-Path ..\..\.trademind-build\trademind_engine_adapter.exe).Path
npm test
```

Run AI tests:

```powershell
Set-Location ..\ai-service
python -m pytest -q
```

Build the frontend:

```powershell
Set-Location ..\..\frontend
npm run build
```

## API examples

Health:

```powershell
Invoke-RestMethod http://127.0.0.1:3000/api/health
```

Order book:

```powershell
Invoke-RestMethod http://127.0.0.1:3000/api/orderbook
```

Place a limit order:

```powershell
$order = @{
  orderId = 1001
  side = "BUY"
  price = 100
  quantity = 5
} | ConvertTo-Json

Invoke-RestMethod -Method Post `
  -Uri http://127.0.0.1:3000/api/orders `
  -ContentType application/json `
  -Body $order
```

Cancel an order:

```powershell
Invoke-RestMethod -Method Delete `
  http://127.0.0.1:3000/api/orders/1001
```

Other gateway routes include:

```text
GET    /api/orders/:id
GET    /api/executions
GET    /api/events
GET    /api/health
GET    /api/ai/health
POST   /api/ai/ingest
GET    /api/ai/search?q=...
POST   /api/ai/chat
POST   /api/ai/chat/stream
GET    /api/ai/retrieval?q=...&strategy=hybrid
POST   /api/ai/agent
POST   /api/ai/benchmarks/run
GET    /api/ai/benchmarks
```

## Codebase RAG assistant

Index the repository:

```powershell
Invoke-RestMethod -Method Post `
  http://127.0.0.1:3000/api/ai/ingest | ConvertTo-Json
```

Ask a grounded question:

```powershell
$request = @{
  question = "How does price-time priority work?"
  top_k = 5
} | ConvertTo-Json

Invoke-RestMethod -Method Post `
  -Uri http://127.0.0.1:3000/api/ai/chat `
  -ContentType application/json `
  -Body $request | ConvertTo-Json -Depth 10
```

The index includes C++ source and headers, tests, CMake files, and Markdown.
C++ is parsed with Tree-sitter into function-, class-, namespace-, and
file-aware chunks. Markdown uses heading-aware splitting. Each result keeps
file paths, symbols, and line ranges for citations. Ingestion uses SHA-256
hashes so unchanged files are not embedded again.

Retrieval supports:

- `vector`
- `keyword`
- `hybrid`
- `hybrid+rereank`

Without provider credentials, local deterministic embeddings and a grounded
fallback answer are used. Configure `LLM_API_URL`, `LLM_API_KEY`,
`LLM_MODEL`, `EMBEDDING_API_URL`, `EMBEDDING_API_KEY`, and
`EMBEDDING_MODEL` for provider-backed answers. Set `DATABASE_URL` to enable
PostgreSQL/pgvector persistence.

Run the retrieval evaluation:

```powershell
Invoke-RestMethod -Method Post `
  "http://127.0.0.1:3000/api/ai/evaluate?k=5" |
  ConvertTo-Json -Depth 10
```

## Benchmarking

The original `lob_bench` methodology is preserved. Run it directly:

```powershell
.\.trademind-build\lob_bench.exe
.\.trademind-build\lob_bench.exe --events=5000000 --seed=42 --sigma=25
```

Supported options are `--events`, `--seed`, and `--sigma`. The AI service can
run the executable through a controlled process, parse its human-readable
output, enforce timeouts, limit concurrency, and store measured results.

## Configuration and security

Configuration is environment-based. Important settings include:

- `ENGINE_ADAPTER_PATH`
- `AI_SERVICE_URL`
- `DATABASE_URL`
- `REDIS_URL`
- `LLM_API_URL`, `LLM_API_KEY`, `LLM_MODEL`
- `EMBEDDING_API_URL`, `EMBEDDING_API_KEY`, `EMBEDDING_MODEL`
- `AUTH_REQUIRED`, `JWT_SECRET`, `AUTH_PASSWORD`
- `INTERNAL_SERVICE_TOKEN`
- `CORS_ORIGINS`

The gateway supports optional HS256 JWT authentication, input validation,
rate limiting, CORS allowlisting, request timeouts, and internal service
authentication. AI investigation tools are allowlisted, bounded, validated,
and never execute arbitrary shell commands. State-changing simulations require
explicit approval.

## Known limitations

- The engine currently supports limit orders only; market, IOC, FOK, amend,
  and self-trade prevention are not implemented.
- The engine is intentionally single-threaded and uses a fixed price grid and
  fixed-capacity order pool.
- PostgreSQL/pgvector and Redis persistence require separate native Windows
  installations.
- OpenTelemetry export and Grafana dashboards require external collector and
  dashboard configuration.
- Local AI fallback is grounded retrieval, not a hosted LLM.
- The project is not declared production-ready until live database, Redis,
  authentication, restart, persistence, and provider integrations are tested
  in the target environment.

## Troubleshooting

Check service health:

```powershell
.\scripts\health-check.ps1
```

If the native adapter is missing, rebuild:

```powershell
cmake -S . -B .trademind-build -DCMAKE_BUILD_TYPE=Release
cmake --build .trademind-build --parallel
```

If a previous run is stuck:

```powershell
.\scripts\stop-all.ps1
.\scripts\start-all.ps1
```

Inspect logs:

```powershell
Get-Content .\.trademind\api-gateway.log -Tail 100
Get-Content .\.trademind\api-gateway.log.err -Tail 100
Get-Content .\.trademind\ai-service.log -Tail 100
Get-Content .\.trademind\frontend.log -Tail 100
```
