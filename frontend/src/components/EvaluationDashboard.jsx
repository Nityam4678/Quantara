import { useState } from 'react';
import { BarChart3, Play } from 'lucide-react';
import { Panel } from './Panel';

export function EvaluationDashboard() {
  const [report, setReport] = useState(null);
  const [running, setRunning] = useState(false);
  const [error, setError] = useState('');

  const run = async () => {
    setRunning(true);
    setError('');
    try {
      const response = await fetch('/api/ai/evaluate', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ k: 5 })
      });
      const body = await response.json();
      if (!response.ok) throw new Error(body.error?.message || 'Evaluation failed');
      setReport(body);
    } catch (reason) {
      setError(reason.message);
    } finally {
      setRunning(false);
    }
  };

  return <Panel title="RAG evaluation" eyebrow="Retrieval quality" action={
    <button className="assistant-ingest" disabled={running} onClick={run}>
      <Play size={13} /> {running ? 'Evaluating…' : 'Run 30 questions'}
    </button>
  }>
    {!report && !error && <p className="muted">Run the representative evaluation dataset to compare vector, keyword, hybrid, and reranked retrieval.</p>}
    {error && <p className="assistant-notice">{error}</p>}
    {report && <div className="evaluation-table">
      <div className="evaluation-row evaluation-header"><span>Strategy</span><span>Recall@5</span><span>MRR</span><span>nDCG</span><span>Latency</span></div>
      {report.metrics.map(metric => <div className="evaluation-row" key={metric.strategy}>
        <span><BarChart3 size={13} />{metric.strategy}</span>
        <span>{metric.recall_at_k.toFixed(3)}</span>
        <span>{metric.mrr.toFixed(3)}</span>
        <span>{metric.ndcg.toFixed(3)}</span>
        <span>{metric.retrieval_latency_ms.toFixed(1)} ms</span>
      </div>)}
      <small className="muted">Measured at {new Date(report.generated_at).toLocaleString()} · {report.metrics[0]?.questions} questions</small>
    </div>}
  </Panel>;
}
