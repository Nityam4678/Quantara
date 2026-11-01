from pathlib import Path

from app.chunking import chunk_cpp, chunk_markdown, content_hash
from app.embeddings import EmbeddingProvider
from app.rag import CodebaseRAG
from app.store import MemoryStore
from app.config import Settings
from app.evaluation import evaluate
from app.retrieval import understand_query, reciprocal_rank_fusion
from app.agent import InvestigationAgent


def test_cpp_chunking_preserves_symbols_and_lines(tmp_path: Path):
    source = "namespace lob {\nclass Book {\npublic:\n void add() {}\n};\n}\n"
    path = tmp_path / "book.hpp"
    path.write_text(source)
    chunks = chunk_cpp(path, tmp_path, source)
    assert chunks
    assert any(chunk.symbol for chunk in chunks)
    assert chunks[0].start_line == 1
    assert chunks[-1].end_line == 6


def test_cpp_function_boundaries_and_scope_metadata(tmp_path: Path):
    source = (
        "namespace lob {\n"
        "class Book {\n"
        "public:\n"
        "  void add();\n"
        "};\n"
        "}\n"
        "void lob::Book::add() { int value = 1; }\n"
        "void Book::cancel() { int removed = 1; }\n"
    )
    chunks = chunk_cpp(tmp_path / "book.cpp", tmp_path, source)
    functions = [chunk for chunk in chunks if chunk.chunk_kind == "function"]
    assert {chunk.symbol for chunk in functions} == {"lob::Book::add()", "Book::cancel()"}
    add = next(chunk for chunk in functions if chunk.symbol == "lob::Book::add()")
    assert add.class_name == "Book"
    assert add.namespace == "lob"
    assert add.start_line == 7 and add.end_line == 7
    assert add.declaration == "void add();"
    assert "void Book::cancel()" in next(
        chunk.content for chunk in functions if chunk.symbol == "Book::cancel()")


def test_cpp_large_function_subchunks_cover_source_without_duplicates(tmp_path: Path):
    source = "void run() {\n" + "".join(f"  step_{index}();\n" for index in range(20)) + "}\n"
    chunks = chunk_cpp(tmp_path / "large.cpp", tmp_path, source, max_chars=80)
    functions = [chunk for chunk in chunks if chunk.chunk_kind == "function"]
    assert len(functions) > 1
    assert all(chunk.start_line <= chunk.end_line for chunk in functions)
    assert len({chunk.chunk_id for chunk in chunks}) == len(chunks)
    assert all(f"step_{index}();" in "\n".join(chunk.content for chunk in functions)
               for index in range(20))


def test_markdown_chunking_follows_headings(tmp_path: Path):
    source = "# One\nalpha\n## Two\nbeta\n"
    path = tmp_path / "README.md"
    chunks = chunk_markdown(path, tmp_path, source)
    assert [chunk.symbol for chunk in chunks] == ["One", "Two"]
    assert chunks[1].start_line == 3


def test_markdown_oversized_sections_keep_all_content(tmp_path: Path):
    source = "# One\n" + ("alpha beta gamma\n" * 20) + "## Two\nomega\n"
    chunks = chunk_markdown(tmp_path / "README.md", tmp_path, source,
                            chunk_size=40, chunk_overlap=0)
    assert len(chunks) > 2
    assert any("omega" in chunk.content for chunk in chunks)
    assert all(chunk.start_line <= chunk.end_line for chunk in chunks)


def test_embedding_is_deterministic_and_normalized():
    provider = EmbeddingProvider(dimensions=32)
    first = provider.embed_one("OrderBook price level")
    assert first == provider.embed_one("OrderBook price level")
    assert len(first) == 32


def test_incremental_ingestion_and_retrieval(tmp_path: Path):
    (tmp_path / "order_book.hpp").write_text("class OrderBook { void cancel_order(); };\n")
    settings = Settings(repository_root=tmp_path, embedding_dimensions=32)
    rag = CodebaseRAG(settings, MemoryStore(), EmbeddingProvider(dimensions=32))
    first = rag.ingest()
    second = rag.ingest()
    assert first.changed == 1
    assert second.unchanged == 1
    assert rag.retrieve("cancel order")


def test_generation_abstains_without_evidence(tmp_path: Path):
    settings = Settings(repository_root=tmp_path, embedding_dimensions=32)
    rag = CodebaseRAG(settings, MemoryStore(), EmbeddingProvider(dimensions=32))
    answer = rag.answer("What does the matching engine do?")
    assert answer.grounded is False
    assert "insufficient" in answer.answer


def test_query_understanding_preserves_cpp_identifiers():
    plan = understand_query("How does OrderBook::cancel_order affect the price level?")
    assert "OrderBook::cancel_order" in plan.identifiers
    assert "cancellation" in plan.concepts
    assert "order lifecycle" in plan.expanded


def test_rrf_combines_vector_and_keyword_results(tmp_path: Path):
    source = "void cancel_order() { }\n"
    chunk = chunk_cpp(tmp_path / "book.cpp", tmp_path, source)[0]
    fused = reciprocal_rank_fusion([[(chunk, 0.9)], [(chunk, 1.0)]], [0.65, 0.35])
    assert len(fused) == 1
    assert fused[0][0].chunk_id == chunk.chunk_id


def test_keyword_search_supports_exact_cpp_identifiers(tmp_path: Path):
    source = "void cancel_order() { }\n"
    (tmp_path / "book.cpp").write_text(source)
    settings = Settings(repository_root=tmp_path, embedding_dimensions=32)
    rag = CodebaseRAG(settings, MemoryStore(), EmbeddingProvider(dimensions=32))
    rag.ingest()
    results = rag.retrieve("OrderBook::cancel_order", 1, strategy="keyword")
    assert results and results[0][0].symbol == "cancel_order()"


def test_evaluation_reports_all_retrieval_strategies(tmp_path: Path):
    (tmp_path / "order_book.cpp").write_text(
        "void cancel_order() { }\nvoid add_order() { }\n")
    settings = Settings(repository_root=tmp_path, embedding_dimensions=32)
    rag = CodebaseRAG(settings, MemoryStore(), EmbeddingProvider(dimensions=32))
    rag.ingest()
    report = evaluate(rag, 3)
    assert {metric.strategy for metric in report.metrics} == {
        "vector", "keyword", "hybrid", "hybrid+rereank"}
    assert all(metric.questions == 30 for metric in report.metrics)


def test_agent_requires_approval_for_simulation(tmp_path: Path):
    settings = Settings(repository_root=tmp_path, embedding_dimensions=32)
    rag = CodebaseRAG(settings, MemoryStore(), EmbeddingProvider(dimensions=32))
    result = InvestigationAgent(settings, rag).investigate(
        "Simulate five orders and explain the resulting order book")
    assert result.approval_required is True
    simulation = next(tool for tool in result.tools if tool.name == "run_order_simulation")
    assert simulation.status == "approval_required"


def test_agent_plan_is_allowlisted_and_bounded(tmp_path: Path):
    settings = Settings(repository_root=tmp_path, embedding_dimensions=32,
                        agent_max_iterations=2)
    rag = CodebaseRAG(settings, MemoryStore(), EmbeddingProvider(dimensions=32))
    agent = InvestigationAgent(settings, rag)
    plan = agent._plan("Why was order 42 partially filled?")
    assert len(plan) == 4
    assert {name for name, _ in plan} <= {
        "get_order_book", "get_order_details", "get_execution_history",
        "get_recent_events", "run_order_simulation", "search_codebase",
        "search_documentation"}
