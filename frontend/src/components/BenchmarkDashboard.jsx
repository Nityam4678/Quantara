import { useEffect, useMemo, useState } from 'react';

function BenchmarkDashboard() {
  const [records, setRecords] = useState([]);
  const [question, setQuestion] = useState('');
  const [analysis, setAnalysis] = useState(null);
  const [running, setRunning] = useState(false);
  const [error, setError] = useState('');

  const load = () => fetch('/api/ai/benchmarks?limit=100')
    .then(response => response.json())
    .then(body => setRecords(body.results || []))
    .catch(() => setError('Benchmark history is unavailable'));
  useEffect(() => { load(); }, []);

  const latest = useMemo(() => {
    const map = new Map();
    records.forEach(record => { if (!map.has(record.workload)) map.set(record.workload, record); });
    return [...map.values()];
  }, [records]);

  async function run() {
    setRunning(true); setError('');
    try {
      const response = await fetch('/api/ai/benchmarks/run', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ events: 10000, seed: 42, sigma: 12 })
      });
      if (!response.ok) throw new Error();
      await response.json(); await load();
    } catch { setError('Benchmark execution failed'); }
    finally { setRunning(false); }
  }

  async function ask(event) {
    event.preventDefault();
    const response = await fetch('/api/ai/benchmarks/analyze', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ question })
    });
    if (response.ok) setAnalysis(await response.json());
    else setError('Performance analysis failed');
  }

  return <section className="assistant-panel benchmark-panel">
    <div className="section-heading"><div><p className="eyebrow">MEASURED PERFORMANCE</p><h2>Benchmark intelligence</h2></div><button className="primary-button" onClick={run} disabled={running}>{running ? 'Running…' : 'Run 10k experiment'}</button></div>
    {error && <div className="error-banner">{error}</div>}
    <div className="benchmark-grid">
      {latest.map(record => <div className="benchmark-card" key={record.workload}>
        <span className="muted">{record.workload}</span>
        <strong>{record.throughput_events_sec.toLocaleString()} <small>events/sec</small></strong>
        <div className="metric-row"><span>Mean latency</span><b>{record.average_latency_ns.toFixed(1)} ns</b></div>
        <div className="metric-row"><span>p99</span><b>{record.p99_latency_ns?.toFixed(1)} ns</b></div>
        <div className="metric-row"><span>p99.9</span><b>{record.p999_latency_ns?.toFixed(1)} ns</b></div>
      </div>)}
      {!latest.length && <div className="empty-state">No measured benchmark runs yet.</div>}
    </div>
    <div className="benchmark-history">
      <h3>Historical experiments</h3>
      <div className="table-scroll"><table><thead><tr><th>Workload</th><th>Events</th><th>Throughput</th><th>p99</th><th>Machine</th></tr></thead><tbody>
        {records.map((record, index) => <tr key={`${record.created_at}-${record.workload}-${index}`}><td>{record.workload}</td><td>{record.event_count.toLocaleString()}</td><td>{record.throughput_events_sec.toLocaleString()}</td><td>{record.p99_latency_ns?.toFixed(1)} ns</td><td>{record.machine}</td></tr>)}
      </tbody></table></div>
    </div>
    <form className="analysis-form" onSubmit={ask}><input value={question} onChange={event => setQuestion(event.target.value)} placeholder="Why is deep-book throughput lower?" /><button className="secondary-button">Analyze measurements</button></form>
    {analysis && <div className="analysis-result"><h3>Measured facts and hypotheses</h3><pre>{analysis.answer}</pre><div className="citation-list">{analysis.evidence?.map(citation => <span key={`${citation.path}-${citation.start_line}`}>{citation.path}:{citation.start_line}-{citation.end_line}</span>)}</div></div>}
  </section>;
}

export { BenchmarkDashboard };
