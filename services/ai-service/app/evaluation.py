from __future__ import annotations

import math
import time
from datetime import datetime, timezone

from .models import EvaluationMetric, EvaluationQuestion, EvaluationReport
from .rag import CodebaseRAG


EVALUATION_DATASET = [
    EvaluationQuestion(question=q, expected_paths=paths, expected_symbols=symbols,
                       answer_characteristics=characteristics)
    for q, paths, symbols, characteristics in [
        ("How does price-time priority work?", ["src/order_book.cpp", "include/lob/order_book.hpp"], ["add_order"], ["price priority", "time priority"]),
        ("How are orders matched?", ["src/order_book.cpp"], ["add_order"], ["opposite side"]),
        ("How does cancellation locate an order?", ["src/order_book.cpp", "include/lob/order_map.hpp"], ["cancel_order"], ["order id"]),
        ("How does the fixed order pool handle exhaustion?", ["src/order_pool.cpp", "include/lob/order_pool.hpp"], ["allocate"], ["reject"]),
        ("What is the intrusive queue structure?", ["src/price_level.cpp", "include/lob/price_level.hpp"], [], ["FIFO"]),
        ("How are best bid and ask cached?", ["src/order_book.cpp", "include/lob/order_book.hpp"], ["best_bid", "best_ask"], ["top of book"]),
        ("Where are execution reports generated?", ["src/order_book.cpp", "include/lob/order_book.hpp"], ["last_report"], ["execution"]),
        ("How are trades represented?", ["include/lob/order_book.hpp"], ["Trade"], ["quantity"]),
        ("What are the supported price bounds?", ["src/order_book.cpp", "include/lob/order_book.hpp"], [], ["bounds"]),
        ("How does the order ID index work?", ["include/lob/order_map.hpp"], ["OrderTracker"], ["lookup"]),
        ("What invariants does verify check?", ["src/order_book.cpp", "include/lob/order_book.hpp"], ["verify"], ["invariant"]),
        ("How does partial fill preserve FIFO priority?", ["src/price_level.cpp", "tests/level_smoke.cpp"], [], ["partial"]),
        ("How does a crossing order sweep levels?", ["src/order_book.cpp", "tests/book_smoke.cpp"], ["add_order"], ["levels"]),
        ("What is the benchmark workload?", ["src/main.cpp", "README.md"], [], ["benchmark"]),
        ("What are the latency characteristics?", ["README.md", "src/main.cpp"], [], ["latency"]),
        ("Why is the engine single threaded?", ["README.md", "include/lob/order_book.hpp"], [], ["single-threaded"]),
        ("How does the flat price array work?", ["include/lob/order_book.hpp", "src/order_book.cpp"], [], ["flat array"]),
        ("What happens when a level becomes empty?", ["src/order_book.cpp"], [], ["best price"]),
        ("How are orders linked in a price level?", ["include/lob/order.hpp", "include/lob/price_level.hpp"], [], ["prev", "next"]),
        ("What does the Order data structure contain?", ["include/lob/order.hpp"], ["Order"], ["order"]),
        ("How are tests organized?", ["tests/book_smoke.cpp", "tests/level_smoke.cpp", "tests/pool_smoke.cpp"], [], ["tests"]),
        ("Does the engine support market orders?", ["README.md"], [], ["not implemented"]),
        ("How does the native adapter communicate?", ["services/engine-adapter/main.cpp"], [], ["JSON"]),
        ("How does the gateway handle engine errors?", ["services/api-gateway/src/engine-client.js"], [], ["error"]),
        ("How are source chunks created?", ["services/ai-service/app/chunking.py"], ["chunk_cpp"], ["Tree-sitter"]),
        ("How are Markdown sections split?", ["services/ai-service/app/chunking.py"], ["chunk_markdown"], ["Markdown"]),
        ("How does incremental ingestion avoid re-embedding?", ["services/ai-service/app/rag.py"], ["ingest"], ["hash"]),
        ("How are vectors stored?", ["services/ai-service/app/store.py"], ["PostgresStore"], ["pgvector"]),
        ("How are citations returned?", ["services/ai-service/app/rag.py", "services/ai-service/app/models.py"], ["Citation"], ["line"]),
        ("How does the RAG assistant abstain?", ["services/ai-service/app/rag.py"], ["answer"], ["insufficient"]),
    ]
]


def _dcg(relevances: list[int]) -> float:
    return sum(value / math.log2(index + 2) for index, value in enumerate(relevances))


def _score_strategy(rag: CodebaseRAG, strategy: str, k: int) -> EvaluationMetric:
    started = time.perf_counter()
    generation_started = time.perf_counter()
    recall = precision = mrr = ndcg = citation = faithfulness = 0.0
    generated = 0
    for item in EVALUATION_DATASET:
        results = rag.retrieve(item.question, k, strategy=strategy)
        paths = [chunk.path for chunk, _ in results]
        relevant = [path for path in paths if any(path.endswith(expected) for expected in item.expected_paths)]
        recall += len(set(relevant)) / max(1, len(item.expected_paths))
        precision += len(relevant) / max(1, k)
        rank = next((index + 1 for index, path in enumerate(paths)
                     if any(path.endswith(expected) for expected in item.expected_paths)), 0)
        mrr += 1 / rank if rank else 0
        ndcg += _dcg([int(any(path.endswith(expected) for expected in item.expected_paths))
                      for path in paths]) / max(1.0, _dcg([1] * min(k, len(item.expected_paths))))
        citation += int(bool(relevant))
        if strategy == "hybrid+rereank":
            answer = rag.answer(item.question, k)
            generated += 1
            faithfulness += int(
                answer.grounded and bool(answer.citations) and
                all(citation_item.snippet.strip() for citation_item in answer.citations))
    count = len(EVALUATION_DATASET)
    return EvaluationMetric(
        strategy=strategy, questions=count, recall_at_k=round(recall / count, 4),
        precision_at_k=round(precision / count, 4), mrr=round(mrr / count, 4),
        ndcg=round(ndcg / count, 4), citation_correctness=round(citation / count, 4),
        answer_faithfulness=round(faithfulness / max(1, generated), 4) if generated else None,
        retrieval_latency_ms=round((time.perf_counter() - started) * 1000 / count, 3),
        generation_latency_ms=round((time.perf_counter() - generation_started) * 1000 /
                                    max(1, generated), 3) if generated else None,
        tokens=0 if generated else None,
        estimated_cost_usd=0.0 if generated else None,
    )


def evaluate(rag: CodebaseRAG, k: int) -> EvaluationReport:
    strategies = ["vector", "keyword", "hybrid", "hybrid+rereank"]
    metrics = [_score_strategy(rag, strategy, k) for strategy in strategies]
    return EvaluationReport(
        generated_at=datetime.now(timezone.utc).isoformat(), k=k, metrics=metrics)
