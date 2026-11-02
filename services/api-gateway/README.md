# Quantara API Gateway

Phase 1 exposes the existing C++ `lob` static library through a persistent
native adapter process and an Express API. The matching algorithm and its hot
path are unchanged.

## Protocol

The adapter reads one JSON object per line from stdin and writes one JSON object
per line to stdout. Requests are serialized by the Node client, and every
request includes a generated `requestId`.

Supported adapter requests:

```json
{"requestId":"...","type":"add_order","orderId":1,"side":"BUY","price":100,"quantity":10}
{"requestId":"...","type":"cancel_order","orderId":1}
{"requestId":"...","type":"snapshot","depth":20}
{"requestId":"...","type":"order","orderId":1}
{"requestId":"...","type":"recent_events","limit":100}
{"requestId":"...","type":"health"}
```

The adapter rejects malformed JSON, duplicate fields, unsupported request
types, invalid integers, invalid sides, out-of-range prices, and quantities
that do not fit the C++ `uint32_t` quantity type. Each response is flushed
immediately, and responses never expose C++ pointers or internal addresses.

## HTTP endpoints

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/orders` | Submit a limit order |
| DELETE | `/api/orders/:orderId` | Cancel a resting order |
| GET | `/api/orders/:orderId` | Retrieve a resting order |
| GET | `/api/orderbook?depth=20` | Retrieve top-of-book and depth |
| GET | `/api/executions?limit=100` | Retrieve execution reports received by gateway |
| GET | `/api/events?limit=100` | Retrieve gateway event history |
| GET | `/api/metrics` | Retrieve live order and execution counters |
| GET | `/api/stream` | Server-Sent Events stream for engine changes |
| GET | `/api/health` | Check adapter health and pool usage |
| GET | `/api/ai/health` | Check the FastAPI RAG service |
| POST | `/api/ai/ingest` | Scan and incrementally index the repository |
| POST | `/api/ai/chat` | Ask a grounded codebase question |
| POST | `/api/ai/chat/stream` | Stream a grounded answer as SSE |

Example:

```powershell
Invoke-RestMethod -Method Post http://127.0.0.1:3000/api/orders `
  -ContentType 'application/json' `
  -Body '{"orderId":1,"side":"SELL","price":100,"quantity":10}'
```

The API uses HTTP 400 for invalid client input, 404 for a non-resting order,
503 when the adapter is unavailable, and 504 for adapter timeouts. Engine
business outcomes such as duplicate IDs and pool exhaustion are returned as
normal execution reports because they are valid matching-engine responses.

## Windows setup and startup

From the repository root:

```powershell
$Build = "$PWD\build-phase1"
cmake -S . -B $Build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build $Build --parallel

Set-Location services\api-gateway
npm install
$env:ENGINE_ADAPTER_PATH = "$PWD\..\..\build-phase1\quantara_engine_adapter.exe"
$env:ENGINE_CAPACITY = "1000000"
$env:PORT = "3000"
npm start
```

The adapter is a child process of the gateway. If it exits, pending and future
requests receive an explicit `ENGINE_UNAVAILABLE` error rather than hanging.

The frontend consumes `/api/stream` as an SSE connection. Each `engine` event
contains the native adapter sequence, operation, execution report, trades, and
gateway receipt timestamp. The stream sends heartbeats and the frontend
reconnects after a connection error.

## Tests

```powershell
# Original C++ tests
ctest --test-dir ..\..\build-phase1 --output-on-failure

# Express-to-native integration tests
npm test
```

## Known limitations

- The adapter is single-instrument and single-threaded, matching the current
  `OrderBook` contract.
- Events and execution reports are retained in process memory only.
- The gateway does not yet persist events; Phase 2 uses SSE for browser updates.
- The gateway uses an in-memory recent-event view; Phase 2 must add durable
  persistence and asynchronous processing.
- The adapter intentionally supports only the existing limit-order and cancel
  operations. Market, IOC, FOK, amend, and self-trade prevention are not added.
- JSON parsing is intentionally limited to the flat scalar request schema. No
  arbitrary nested JSON is accepted by the native protocol.
