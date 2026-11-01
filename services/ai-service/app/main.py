from fastapi import FastAPI, Query
from fastapi.responses import StreamingResponse
import asyncio
import json

from .config import settings
from .models import (AgentRequest, AgentResponse, BenchmarkRunRequest,
                     PerformanceAnalysisRequest, AgentResponse, ChatRequest, ChatResponse)
from .rag import CodebaseRAG
from .store import MemoryStore, PostgresStore
from .evaluation import evaluate
from .agent import InvestigationAgent
from .benchmark import BenchmarkRunner, analyze_performance

app = FastAPI(title="TradeMind Codebase RAG", version="0.3.0")
store = PostgresStore(settings.database_url) if settings.database_url else MemoryStore()
if hasattr(store, "initialize"):
    store.initialize()
rag = CodebaseRAG(settings, store=store)
agent = InvestigationAgent(settings, rag, store)
benchmark_runner = BenchmarkRunner(settings, store)


@app.get("/health")
def health() -> dict:
    return {"ok": True, "chunks": store.count(), "repository": str(settings.repository_root)}


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
