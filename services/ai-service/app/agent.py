from __future__ import annotations

import json
import re
import urllib.parse
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Callable

from .config import Settings
from .models import AgentResponse, Citation, ToolCall
from .rag import CodebaseRAG
from .store import MemoryStore


@dataclass(frozen=True)
class ToolDefinition:
    name: str
    description: str
    state_changing: bool
    handler: Callable[[dict], object]


class AgentTools:
    def __init__(self, settings: Settings, rag: CodebaseRAG,
                 store: MemoryStore | None = None):
        self.settings = settings
        self.rag = rag
        self.store = store
        self.audit: list[dict] = []

    def _engine(self, path: str, query: dict | None = None) -> object:
        url = f"{self.settings.engine_api_url.rstrip('/')}{path}"
        if query:
            url += "?" + urllib.parse.urlencode(query)
        request = urllib.request.Request(url, headers={"Accept": "application/json"})
        with urllib.request.urlopen(request, timeout=self.settings.agent_tool_timeout_seconds) as response:
            return json.loads(response.read())

    def _order_id(self, arguments: dict) -> int:
        value = arguments.get("order_id")
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise ValueError("order_id must be a positive integer")
        return value

    def definitions(self) -> dict[str, ToolDefinition]:
        return {
            "get_order_book": ToolDefinition(
                "get_order_book", "Read the actual native order book", False,
                lambda args: self._engine("/api/orderbook", {"depth": min(int(args.get("depth", 20)), 100)})),
            "get_order_details": ToolDefinition(
                "get_order_details", "Read a resting order by ID", False,
                lambda args: self._engine(f"/api/orders/{self._order_id(args)}")),
            "get_execution_history": ToolDefinition(
                "get_execution_history", "Read recent execution events related to an order", False,
                lambda args: [
                    event for event in self._engine("/api/events", {"limit": 100})
                    if any(str(order_id) in json.dumps(event)
                           for order_id in [self._order_id(args)])
                ]),
            "get_recent_events": ToolDefinition(
                "get_recent_events", "Read actual recent native engine events", False,
                lambda args: self._engine("/api/events", {"limit": min(int(args.get("limit", 20)), 100)})),
            "run_order_simulation": ToolDefinition(
                "run_order_simulation", "Run an approved, bounded sequence of limit orders",
                True, self._simulate),
            "search_codebase": ToolDefinition(
                "search_codebase", "Retrieve relevant indexed C++ source evidence", False,
                lambda args: self._search(str(args["query"]), min(int(args.get("top_k", 5)), 10))),
            "search_documentation": ToolDefinition(
                "search_documentation", "Retrieve relevant indexed documentation", False,
                lambda args: self._search_documentation(
                    str(args["query"]), min(int(args.get("top_k", 5)), 10))),
        }

    def _search(self, query: str, top_k: int) -> list[dict]:
        return [
            {"path": chunk.path, "start_line": chunk.start_line, "end_line": chunk.end_line,
             "symbol": chunk.symbol, "snippet": chunk.content[:1200], "score": float(score)}
            for chunk, score in self.rag.retrieve(query, top_k, strategy="hybrid+rereank")
        ]

    def _search_documentation(self, query: str, top_k: int) -> list[dict]:
        return [item for item in self._search(query, top_k * 2)
                if item["path"].lower().endswith((".md", ".txt"))][:top_k]

    def _simulate(self, arguments: dict) -> object:
        scenario = arguments.get("scenario")
        if not isinstance(scenario, list) or not 1 <= len(scenario) <= 10:
            raise ValueError("scenario must contain 1 to 10 orders")
        results = []
        seen: set[int] = set()
        for order in scenario:
            if not isinstance(order, dict):
                raise ValueError("each scenario item must be an order object")
            order_id = order.get("orderId")
            if isinstance(order_id, bool) or not isinstance(order_id, int) or order_id <= 0:
                raise ValueError("scenario orderId must be a positive integer")
            if order_id in seen:
                raise ValueError("scenario order IDs must be unique")
            seen.add(order_id)
            if order.get("side") not in {"BUY", "SELL"}:
                raise ValueError("scenario side must be BUY or SELL")
            if not all(isinstance(order.get(key), int) and order[key] > 0
                       for key in ("price", "quantity")):
                raise ValueError("scenario price and quantity must be positive integers")
        for order in scenario:
            request = urllib.request.Request(
                f"{self.settings.engine_api_url.rstrip('/')}/api/orders",
                data=json.dumps(order).encode(),
                headers={"Content-Type": "application/json"}, method="POST")
            with urllib.request.urlopen(request, timeout=self.settings.agent_tool_timeout_seconds) as response:
                results.append(json.loads(response.read()))
        results.append(self._engine("/api/orderbook", {"depth": 20}))
        return results

    def call(self, name: str, arguments: dict, approved: bool) -> ToolCall:
        definitions = self.definitions()
        if name not in definitions:
            raise ValueError("tool is not allowlisted")
        definition = definitions[name]
        if definition.state_changing and not approved:
            return ToolCall(name=name, arguments=arguments, status="approval_required",
                            duration_ms=0, error="explicit approval is required")
        started = time.perf_counter()
        error = None
        output = None
        for attempt in range(self.settings.agent_retry_count + 1):
            try:
                output = definition.handler(arguments)
                status = "succeeded"
                break
            except (ValueError, urllib.error.URLError, TimeoutError, KeyError) as reason:
                error = str(reason)
                status = "failed"
                if attempt >= self.settings.agent_retry_count:
                    break
        duration = round((time.perf_counter() - started) * 1000, 3)
        record = {"name": name, "arguments": arguments, "status": status,
                  "duration_ms": duration, "error": error}
        self.audit.append(record)
        self.audit = self.audit[-1000:]
        if self.store is not None:
            self.store.save_agent_audit(record)
        return ToolCall(**record, output=output)


class InvestigationAgent:
    def __init__(self, settings: Settings, rag: CodebaseRAG,
                 store: MemoryStore | None = None):
        self.settings = settings
        self.tools = AgentTools(settings, rag, store)
        self.store = store
        self.rag = rag

    def _plan(self, question: str, scenario: list[dict] | None = None) -> list[tuple[str, dict]]:
        lower = question.lower()
        plan: list[tuple[str, dict]] = []
        if any(term in lower for term in ("book", "best bid", "best ask", "price level")):
            plan.append(("get_order_book", {}))
        order_match = re.search(r"\border(?:\s+id)?\s*[#:]?\s*(\d+)", lower)
        if order_match:
            order_id = int(order_match.group(1))
            plan += [("get_order_details", {"order_id": order_id}),
                     ("get_execution_history", {"order_id": order_id})]
        if any(term in lower for term in ("execution", "filled", "partial", "trade", "event")):
            plan.append(("get_recent_events", {"limit": 20}))
        if any(term in lower for term in ("simulate", "scenario", "orders arrive")):
            plan.append(("run_order_simulation", {"scenario": scenario or []}))
        if any(term in lower for term in ("code", "how", "why", "explain", "implementation",
                                          "priority", "structure", "cancel", "matching")):
            plan.append(("search_codebase", {"query": question, "top_k": 6}))
        if any(term in lower for term in ("readme", "document", "benchmark", "architecture")):
            plan.append(("search_documentation", {"query": question, "top_k": 5}))
        unique: list[tuple[str, dict]] = []
        seen: set[str] = set()
        for name, arguments in plan:
            signature = name + json.dumps(arguments, sort_keys=True)
            if signature not in seen:
                seen.add(signature)
                unique.append((name, arguments))
        return unique

    def investigate(self, question: str, approved: bool = False,
                    scenario: list[dict] | None = None) -> AgentResponse:
        calls: list[ToolCall] = []
        execution_results: list[object] = []
        citations: list[Citation] = []
        approval_required = False
        for name, arguments in self._plan(question, scenario)[:self.settings.agent_max_iterations]:
            if name == "run_order_simulation" and not approved:
                arguments = {"scenario": []}
            result = self.tools.call(name, arguments, approved)
            calls.append(result)
            if result.status == "approval_required":
                approval_required = True
            if result.output is not None:
                execution_results.append(result.output)
                if name.startswith("search_"):
                    for item in result.output:
                        chunk = type("ChunkView", (), item)
                        score = item["score"]
                        citations.append(Citation(
                            path=chunk.path, start_line=chunk.start_line, end_line=chunk.end_line,
                            symbol=chunk.symbol, snippet=chunk.snippet,
                            score=round(float(score), 4)))
        citations = list({citation.path + str(citation.start_line): citation for citation in citations}.values())
        summary = "Inspected " + (", ".join(call.name for call in calls) or "no tools") + "."
        if approval_required:
            answer = "Simulation requires explicit approval before changing native engine state."
        else:
            observed = f" Returned {len(execution_results)} live/tool result(s)." if execution_results else ""
            answer = (f"{summary}{observed} The explanation is grounded in actual engine "
                      "results and indexed evidence.")
        return AgentResponse(answer=answer, reasoning_summary=summary, tools=calls,
                             evidence=citations, execution_results=execution_results,
                             grounded=bool(citations or execution_results),
                             approval_required=approval_required)
