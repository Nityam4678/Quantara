from __future__ import annotations

import math
import re
from dataclasses import dataclass
from typing import Any

from .models import Chunk
from .models import BenchmarkRecord


@dataclass
class StoredChunk:
    chunk: Chunk
    embedding: list[float]


class MemoryStore:
    def __init__(self):
        self.documents: dict[str, tuple[str, int]] = {}
        self.chunks: dict[str, StoredChunk] = {}
        self.evaluations: list[dict] = []
        self.agent_audit: list[dict] = []
        self.benchmarks: list[dict] = []

    def document_hash(self, path: str) -> str | None:
        value = self.documents.get(path)
        return value[0] if value else None

    def upsert_document(self, path: str, digest: str) -> int:
        version = self.documents.get(path, ("", 0))[1] + (0 if self.documents.get(path, ("", 0))[0] == digest else 1)
        self.documents[path] = (digest, version)
        return version

    def replace_chunks(self, path: str, chunks: list[Chunk], vectors: list[list[float]]) -> None:
        self.chunks = {key: value for key, value in self.chunks.items() if value.chunk.path != path}
        for chunk, vector in zip(chunks, vectors):
            self.chunks[chunk.chunk_id] = StoredChunk(chunk, vector)

    def search(self, vector: list[float], top_k: int, path_prefix: str | None = None) -> list[tuple[Chunk, float]]:
        def score(item: StoredChunk) -> float:
            return sum(a * b for a, b in zip(vector, item.embedding)) / (
                math.sqrt(sum(a * a for a in vector)) * math.sqrt(sum(b * b for b in item.embedding)) or 1)
        matches = [(item.chunk, score(item)) for item in self.chunks.values()
                   if not path_prefix or item.chunk.path.startswith(path_prefix)]
        return sorted(matches, key=lambda match: match[1], reverse=True)[:top_k]

    def search_keyword(self, query: str, top_k: int, path_prefix: str | None = None) -> list[tuple[Chunk, float]]:
        identifiers = [term.lower() for term in re.findall(
            r"[A-Za-z_]\w*(?:::\w+)*", query)]
        terms = list(dict.fromkeys(
            identifiers + [part for identifier in identifiers
                           for part in identifier.split("::")]))
        matches = []
        for item in self.chunks.values():
            if path_prefix and not item.chunk.path.startswith(path_prefix):
                continue
            haystack = " ".join(filter(None, [
                item.chunk.content, item.chunk.symbol, item.chunk.class_name,
                item.chunk.namespace, item.chunk.path,
            ])).lower()
            exact = any(term in {
                (item.chunk.symbol or "").lower(),
                (item.chunk.class_name or "").lower(),
                (item.chunk.namespace or "").lower(),
            } for term in terms)
            score = sum(haystack.count(term) for term in terms) + (10 if exact else 0)
            if score:
                matches.append((item.chunk, float(score)))
        return sorted(matches, key=lambda match: match[1], reverse=True)[:top_k]

    def count(self) -> int:
        return len(self.chunks)

    def save_evaluation(self, report: dict) -> None:
        self.evaluations.append(report)

    def save_agent_audit(self, record: dict) -> None:
        self.agent_audit.append(record)
        self.agent_audit[:] = self.agent_audit[-1000:]

    def save_benchmark(self, record: dict) -> None:
        self.benchmarks.append(record)

    def list_benchmarks(self, limit: int = 100) -> list[dict]:
        return self.benchmarks[-limit:][::-1]


class PostgresStore(MemoryStore):
    def __init__(self, database_url: str):
        super().__init__()
        try:
            import psycopg
        except ImportError as error:
            raise RuntimeError("psycopg is required for PostgreSQL storage") from error
        self.psycopg = psycopg
        self.connection = psycopg.connect(database_url, autocommit=True)

    def initialize(self) -> None:
        with self.connection.cursor() as cursor:
            cursor.execute("""
                CREATE EXTENSION IF NOT EXISTS vector;
                CREATE TABLE IF NOT EXISTS rag_documents (
                  path TEXT PRIMARY KEY, content_hash TEXT NOT NULL,
                  version INTEGER NOT NULL, updated_at TIMESTAMPTZ DEFAULT now()
                );
                CREATE TABLE IF NOT EXISTS rag_chunks (
                  chunk_id TEXT PRIMARY KEY, path TEXT NOT NULL,
                  start_line INTEGER NOT NULL, end_line INTEGER NOT NULL,
                  symbol TEXT, class_name TEXT, namespace TEXT, parent_symbol TEXT,
                  declaration TEXT,
                  chunk_kind TEXT NOT NULL DEFAULT 'section',
                  language TEXT NOT NULL, content TEXT NOT NULL,
                  content_hash TEXT NOT NULL, version INTEGER NOT NULL,
                  embedding vector, updated_at TIMESTAMPTZ DEFAULT now()
                );
                ALTER TABLE rag_chunks ADD COLUMN IF NOT EXISTS class_name TEXT;
                ALTER TABLE rag_chunks ADD COLUMN IF NOT EXISTS namespace TEXT;
                ALTER TABLE rag_chunks ADD COLUMN IF NOT EXISTS parent_symbol TEXT;
                ALTER TABLE rag_chunks ADD COLUMN IF NOT EXISTS declaration TEXT;
                ALTER TABLE rag_chunks ADD COLUMN IF NOT EXISTS chunk_kind TEXT NOT NULL DEFAULT 'section';
                CREATE INDEX IF NOT EXISTS rag_chunks_path_idx ON rag_chunks(path);
                ALTER TABLE rag_chunks ADD COLUMN IF NOT EXISTS search_vector tsvector
                  GENERATED ALWAYS AS (to_tsvector('simple',
                    coalesce(path,'') || ' ' || coalesce(symbol,'') || ' ' ||
                    coalesce(class_name,'') || ' ' || coalesce(namespace,'') || ' ' ||
                    content)) STORED;
                CREATE INDEX IF NOT EXISTS rag_chunks_search_idx
                  ON rag_chunks USING GIN(search_vector);
                CREATE TABLE IF NOT EXISTS rag_evaluations (
                  id BIGSERIAL PRIMARY KEY, generated_at TIMESTAMPTZ NOT NULL,
                  k INTEGER NOT NULL, report JSONB NOT NULL
                );
                CREATE TABLE IF NOT EXISTS agent_tool_audit (
                  id BIGSERIAL PRIMARY KEY, created_at TIMESTAMPTZ DEFAULT now(),
                  tool_name TEXT NOT NULL, arguments JSONB NOT NULL,
                  status TEXT NOT NULL, duration_ms DOUBLE PRECISION,
                  error TEXT
                );
                CREATE TABLE IF NOT EXISTS benchmark_runs (
                  id BIGSERIAL PRIMARY KEY, workload TEXT NOT NULL,
                  description TEXT, event_count BIGINT NOT NULL, seed NUMERIC NOT NULL,
                  sigma DOUBLE PRECISION NOT NULL, throughput_events_sec DOUBLE PRECISION NOT NULL,
                  average_latency_ns DOUBLE PRECISION NOT NULL, p50_latency_ns DOUBLE PRECISION,
                  p90_latency_ns DOUBLE PRECISION, p99_latency_ns DOUBLE PRECISION,
                  p999_latency_ns DOUBLE PRECISION, p9999_latency_ns DOUBLE PRECISION,
                  max_latency_ns DOUBLE PRECISION, memory_mb DOUBLE PRECISION,
                  compiler TEXT, build_config TEXT, machine TEXT, raw_output TEXT NOT NULL,
                  raw_stderr TEXT NOT NULL DEFAULT '',
                  created_at TIMESTAMPTZ NOT NULL
                );
                ALTER TABLE benchmark_runs ADD COLUMN IF NOT EXISTS raw_stderr TEXT NOT NULL DEFAULT '';
            """)

    def document_hash(self, path: str) -> str | None:
        with self.connection.cursor() as cursor:
            cursor.execute("SELECT content_hash FROM rag_documents WHERE path = %s", (path,))
            row = cursor.fetchone()
        return row[0] if row else None

    def upsert_document(self, path: str, digest: str) -> int:
        with self.connection.cursor() as cursor:
            cursor.execute("SELECT version, content_hash FROM rag_documents WHERE path = %s", (path,))
            row = cursor.fetchone()
            version = (row[0] if row else 0) + (0 if row and row[1] == digest else 1)
            cursor.execute("""
              INSERT INTO rag_documents(path, content_hash, version) VALUES(%s,%s,%s)
              ON CONFLICT(path) DO UPDATE SET content_hash=EXCLUDED.content_hash,
              version=EXCLUDED.version, updated_at=now()
            """, (path, digest, version))
        return version

    def replace_chunks(self, path: str, chunks: list[Chunk], vectors: list[list[float]]) -> None:
        with self.connection.cursor() as cursor:
            cursor.execute("DELETE FROM rag_chunks WHERE path = %s", (path,))
            for chunk, vector in zip(chunks, vectors):
                cursor.execute("""
                  INSERT INTO rag_chunks(chunk_id,path,start_line,end_line,symbol,class_name,
                  namespace,parent_symbol,declaration,chunk_kind,language,
                  content,content_hash,version,embedding)
                  VALUES(%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
                """, (chunk.chunk_id, chunk.path, chunk.start_line, chunk.end_line,
                      chunk.symbol, chunk.class_name, chunk.namespace, chunk.parent_symbol,
                      chunk.declaration, chunk.chunk_kind, chunk.language, chunk.content,
                      chunk.content_hash, chunk.version, vector))

    def search(self, vector: list[float], top_k: int, path_prefix: str | None = None) -> list[tuple[Chunk, float]]:
        with self.connection.cursor() as cursor:
            if path_prefix:
                cursor.execute("""
                  SELECT chunk_id,path,start_line,end_line,symbol,class_name,namespace,
                  parent_symbol,declaration,chunk_kind,language,content,
                  content_hash,version,1-(embedding <=> %s::vector) AS score
                  FROM rag_chunks WHERE path LIKE %s ORDER BY embedding <=> %s::vector LIMIT %s
                """, (vector, f"{path_prefix}%", vector, top_k))
            else:
                cursor.execute("""
                  SELECT chunk_id,path,start_line,end_line,symbol,class_name,namespace,
                  parent_symbol,declaration,chunk_kind,language,content,
                  content_hash,version,1-(embedding <=> %s::vector) AS score
                  FROM rag_chunks ORDER BY embedding <=> %s::vector LIMIT %s
                """, (vector, vector, top_k))
            rows = cursor.fetchall()
        return [(Chunk(chunk_id=row[0], path=row[1], start_line=row[2], end_line=row[3],
                       symbol=row[4], class_name=row[5], namespace=row[6],
                       parent_symbol=row[7], declaration=row[8], chunk_kind=row[9],
                       language=row[10], content=row[11], content_hash=row[12],
                       version=row[13]), float(row[14]))
                for row in rows]

    def search_keyword(self, query: str, top_k: int,
                       path_prefix: str | None = None) -> list[tuple[Chunk, float]]:
        with self.connection.cursor() as cursor:
            path_clause = "AND path LIKE %s" if path_prefix else ""
            params = [query]
            if path_prefix:
                params.append(f"{path_prefix}%")
            params.append(top_k)
            cursor.execute(f"""
                SELECT chunk_id,path,start_line,end_line,symbol,class_name,namespace,
                parent_symbol,declaration,chunk_kind,language,content,
                content_hash,version,ts_rank_cd(search_vector, plainto_tsquery('simple', %s)) AS score
                FROM rag_chunks
                WHERE search_vector @@ plainto_tsquery('simple', %s) {path_clause}
                ORDER BY score DESC LIMIT %s
            """, [query, query, *params[1:]])
            rows = cursor.fetchall()
        return [(Chunk(chunk_id=row[0], path=row[1], start_line=row[2], end_line=row[3],
                       symbol=row[4], class_name=row[5], namespace=row[6],
                       parent_symbol=row[7], declaration=row[8], chunk_kind=row[9],
                       language=row[10], content=row[11], content_hash=row[12],
                       version=row[13]), float(row[14])) for row in rows]

    def save_evaluation(self, report: dict) -> None:
        with self.connection.cursor() as cursor:
            cursor.execute(
                       "INSERT INTO rag_evaluations(generated_at,k,report) VALUES(%s,%s,%s)",
                       (report["generated_at"], report["k"], report))

    def save_agent_audit(self, record: dict) -> None:
        with self.connection.cursor() as cursor:
            cursor.execute(
                       """INSERT INTO agent_tool_audit(tool_name,arguments,status,duration_ms,error)
                          VALUES(%s,%s,%s,%s,%s)""",
                       (record["name"], record["arguments"], record["status"],
                        record["duration_ms"], record.get("error")))

    def save_benchmark(self, record: dict) -> None:
        with self.connection.cursor() as cursor:
            cursor.execute("""
              INSERT INTO benchmark_runs(
                workload,description,event_count,seed,sigma,throughput_events_sec,
                average_latency_ns,p50_latency_ns,p90_latency_ns,p99_latency_ns,
                p999_latency_ns,p9999_latency_ns,max_latency_ns,memory_mb,compiler,
                build_config,machine,raw_output,raw_stderr,created_at)
              VALUES(%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
            """, tuple(record.get(key) for key in (
                "workload", "description", "event_count", "seed", "sigma",
                "throughput_events_sec", "average_latency_ns", "p50_latency_ns",
                "p90_latency_ns", "p99_latency_ns", "p999_latency_ns",
                "p9999_latency_ns", "max_latency_ns", "memory_mb", "compiler",
                "build_config", "machine", "raw_output", "raw_stderr", "created_at")))

    def list_benchmarks(self, limit: int = 100) -> list[dict]:
        with self.connection.cursor() as cursor:
            cursor.execute("""
              SELECT id,workload,description,event_count,seed,sigma,
              throughput_events_sec,average_latency_ns,p50_latency_ns,p90_latency_ns,
              p99_latency_ns,p999_latency_ns,p9999_latency_ns,max_latency_ns,memory_mb,
              compiler,build_config,machine,raw_output,raw_stderr,created_at
              FROM benchmark_runs ORDER BY created_at DESC LIMIT %s
            """, (limit,))
            rows = cursor.fetchall()
        keys = ("id", "workload", "description", "event_count", "seed", "sigma",
                "throughput_events_sec", "average_latency_ns", "p50_latency_ns",
                "p90_latency_ns", "p99_latency_ns", "p999_latency_ns",
                "p9999_latency_ns", "max_latency_ns", "memory_mb", "compiler",
                "build_config", "machine", "raw_output", "raw_stderr", "created_at")
        results = []
        for row in rows:
            record = dict(zip(keys, row))
            if hasattr(record["created_at"], "isoformat"):
                record["created_at"] = record["created_at"].isoformat()
            results.append(record)
        return results
