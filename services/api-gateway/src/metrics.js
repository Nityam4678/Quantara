function createMetrics() {
  const counters = new Map();
  const latencies = new Map();
  return {
    observe(route, durationMs) {
      counters.set(route, (counters.get(route) || 0) + 1);
      const values = latencies.get(route) || [];
      values.push(durationMs);
      latencies.set(route, values.slice(-1000));
    },
    snapshot() {
      return [...counters.keys()].sort().map(route => {
        const values = latencies.get(route) || [];
        return {
          route, requests: counters.get(route),
          averageMs: values.length ? values.reduce((a, b) => a + b, 0) / values.length : 0
        };
      });
    }
  };
}

module.exports = { createMetrics };
