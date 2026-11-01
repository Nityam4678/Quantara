from pydantic import BaseModel, Field


class Chunk(BaseModel):
    chunk_id: str
    path: str
    start_line: int
    end_line: int
    symbol: str | None = None
    class_name: str | None = None
    namespace: str | None = None
    parent_symbol: str | None = None
    declaration: str | None = None
    chunk_kind: str = "section"
    language: str
    content: str
    content_hash: str
    version: int = 1


class IngestResponse(BaseModel):
    scanned: int
    changed: int
    unchanged: int
    chunks: int
    embedded: int


class ChatRequest(BaseModel):
    question: str = Field(min_length=1, max_length=4000)
    conversation: list[dict[str, str]] = Field(default_factory=list)
    top_k: int | None = Field(default=None, ge=1, le=20)


class Citation(BaseModel):
    path: str
    start_line: int
    end_line: int
    symbol: str | None = None
    snippet: str
    score: float


class ChatResponse(BaseModel):
    answer: str
    citations: list[Citation]
    grounded: bool
    uncertainty: str | None = None
    retrieval_strategy: str = "hybrid"
    retrieval_latency_ms: float | None = None
    generation_latency_ms: float | None = None


class AgentRequest(BaseModel):
    question: str = Field(min_length=1, max_length=4000)
    approved: bool = False
    scenario: list[dict] | None = None


class ToolCall(BaseModel):
    name: str
    arguments: dict
    status: str
    duration_ms: float
    output: object | None = None
    error: str | None = None


class AgentResponse(BaseModel):
    answer: str
    reasoning_summary: str
    tools: list[ToolCall]
    evidence: list[Citation]
    execution_results: list[object]
    grounded: bool
    approval_required: bool = False


class BenchmarkRunRequest(BaseModel):
    events: int = Field(default=2_000_000, ge=1, le=10_000_000)
    seed: int = Field(default=0x9E3779B97F4A7C15, ge=0)
    sigma: float = Field(default=12.0, gt=0, le=1000)


class BenchmarkRecord(BaseModel):
    id: int | None = None
    workload: str
    description: str | None = None
    event_count: int
    seed: int
    sigma: float
    throughput_events_sec: float
    average_latency_ns: float
    p50_latency_ns: float | None = None
    p90_latency_ns: float | None = None
    p99_latency_ns: float | None = None
    p999_latency_ns: float | None = None
    p9999_latency_ns: float | None = None
    max_latency_ns: float | None = None
    memory_mb: float | None = None
    compiler: str | None = None
    build_config: str | None = None
    machine: str | None = None
    raw_output: str
    raw_stderr: str = ""
    created_at: str


class BenchmarkRunResponse(BaseModel):
    run_id: str
    records: list[BenchmarkRecord]
    duration_ms: float


class PerformanceAnalysisRequest(BaseModel):
    question: str = Field(min_length=1, max_length=4000)


class PerformanceAnalysisResponse(BaseModel):
    answer: str
    measurements: list[BenchmarkRecord]
    evidence: list[Citation]
    hypotheses: list[str]


class RetrievalResult(BaseModel):
    chunk: Chunk
    score: float
    vector_score: float = 0.0
    keyword_score: float = 0.0
    rerank_score: float | None = None
    source: str = "hybrid"


class EvaluationQuestion(BaseModel):
    question: str
    expected_paths: list[str]
    expected_symbols: list[str] = Field(default_factory=list)
    answer_characteristics: list[str] = Field(default_factory=list)


class EvaluationMetric(BaseModel):
    strategy: str
    questions: int
    recall_at_k: float
    precision_at_k: float
    mrr: float
    ndcg: float
    citation_correctness: float
    answer_faithfulness: float | None = None
    retrieval_latency_ms: float
    generation_latency_ms: float | None = None
    tokens: int | None = None
    estimated_cost_usd: float | None = None


class EvaluationReport(BaseModel):
    generated_at: str
    k: int
    metrics: list[EvaluationMetric]
