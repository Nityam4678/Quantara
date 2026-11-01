from __future__ import annotations

import json
import re
import urllib.request
from dataclasses import dataclass

from .config import Settings
from .embeddings import EmbeddingProvider
from .models import Chunk, RetrievalResult


@dataclass(frozen=True)
class QueryPlan:
    original: str
    expanded: str
    concepts: tuple[str, ...]
    identifiers: tuple[str, ...]


def understand_query(question: str) -> QueryPlan:
    identifiers = tuple(dict.fromkeys(re.findall(r"\b[A-Za-z_]\w*(?:::\w+)*\b", question)))
    lower = question.lower()
    concepts = tuple(name for name, terms in {
        "matching": ("match", "cross", "priority", "fill", "execution"),
        "data_structures": ("pool", "list", "array", "index", "level", "pointer"),
        "cancellation": ("cancel", "remove", "order id"),
        "performance": ("latency", "throughput", "benchmark", "allocation", "cache"),
    }.items() if any(term in lower for term in terms))
    additions = {
        "matching": " price-time priority execution report",
        "data_structures": " implementation data structure invariant",
        "cancellation": " cancel_order order lifecycle",
        "performance": " benchmark latency complexity",
    }
    expanded = question + "".join(additions[concept] for concept in concepts)
    return QueryPlan(question, expanded, concepts, identifiers)


def reciprocal_rank_fusion(ranked_lists: list[list[tuple[Chunk, float]]],
                           weights: list[float], k: int = 60) -> list[tuple[Chunk, float]]:
    scores: dict[str, tuple[Chunk, float]] = {}
    for ranked, weight in zip(ranked_lists, weights):
        for rank, (chunk, _) in enumerate(ranked, start=1):
            current = scores.get(chunk.chunk_id)
            score = weight / (k + rank)
            scores[chunk.chunk_id] = (chunk, (current[1] if current else 0.0) + score)
    return sorted(scores.values(), key=lambda item: item[1], reverse=True)


class Reranker:
    def __init__(self, settings: Settings):
        self.settings = settings

    def score(self, query: str, chunks: list[tuple[Chunk, float]]) -> list[RetrievalResult]:
        if not chunks:
            return []
        if self.settings.rerank_api_url and self.settings.rerank_model:
            payload = json.dumps({
                "model": self.settings.rerank_model,
                "query": query,
                "documents": [chunk.content for chunk, _ in chunks],
            }).encode()
            request = urllib.request.Request(
                self.settings.rerank_api_url, data=payload,
                headers={"Content-Type": "application/json",
                         **({"Authorization": f"Bearer {self.settings.rerank_api_key}"}
                            if self.settings.rerank_api_key else {})})
            with urllib.request.urlopen(request, timeout=30) as response:
                data = json.loads(response.read())
            scores = {int(item["index"]): float(item["relevance_score"])
                      for item in data.get("results", [])}
        else:
            terms = set(re.findall(r"[A-Za-z_]\w+", query.lower()))
            scores = {
                index: sum(token in chunk.content.lower() for token in terms) / max(1, len(terms))
                for index, (chunk, _) in enumerate(chunks)
            }
        return [
            RetrievalResult(chunk=chunk, score=score, rerank_score=scores.get(index, 0.0),
                            source="hybrid+rereank")
            for index, (chunk, score) in enumerate(chunks)
        ]
