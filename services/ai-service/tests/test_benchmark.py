from pathlib import Path

import pytest

from app.benchmark import BenchmarkRunner, parse_benchmark_output
from app.config import Settings
from app.models import BenchmarkRunRequest
from app.store import MemoryStore


OUTPUT = """compiler              gcc 15.2.0
asserts                off (NDEBUG)
WORKLOAD: crossing flow
  Prices overlap the mid.
  generated 10000 events in 0.00 s
------------------------------------------------------------------------------
  events processed             10000
  throughput                17771459 events/sec  (17.77 M/s)
  average latency               56.3 ns/event
  order working set              0.0 MB
                  mean       p50       p90       p99     p99.9    p99.99       max
  all               70        70       100       160       271       651       962
WORKLOAD: deep book
  Same distribution.
  generated 10000 events in 0.00 s
  events processed             10000
  throughput                16567263 events/sec  (16.57 M/s)
  average latency               60.4 ns/event
  order working set              0.2 MB
                  mean       p50       p90       p99     p99.9    p99.99       max
  all               82        70        90       180       501     23615     23785
"""


def test_parser_extracts_all_workloads_and_percentiles():
    records = parse_benchmark_output(OUTPUT, BenchmarkRunRequest(events=10000, seed=42, sigma=12))
    assert [record.workload for record in records] == ["crossing flow", "deep book"]
    assert records[0].throughput_events_sec == 17771459
    assert records[1].p999_latency_ns == 501
    assert records[1].memory_mb == 0.2
    assert records[0].compiler == "gcc 15.2.0"


def test_parser_rejects_incomplete_output():
    with pytest.raises(ValueError, match="missing required"):
        parse_benchmark_output("WORKLOAD: crossing flow\n", BenchmarkRunRequest())


def test_runner_rejects_unconfigured_executable(tmp_path: Path):
    settings = Settings(repository_root=tmp_path)
    with pytest.raises(RuntimeError, match="not configured"):
        BenchmarkRunner(settings, MemoryStore()).run(BenchmarkRunRequest(events=1))


def test_runner_enforces_event_limit(tmp_path: Path):
    settings = Settings(repository_root=tmp_path, benchmark_max_events=10,
                        benchmark_executable=str(tmp_path / "bench.exe"))
    with pytest.raises(ValueError, match="maximum"):
        BenchmarkRunner(settings, MemoryStore()).run(BenchmarkRunRequest(events=11))
