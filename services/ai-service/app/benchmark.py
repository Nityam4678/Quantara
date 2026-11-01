from __future__ import annotations

import os
import platform
import re
import subprocess
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

from .config import Settings
from .models import BenchmarkRecord, BenchmarkRunRequest, BenchmarkRunResponse
from .models import Citation, PerformanceAnalysisResponse


_NUMBER = r"([0-9]+(?:\.[0-9]+)?)"


def _number(pattern: str, text: str) -> float | None:
    match = re.search(pattern, text, re.MULTILINE)
    return float(match.group(1)) if match else None


def _int(pattern: str, text: str) -> int | None:
    value = _number(pattern, text)
    return int(value) if value is not None else None


def parse_benchmark_output(output: str, request: BenchmarkRunRequest,
                           created_at: str | None = None) -> list[BenchmarkRecord]:
    compiler = re.search(r"^\s*compiler\s+(.+)$", output, re.MULTILINE)
    asserts = re.search(r"^\s*asserts\s+(.+)$", output, re.MULTILINE)
    machine = f"{platform.system()} {platform.release()} / {platform.machine()}"
    records: list[BenchmarkRecord] = []
    sections = re.split(r"(?=^WORKLOAD:\s)", output, flags=re.MULTILINE)
    for section in sections:
        header = re.search(r"^WORKLOAD:\s*(.+)$", section, re.MULTILINE)
        if not header:
            continue
        workload = header.group(1).strip()
        description_match = re.search(
            r"^WORKLOAD:.*?\n(.*?)(?=^\s*generated\s)", section,
            re.MULTILINE | re.DOTALL)
        bulk = re.search(
            rf"throughput\s+{_NUMBER}\s+events/sec.*?average latency\s+{_NUMBER}\s+ns/event",
            section, re.DOTALL)
        percentile = re.search(
            r"^\s*all\s+" + r"\s+".join([_NUMBER] * 7) + r"\s*$",
            section, re.MULTILINE)
        if not bulk or not percentile:
            raise ValueError(f"benchmark output missing required metrics for {workload}")
        values = [float(value) for value in percentile.groups()]
        records.append(BenchmarkRecord(
            workload=workload,
            description=description_match.group(1).strip() if description_match else None,
            event_count=_int(r"events processed\s+(\d+)", section) or request.events,
            seed=request.seed,
            sigma=request.sigma,
            throughput_events_sec=float(bulk.group(1)),
            average_latency_ns=float(bulk.group(2)),
            p50_latency_ns=values[1],
            p90_latency_ns=values[2],
            p99_latency_ns=values[3],
            p999_latency_ns=values[4],
            p9999_latency_ns=values[5],
            max_latency_ns=values[6],
            memory_mb=_number(r"order working set\s+" + _NUMBER + r"\s+MB", section),
            compiler=compiler.group(1).strip() if compiler else None,
            build_config=f"asserts {asserts.group(1).strip()}" if asserts else None,
            machine=machine,
            raw_output=output,
            created_at=created_at or datetime.now(timezone.utc).isoformat(),
        ))
    if not records:
        raise ValueError("benchmark output contained no workload records")
    return records


class BenchmarkRunner:
    def __init__(self, settings: Settings, store):
        self.settings = settings
        self.store = store
        self._lock = threading.BoundedSemaphore(settings.benchmark_max_concurrent)

    def executable(self) -> Path:
        if not self.settings.benchmark_executable:
            raise RuntimeError("BENCHMARK_EXECUTABLE is not configured")
        path = Path(self.settings.benchmark_executable).resolve()
        if not path.is_file():
            raise RuntimeError(f"benchmark executable does not exist: {path}")
        return path

    def run(self, request: BenchmarkRunRequest) -> BenchmarkRunResponse:
        if request.events > self.settings.benchmark_max_events:
            raise ValueError("event count exceeds configured maximum")
        if not self._lock.acquire(blocking=False):
            raise RuntimeError("another benchmark is already running")
        started = time.perf_counter()
        try:
            command = [
                str(self.executable()), f"--events={request.events}",
                f"--seed={request.seed}", f"--sigma={request.sigma}",
            ]
            creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
            try:
                completed = subprocess.run(
                    command, capture_output=True, text=True, timeout=self.settings.benchmark_timeout_seconds,
                    cwd=str(self.settings.repository_root), check=False,
                    creationflags=creationflags)
            except subprocess.TimeoutExpired as error:
                raise TimeoutError("benchmark process exceeded configured timeout") from error
            if completed.returncode != 0:
                raise RuntimeError(
                    f"benchmark exited with code {completed.returncode}: "
                    f"{completed.stderr[-2000:]}")
            output = completed.stdout
            records = parse_benchmark_output(output, request)
            for record in records:
                record.raw_stderr = completed.stderr
                self.store.save_benchmark(record.model_dump())
            return BenchmarkRunResponse(
                run_id=str(uuid.uuid4()), records=records,
                duration_ms=round((time.perf_counter() - started) * 1000, 3))
        finally:
            self._lock.release()


def analyze_performance(question: str, store, rag) -> PerformanceAnalysisResponse:
    records = [BenchmarkRecord(**record) for record in store.list_benchmarks(100)]
    lower = question.lower()
    if "deep" in lower and "cross" in lower:
        selected = [record for record in records
                    if record.workload in {"deep book", "crossing flow"}]
    elif "deep" in lower:
        selected = [record for record in records if record.workload == "deep book"]
    elif "cross" in lower:
        selected = [record for record in records if record.workload == "crossing flow"]
    else:
        selected = records[:10]
    selected = selected[:20]
    evidence = []
    for chunk, score in rag.retrieve(question, 6, strategy="hybrid+rereank"):
        evidence.append(Citation(
            path=chunk.path, start_line=chunk.start_line, end_line=chunk.end_line,
            symbol=chunk.symbol, snippet=chunk.content[:1200], score=round(float(score), 4)))
    facts: list[str] = []
    hypotheses: list[str] = []
    workloads = {record.workload for record in selected}
    for workload in sorted(workloads):
        workload_records = [record for record in selected if record.workload == workload]
        latest = workload_records[0]
        facts.append(
            f"Measured {workload}: {latest.throughput_events_sec:.2f} events/sec, "
            f"{latest.average_latency_ns:.2f} ns average latency, "
            f"p99 {latest.p99_latency_ns:.2f} ns, p99.9 {latest.p999_latency_ns:.2f} ns.")
    if len(workloads) >= 2:
        crossing = next((record for record in selected if record.workload == "crossing flow"), None)
        deep = next((record for record in selected if record.workload == "deep book"), None)
        if crossing and deep:
            facts.append(
                f"Measured deep-book throughput is {deep.throughput_events_sec / crossing.throughput_events_sec:.3f}x "
                "the crossing-flow throughput for the selected runs.")
            hypotheses.append(
                "The benchmark documentation hypothesizes memory-bound behavior for deep books; "
                "confirming a bottleneck requires profiling evidence.")
    hypotheses.append(
        "Suggested next experiment: repeat the same seed and event count with a profiler, "
        "then compare allocation, cache-miss, and matching-loop samples.")
    answer = ("Measured facts:\n- " + "\n- ".join(facts) +
              "\n\nHypotheses and next steps:\n- " + "\n- ".join(hypotheses))
    return PerformanceAnalysisResponse(
        answer=answer, measurements=selected, evidence=evidence, hypotheses=hypotheses)
