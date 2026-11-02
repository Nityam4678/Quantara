from fastapi import FastAPI, Query, Request
from fastapi.responses import PlainTextResponse
from fastapi.responses import StreamingResponse
import asyncio
import json
import logging
import secrets
import time

from .config import settings
from .models import (AgentRequest, AgentResponse, BenchmarkRunRequest,
                     PerformanceAnalysisRequest, ChatRequest, ChatResponse)
from .rag import CodebaseRAG
from .store import MemoryStore, PostgresStore
from .evaluation import evaluate
from .agent import InvestigationAgent
from .benchmark import BenchmarkRunner, analyze_performance

app = FastAPI(title="Quantara Codebase RAG", version="0.3.0")
try:
    from opentelemetry.instrumentation.fastapi import FastAPIInstrumentor
    FastAPIInstrumentor.instrument_app(app)
except ImportError:
    pass
logging.basicConfig(level=getattr(logging, settings.log_level.upper(), logging.INFO),
                    format='{"level":"%(levelname)s","message":"%(message)s"}')
logger = logging.getLogger("quantara.ai")
request_count = 0
request_latency_ms = 0.0
store = PostgresStore(settings.database_url) if settings.database_url else MemoryStore()
if hasattr(store, "initialize"):
    store.initialize()
rag = CodebaseRAG(settings, store=store)
agent = InvestigationAgent(settings, rag, store)
benchmark_runner = BenchmarkRunner(settings, store)


@app.middleware("http")
async def hardening_middleware(request: Request, call_next):
    global request_count, request_latency_ms
    if settings.internal_service_token and request.url.path not in {"/health", "/metrics"}:
        supplied = request.headers.get("x-internal-token", "")
        if not secrets.compare_digest(supplied, settings.internal_service_token):
            return PlainTextResponse("internal authentication required", status_code=401)
    started = time.perf_counter()
    response = await call_next(request)
    elapsed = (time.perf_counter() - started) * 1000
    request_count += 1
    request_latency_ms += elapsed
    logger.info("request method=%s path=%s status=%s duration_ms=%.3f",
                request.method, request.url.path, response.status_code, elapsed)
    response.headers["X-Request-Duration-Ms"] = f"{elapsed:.3f}"
    return response


@app.get("/health")
def health() -> dict:
    return {"ok": True, "chunks": store.count(), "repository": str(settings.repository_root)}


@app.get("/metrics", response_class=PlainTextResponse)
def metrics():
    average = request_latency_ms / request_count if request_count else 0
    return (f"quantara_ai_requests_total {request_count}\n"
            f"quantara_ai_request_latency_average_ms {average}\n"
            f"quantara_ai_benchmark_records {len(store.list_benchmarks())}\n")


@app.get("/retrieval")
def retrieval(q: str = Query(min_length=1),
              top_k: int = Query(default=6, ge=1, le=50),
              strategy: str = Query(default="hybrid"),
              path_prefix: str | None = None):
    results = rag.retrieve(q, top_k, path_prefix, strategy)
    return {"strategy": strategy, "results": [
        {"path": chunk.path, "startLine": chunk.start_line, "endLine": chunk.end_line,
         "symbol": chunk.symbol, "score": score}
        for chunk, score in results
    ]}


@app.post("/evaluate")
def run_evaluation(k: int = Query(default=settings.evaluation_k, ge=1, le=20)):
    report = evaluate(rag, k)
    store.save_evaluation(report.model_dump())
    return report


@app.post("/benchmarks/run")
def run_benchmark(request: BenchmarkRunRequest):
    return benchmark_runner.run(request)


@app.get("/benchmarks")
def benchmarks(limit: int = Query(default=100, ge=1, le=500)):
    return {"results": store.list_benchmarks(limit)}


@app.post("/benchmarks/analyze")
def benchmark_analysis(request: PerformanceAnalysisRequest):
    return analyze_performance(request.question, store, rag)


@app.post("/ingest")
def ingest():
    return rag.ingest()


@app.post("/chat", response_model=ChatResponse)
def chat(request: ChatRequest):
    return rag.answer(request.question, request.top_k)


@app.post("/agent", response_model=AgentResponse)
def investigate(request: AgentRequest):
    return agent.investigate(request.question, request.approved, request.scenario)


@app.post("/agent/stream")
async def investigate_stream(request: AgentRequest):
    result = agent.investigate(request.question, request.approved, request.scenario)

    async def events():
        yield f"event: summary\ndata: {json.dumps(result.reasoning_summary)}\n\n"
        for tool in result.tools:
            yield f"event: tool\ndata: {tool.model_dump_json()}\n\n"
        yield f"event: result\ndata: {result.model_dump_json()}\n\n"
        yield "event: done\ndata: {}\n\n"

    return StreamingResponse(events(), media_type="text/event-stream",
                             headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@app.post("/chat/stream")
async def chat_stream(request: ChatRequest):
    result = rag.answer(request.question, request.top_k)

    async def events():
        yield f"event: metadata\ndata: {json.dumps({'grounded': result.grounded, 'citations': [citation.model_dump() for citation in result.citations]})}\n\n"
        for word in result.answer.split(" "):
            yield f"event: token\ndata: {json.dumps({'text': word + ' '})}\n\n"
            await asyncio.sleep(0)
        yield "event: done\ndata: {}\n\n"

    return StreamingResponse(events(), media_type="text/event-stream",
                             headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@app.get("/search")
def search(q: str = Query(min_length=1), top_k: int = Query(default=6, ge=1, le=20),
           path_prefix: str | None = None):
    return {"results": [
        {"path": chunk.path, "startLine": chunk.start_line, "endLine": chunk.end_line,
         "symbol": chunk.symbol, "className": chunk.class_name,
         "namespace": chunk.namespace, "declaration": chunk.declaration,
         "chunkKind": chunk.chunk_kind,
         "snippet": chunk.content[:1200], "score": score}
        for chunk, score in rag.retrieve(q, top_k, path_prefix)
    ]}
