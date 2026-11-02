const { AppError } = require('./errors');

class AiClient {
  constructor(baseUrl = process.env.AI_SERVICE_URL || 'http://127.0.0.1:8000') {
    this.baseUrl = baseUrl.replace(/\/$/, '');
  }

  async request(path, options = {}) {
    let response;
    try {
      response = await fetch(`${this.baseUrl}${path}`, {
        ...options,
        headers: {
          'Content-Type': 'application/json',
          ...(process.env.INTERNAL_SERVICE_TOKEN
            ? { 'X-Internal-Token': process.env.INTERNAL_SERVICE_TOKEN } : {}),
          ...(options.headers || {})
        }
      });
    } catch (error) {
      throw new AppError(503, 'AI_UNAVAILABLE', error.message);
    }
    if (!response.ok) {
      throw new AppError(502, 'AI_ERROR', await response.text());
    }
    return response;
  }

  async health() {
    return (await this.request('/health')).json();
  }

  async ingest() {
    return (await this.request('/ingest', { method: 'POST' })).json();
  }

  async retrieval(query) {
    const params = new URLSearchParams(query);
    return (await this.request(`/retrieval?${params}`)).json();
  }

  async evaluate(k) {
    return (await this.request(`/evaluate?k=${encodeURIComponent(k)}`, {
      method: 'POST'
    })).json();
  }

  async chat(payload) {
    return (await this.request('/chat', {
      method: 'POST',
      body: JSON.stringify(payload)
    })).json();
  }

  async stream(payload) {
    return this.request('/chat/stream', {
      method: 'POST',
      body: JSON.stringify(payload)
    });
  }

  async agent(payload) {
    return (await this.request('/agent', {
      method: 'POST',
      body: JSON.stringify(payload)
    })).json();
  }

  async agentStream(payload) {
    return this.request('/agent/stream', {
      method: 'POST',
      body: JSON.stringify(payload)
    });
  }

  async runBenchmark(payload) {
    return (await this.request('/benchmarks/run', {
      method: 'POST', body: JSON.stringify(payload)
    })).json();
  }

  async benchmarks(limit) {
    return (await this.request(`/benchmarks?limit=${encodeURIComponent(limit)}`)).json();
  }

  async benchmarkAnalysis(payload) {
    return (await this.request('/benchmarks/analyze', {
      method: 'POST', body: JSON.stringify(payload)
    })).json();
  }
}

module.exports = { AiClient };
