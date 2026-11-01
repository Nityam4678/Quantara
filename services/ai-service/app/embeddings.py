from __future__ import annotations

import hashlib
import json
import math
import urllib.request
from collections import Counter

from langchain_core.embeddings import Embeddings
from langchain_openai import OpenAIEmbeddings

def _local_embedding(text: str, dimensions: int) -> list[float]:
    """Deterministic offline fallback used by tests and local development."""
    values = [0.0] * dimensions
    tokens = text.lower().split()
    for token in tokens:
        index = int(hashlib.sha256(token.encode()).hexdigest()[:12], 16) % dimensions
        values[index] += 1.0
    norm = math.sqrt(sum(value * value for value in values)) or 1.0
    return [value / norm for value in values]


class EmbeddingProvider(Embeddings):
    def __init__(self, url: str = "", api_key: str = "", model: str = "local",
                 dimensions: int = 256):
        self.url, self.api_key, self.model, self.dimensions = url, api_key, model, dimensions
        self._langchain_backend = None
        if url and api_key:
            base_url = url.removesuffix("/embeddings")
            self._langchain_backend = OpenAIEmbeddings(
                model=model, api_key=api_key, dimensions=dimensions, base_url=base_url)

    def embed(self, texts: list[str]) -> list[list[float]]:
        if self._langchain_backend is not None:
            return self._langchain_backend.embed_documents(texts)
        if not self.url:
            return [_local_embedding(text, self.dimensions) for text in texts]
        payload = json.dumps({"model": self.model, "input": texts}).encode()
        request = urllib.request.Request(
            self.url, data=payload, headers={
                "Content-Type": "application/json",
                **({"Authorization": f"Bearer {self.api_key}"} if self.api_key else {}),
            })
        with urllib.request.urlopen(request, timeout=30) as response:
            body = json.loads(response.read())
        return [item["embedding"] for item in body["data"]]

    def embed_one(self, text: str) -> list[float]:
        return self.embed([text])[0]

    def embed_documents(self, texts: list[str]) -> list[list[float]]:
        return self.embed(texts)

    def embed_query(self, text: str) -> list[float]:
        return self.embed_one(text)
