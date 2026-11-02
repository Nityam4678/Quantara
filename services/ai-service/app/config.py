from dataclasses import dataclass
import os
from pathlib import Path


def _repository_root() -> Path:
    configured = os.getenv("REPOSITORY_ROOT")
    if configured:
        return Path(configured).resolve()
    return Path(__file__).resolve().parents[3]


@dataclass(frozen=True)
class Settings:
    repository_root: Path = _repository_root()
    database_url: str = os.getenv("DATABASE_URL", "")
    embedding_api_url: str = os.getenv("EMBEDDING_API_URL", "")
    embedding_api_key: str = os.getenv("EMBEDDING_API_KEY", "")
    embedding_model: str = os.getenv("EMBEDDING_MODEL", "text-embedding-3-small")
    embedding_dimensions: int = int(os.getenv("EMBEDDING_DIMENSIONS", "1536"))
    llm_api_url: str = os.getenv("LLM_API_URL", "")
    llm_api_key: str = os.getenv("LLM_API_KEY", "")
    llm_model: str = os.getenv("LLM_MODEL", "")
    top_k: int = int(os.getenv("TOP_K", "6"))
    cpp_chunk_size: int = int(os.getenv("CPP_CHUNK_SIZE", "12000"))
    markdown_chunk_size: int = int(os.getenv("MARKDOWN_CHUNK_SIZE", "6000"))
    markdown_chunk_overlap: int = int(os.getenv("MARKDOWN_CHUNK_OVERLAP", "200"))
    retrieval_vector_weight: float = float(os.getenv("RETRIEVAL_VECTOR_WEIGHT", "0.65"))
    retrieval_keyword_weight: float = float(os.getenv("RETRIEVAL_KEYWORD_WEIGHT", "0.35"))
    retrieval_candidate_k: int = int(os.getenv("RETRIEVAL_CANDIDATE_K", "20"))
    rerank_enabled: bool = os.getenv("RERANK_ENABLED", "true").lower() == "true"
    rerank_api_url: str = os.getenv("RERANK_API_URL", "")
    rerank_api_key: str = os.getenv("RERANK_API_KEY", "")
    rerank_model: str = os.getenv("RERANK_MODEL", "")
    evaluation_k: int = int(os.getenv("EVALUATION_K", "5"))
    engine_api_url: str = os.getenv("ENGINE_API_URL", "http://127.0.0.1:3000")
    agent_max_iterations: int = int(os.getenv("AGENT_MAX_ITERATIONS", "6"))
    agent_tool_timeout_seconds: float = float(os.getenv("AGENT_TOOL_TIMEOUT_SECONDS", "5"))
    agent_retry_count: int = int(os.getenv("AGENT_RETRY_COUNT", "1"))
    benchmark_executable: str = os.getenv("BENCHMARK_EXECUTABLE", "")
    benchmark_timeout_seconds: float = float(os.getenv("BENCHMARK_TIMEOUT_SECONDS", "120"))
    benchmark_max_events: int = int(os.getenv("BENCHMARK_MAX_EVENTS", "10000000"))
    benchmark_max_concurrent: int = int(os.getenv("BENCHMARK_MAX_CONCURRENT", "1"))
    internal_service_token: str = os.getenv("INTERNAL_SERVICE_TOKEN", "")
    redis_url: str = os.getenv("REDIS_URL", "redis://127.0.0.1:6379/0")
    log_level: str = os.getenv("LOG_LEVEL", "INFO")


settings = Settings()
