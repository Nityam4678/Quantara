from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass
from pathlib import Path

from langchain_core.documents import Document
from langchain_text_splitters import MarkdownHeaderTextSplitter, RecursiveCharacterTextSplitter
from tree_sitter import Language, Node, Parser
import tree_sitter_cpp

from .models import Chunk


CODE_EXTENSIONS = {".cpp", ".hpp", ".h", ".cc", ".cxx", ".cmake"}
DOC_EXTENSIONS = {".md", ".txt"}


def content_hash(content: str) -> str:
    return hashlib.sha256(content.encode("utf-8")).hexdigest()


@dataclass(frozen=True)
class _Region:
    start_byte: int
    end_byte: int
    start_line: int
    end_line: int
    symbol: str | None
    class_name: str | None
    namespace: str | None
    parent_symbol: str | None
    kind: str
    context: str


def _document(content: str, path: str, metadata: dict[str, object]) -> Document:
    return Document(page_content=content, metadata={"path": path, **metadata})


def _make_chunk(path: Path, root: Path, document: Document, content: str,
                start: int, end: int, language: str, symbol: str | None,
                class_name: str | None = None, namespace: str | None = None,
                parent_symbol: str | None = None, kind: str = "section",
                declaration: str | None = None) -> Chunk:
    relative = path.relative_to(root).as_posix()
    digest = content_hash(content)
    metadata = "|".join(filter(None, (relative, symbol, class_name, namespace,
                                      str(start), str(end), digest)))
    chunk_id = hashlib.sha256(metadata.encode("utf-8")).hexdigest()
    return Chunk(
        chunk_id=chunk_id, path=relative, start_line=start, end_line=end,
        symbol=symbol, class_name=class_name, namespace=namespace,
        parent_symbol=parent_symbol, declaration=declaration, chunk_kind=kind,
        language=language,
        content=content, content_hash=digest,
    )


def _line_numbers(content: str, start_byte: int, end_byte: int) -> tuple[int, int]:
    prefix = content.encode("utf-8")[:start_byte].decode("utf-8", errors="ignore")
    body = content.encode("utf-8")[start_byte:end_byte].decode("utf-8", errors="ignore")
    return prefix.count("\n") + 1, prefix.count("\n") + max(1, body.count("\n") + 1)


def _node_text(content: str, node: Node) -> str:
    return content.encode("utf-8")[node.start_byte:node.end_byte].decode("utf-8", errors="replace")


def _qualified_name(content: str, node: Node) -> str:
    declarator = node.child_by_field_name("declarator")
    if declarator is None:
        return _node_text(content, node).split("{", 1)[0].strip()
    return _node_text(content, declarator).strip()


def _context_text(namespace: str | None, class_name: str | None) -> str:
    scope = "::".join(filter(None, (namespace, class_name)))
    return f"Context: {scope}\n" if scope else ""


def _walk_cpp(node: Node, content: str, namespace: str | None = None,
              class_name: str | None = None, parent_symbol: str | None = None
              ) -> list[_Region]:
    regions: list[_Region] = []
    current_namespace, current_class = namespace, class_name
    if node.type == "namespace_definition":
        name = node.child_by_field_name("name")
        current_namespace = _node_text(content, name) if name else namespace
        regions.append(_region(node, content, "::".join(filter(None, (namespace, current_namespace))),
                               None, current_namespace, parent_symbol, "namespace"))
    elif node.type in {"class_specifier", "struct_specifier"}:
        name = node.child_by_field_name("name")
        current_class = _node_text(content, name) if name else class_name
        regions.append(_region(node, content, current_class, current_class,
                               namespace, parent_symbol, "class"))
    elif node.type == "function_definition":
        symbol = _qualified_name(content, node)
        qualified_parts = symbol.split("::")
        inferred_class = current_class or (
            qualified_parts[-2] if len(qualified_parts) > 1 else None)
        inferred_namespace = namespace or (
            "::".join(qualified_parts[:-2]) if len(qualified_parts) > 2 else None)
        regions.append(_region(node, content, symbol, inferred_class, inferred_namespace,
                               parent_symbol, "function"))
        parent_symbol = symbol

    for child in node.children:
        regions.extend(_walk_cpp(child, content, current_namespace, current_class, parent_symbol))
    return regions


def _declarations(node: Node, content: str) -> list[str]:
    result: list[str] = []
    if node.type in {"declaration", "field_declaration"}:
        text = _node_text(content, node).strip()
        if "(" in text and text.endswith(";"):
            result.append(text)
    for child in node.children:
        result.extend(_declarations(child, content))
    return result


def _region(node: Node, content: str, symbol: str | None, class_name: str | None,
            namespace: str | None, parent_symbol: str | None, kind: str) -> _Region:
    start, end = _line_numbers(content, node.start_byte, node.end_byte)
    return _Region(node.start_byte, node.end_byte, start, end, symbol, class_name,
                   namespace, parent_symbol, kind, _context_text(namespace, class_name))


def _split_large_region(region: _Region, content: str, max_chars: int) -> list[tuple[int, int]]:
    raw = content.encode("utf-8")[region.start_byte:region.end_byte].decode("utf-8", errors="replace")
    if len(raw) <= max_chars:
        return [(region.start_byte, region.end_byte)]
    # Split only at complete top-level statements/blocks. The final fallback is
    # line-based but still covers every byte and never crosses the function end.
    parser = Parser(Language(tree_sitter_cpp.language()))
    tree = parser.parse(raw.encode("utf-8"))
    function_node = next((child for child in tree.root_node.children
                          if child.type == "function_definition"), tree.root_node)
    body_node = function_node.child_by_field_name("body")
    boundary_node = body_node if body_node is not None else function_node
    boundaries = [child.end_byte for child in boundary_node.children
                  if child.end_byte > child.start_byte]
    points = [0] + boundaries + [len(raw.encode("utf-8"))]
    result: list[tuple[int, int]] = []
    begin = 0
    for point in points[1:]:
        if point - begin > max_chars and point > begin:
            result.append((region.start_byte + begin, region.start_byte + point))
            begin = point
    if begin < len(raw.encode("utf-8")):
        result.append((region.start_byte + begin, region.end_byte))
    return result or [(region.start_byte, region.end_byte)]


def _cpp_documents(path: Path, root: Path, content: str, max_chars: int) -> list[Chunk]:
    parser = Parser(Language(tree_sitter_cpp.language()))
    tree = parser.parse(content.encode("utf-8"))
    regions = _walk_cpp(tree.root_node, content)
    declarations = _declarations(tree.root_node, content)
    function_regions = [region for region in regions if region.kind == "function"]
    covered_lines = {(line, region.end_line)
                     for region in function_regions
                     for line in range(region.start_line, region.end_line + 1)}
    chunks: list[Chunk] = []
    for region in function_regions:
        for start_byte, end_byte in _split_large_region(region, content, max_chars):
            body = content.encode("utf-8")[start_byte:end_byte].decode("utf-8", errors="replace")
            start, end = _line_numbers(content, start_byte, end_byte)
            function_name = (region.symbol or "").split("::")[-1].split("(", 1)[0]
            declaration = next(
                (item for item in declarations
                 if re.search(rf"\b{re.escape(function_name)}\s*\(", item)),
                None,
            )
            context = region.context
            if declaration:
                context += f"Declaration: {declaration}\n"
            chunks.append(_make_chunk(
                path, root, _document(body, path.relative_to(root).as_posix(),
                                       {"symbol": region.symbol}), context + body,
                start, end, "cpp", region.symbol, region.class_name, region.namespace,
                region.parent_symbol, "function", declaration))

    # Preserve declarations, comments, includes, and class/file context not
    # covered by function implementations.
    raw_lines = content.splitlines(keepends=True)
    uncovered: list[int] = []
    for line_number, line in enumerate(raw_lines, start=1):
        if not any(start <= line_number <= end for start, end in covered_lines):
            uncovered.append(line_number)
    if uncovered:
        begin = previous = uncovered[0]
        for line_number in uncovered[1:] + [None]:
            if line_number is None or line_number != previous + 1:
                body = "".join(raw_lines[begin - 1:previous])
                if body.strip():
                    chunks.append(_make_chunk(path, root, _document(body, "", {}), body,
                                              begin, previous, "cpp", None, None, None,
                                              None, "context"))
                if line_number is not None:
                    begin = line_number
            previous = line_number if line_number is not None else previous
    return _deduplicate(chunks)


def _deduplicate(chunks: list[Chunk]) -> list[Chunk]:
    seen: set[str] = set()
    result = []
    for chunk in sorted(chunks, key=lambda item: (item.start_line, item.end_line, item.chunk_kind)):
        if chunk.chunk_id not in seen and chunk.content.strip():
            seen.add(chunk.chunk_id)
            result.append(chunk)
    return result


def chunk_cpp(path: Path, root: Path, content: str, max_chars: int = 12000) -> list[Chunk]:
    return _cpp_documents(path, root, content, max_chars)


def chunk_markdown(path: Path, root: Path, content: str, chunk_size: int = 6000,
                   chunk_overlap: int = 200) -> list[Chunk]:
    relative = path.relative_to(root).as_posix()
    source = _document(content, relative, {"language": "markdown"})
    header_splitter = MarkdownHeaderTextSplitter(
        headers_to_split_on=[("#", "h1"), ("##", "h2"), ("###", "h3"),
                             ("####", "h4"), ("#####", "h5"), ("######", "h6")],
        strip_headers=False,
    )
    sections = header_splitter.split_text(content)
    secondary = RecursiveCharacterTextSplitter(
        chunk_size=chunk_size, chunk_overlap=chunk_overlap,
        separators=["\n\n", "\n", " ", ""],
    )
    lines = content.splitlines(keepends=True)
    cursor = 0
    chunks: list[Chunk] = []
    for section in sections:
        section_text = section.page_content
        position = content.find(section_text, cursor)
        if position < 0:
            position = cursor
        start = content[:position].count("\n") + 1
        section_docs = secondary.split_documents([section])
        for part in section_docs:
            part_position = content.find(part.page_content, position)
            if part_position < 0:
                part_position = position
            part_start = content[:part_position].count("\n") + 1
            part_end = part_start + max(0, part.page_content.count("\n"))
            headings = re.findall(r"^#{1,6}\s+(.+?)\s*$", section.page_content, re.MULTILINE)
            heading = headings[-1] if headings else None
            chunks.append(_make_chunk(
                path, root, part, part.page_content, part_start, max(start, part_end),
                "markdown", heading, kind="markdown"))
            position = part_position + len(part.page_content)
        cursor = max(cursor, position)
    return _deduplicate(chunks) if chunks else (
        [_make_chunk(path, root, source, content, 1, len(lines), "markdown", None, kind="markdown")]
        if content else []
    )


def chunk_file(path: Path, root: Path, content: str, **options: int) -> list[Chunk]:
    if path.suffix.lower() in DOC_EXTENSIONS:
        return chunk_markdown(
            path, root, content,
            chunk_size=options.get("chunk_size", 6000),
            chunk_overlap=options.get("chunk_overlap", 200))
    return chunk_cpp(path, root, content, max_chars=options.get("max_chars", 12000))
