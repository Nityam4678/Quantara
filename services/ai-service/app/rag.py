from __future__ import annotations

import hashlib
import json
import time
import urllib.request
from pathlib import Path

from langchain_core.documents import Document
from langchain_core.prompts import PromptTemplate
from langchain_core.retrievers import BaseRetriever

from .chunking import CODE_EXTENSIONS, DOC_EXTENSIONS, chunk_file, content_hash
from .config import Settings
from .embeddings import EmbeddingProvider
from .models import Citation, ChatResponse, Chunk, IngestResponse
from .store import MemoryStore
from .retrieval import Reranker, QueryPlan, understand_query, reciprocal_rank_fusion


class CodeChunkRetriever(BaseRetriever):
    """LangChain retriever adapter over the modular vector-store contract."""

    embedder: EmbeddingProvider
    store: MemoryStore
    top_k: int = 6
    path_prefix: str | None = None

    model_config = {"arbitrary_types_allowed": True}

    def _get_relevant_documents(self, query: str, *, run_manager=None) -> list[Document]:
        return [
            Document(
                page_content=chunk.content,
                metadata={
                    "chunk": chunk,
                    "score": score,
                    "path": chunk.path,
                    "start_line": chunk.start_line,
                    "end_line": chunk.end_line,
                },
            )
            for chunk, score in self.store.search(
                self.embedder.embed_query(query), self.top_k, self.path_prefix)
        ]


class CodebaseRAG:
    def __init__(self, settings: Settings, store: MemoryStore | None = None,
                 embedder: EmbeddingProvider | None = None):
        self.settings = settings
        self.store = store or MemoryStore()
        self.embedder = embedder or EmbeddingProvider(
            settings.embedding_api_url, settings.embedding_api_key,
            settings.embedding_model, settings.embedding_dimensions)
        self.retriever = CodeChunkRetriever(
            embedder=self.embedder, store=self.store, top_k=settings.top_k)
        self.reranker = Reranker(settings)

    def files(self) -> list[Path]:
        allowed = CODE_EXTENSIONS | DOC_EXTENSIONS
        excluded = {".git", "build", "node_modules", "dist", "__pycache__", ".venv",
                    "CMakeFiles", "out"}
        return sorted(path for path in self.settings.repository_root.rglob("*")
                      if path.is_file() and path.suffix.lower() in allowed and
                      not any(part in excluded or part.endswith("-build")
                              for part in path.parts))

    def ingest(self) -> IngestResponse:
        scanned = changed = unchanged = chunks_count = embedded = 0
        for path in self.files():
            scanned += 1
            content = path.read_text(encoding="utf-8")
            digest = content_hash(content)
            relative = path.relative_to(self.settings.repository_root).as_posix()
            if self.store.document_hash(relative) == digest:
                unchanged += 1
                continue
            changed += 1
            version = self.store.upsert_document(relative, digest)
            chunks = chunk_file(
                path, self.settings.repository_root, content,
                max_chars=self.settings.cpp_chunk_size,
                chunk_size=self.settings.markdown_chunk_size,
                chunk_overlap=self.settings.markdown_chunk_overlap,
            )
            chunks = [chunk.model_copy(update={"version": version}) for chunk in chunks]
            vectors = self.embedder.embed([chunk.content for chunk in chunks])
            self.store.replace_chunks(relative, chunks, vectors)
            chunks_count += len(chunks)
            embedded += len(vectors)
        return IngestResponse(scanned=scanned, changed=changed, unchanged=unchanged,
                              chunks=chunks_count, embedded=embedded)

    def retrieve(self, question: str, top_k: int | None = None,
                 path_prefix: str | None = None,
                 strategy: str = "hybrid") -> list[tuple[Chunk, float]]:
        plan = understand_query(question)
        limit = top_k or self.settings.retrieval_candidate_k
        vector = self.store.search(self.embedder.embed_query(plan.expanded), limit, path_prefix)
        keyword = self.store.search_keyword(plan.original, limit, path_prefix)
        if strategy == "vector":
            return vector[:top_k or self.settings.top_k]
        if strategy == "keyword":
            return keyword[:top_k or self.settings.top_k]
        fused = reciprocal_rank_fusion(
            [vector, keyword],
            [self.settings.retrieval_vector_weight, self.settings.retrieval_keyword_weight])
        if strategy == "hybrid+rereank" and self.settings.rerank_enabled:
            ranked = self.reranker.score(plan.original, fused[:limit])
            ranked.sort(key=lambda result: result.rerank_score or 0.0, reverse=True)
            return [(result.chunk, result.rerank_score or result.score)
                    for result in ranked[:top_k or self.settings.top_k]]
        return fused[:top_k or self.settings.top_k]

    def _llm(self, prompt: str) -> str | None:
        if not self.settings.llm_api_url or not self.settings.llm_model:
            return None
        payload = json.dumps({
            "model": self.settings.llm_model,
            "messages": [
                {"role": "system", "content": "Answer only from supplied codebase evidence. If evidence is insufficient, say so."},
                {"role": "user", "content": prompt},
            ],
            "temperature": 0.1,
        }).encode()
        request = urllib.request.Request(self.settings.llm_api_url, data=payload, headers={
            "Content-Type": "application/json",
            **({"Authorization": f"Bearer {self.settings.llm_api_key}"} if self.settings.llm_api_key else {}),
        })
        with urllib.request.urlopen(request, timeout=60) as response:
            body = json.loads(response.read())
        return body["choices"][0]["message"]["content"]

    def answer(self, question: str, top_k: int | None = None) -> ChatResponse:
        retrieval_started = time.perf_counter()
        matches = self.retrieve(question, top_k, strategy="hybrid+rereank")
        retrieval_latency = (time.perf_counter() - retrieval_started) * 1000
        citations = [Citation(path=chunk.path, start_line=chunk.start_line,
                              end_line=chunk.end_line, symbol=chunk.symbol,
                              snippet=chunk.content[:1200], score=round(score, 4))
                     for chunk, score in matches if score > 0]
        if not citations:
            return ChatResponse(            answer="There is insufficient evidence in the indexed codebase to answer that.",
                                citations=[], grounded=False,
                                uncertainty="No indexed source matched the question.",
                                retrieval_latency_ms=round(retrieval_latency, 3))
        context = "\n\n".join(f"[{citation.path}:{citation.start_line}-{citation.end_line}]\n{citation.snippet}"
                              for citation in citations)
        prompt = PromptTemplate.from_template(
            "Question: {question}\n\nEvidence:\n{context}\n\n"
            "Answer only from the evidence. Cite the supplied file and line ranges "
            "when explaining implementation details."
        ).format(question=question, context=context)
        generation_started = time.perf_counter()
        answer = self._llm(prompt) or (
            f"I found {len(citations)} relevant source section(s). "
            f"The strongest evidence is in {citations[0].path}:{citations[0].start_line}-{citations[0].end_line}. "
            "Configure LLM_API_URL and LLM_MODEL for a generated explanation grounded in these citations.")
        return ChatResponse(answer=answer, citations=citations, grounded=True,
                            retrieval_latency_ms=round(retrieval_latency, 3),
                            generation_latency_ms=round((time.perf_counter() -
                                                         generation_started) * 1000, 3))
