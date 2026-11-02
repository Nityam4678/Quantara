# Quantara: Low-Latency Trading Infrastructure with RAG-Powered Market Intelligence

Phase 2 is a React/Vite/Tailwind trading terminal backed by the existing
Express gateway and native C++ adapter. It displays actual order-book state and
updates from the engine SSE stream; it does not generate synthetic market data.

## Windows development

From the repository root:

```powershell
Set-Location services\api-gateway
npm install

Set-Location ..\..\frontend
npm install
npm run dev
```

Open `http://127.0.0.1:5173`. Vite proxies `/api` requests to
`http://127.0.0.1:3000`.

The API gateway and native adapter must be running first. See
`services/api-gateway/README.md` for C++ build and gateway startup commands.

## Production build

```powershell
npm run build
npm run preview
```

The terminal provides live engine metrics, bid/ask depth and spread, limit
order entry, resting-order cancellation, native execution history, native
event activity, and SSE reconnect behavior.

The current frontend keeps session order rows in browser memory. Durable order
history, authentication, and multi-instrument routing are later phases.
