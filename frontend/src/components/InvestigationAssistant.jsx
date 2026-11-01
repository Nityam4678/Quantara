import { useState } from 'react';
import { ChevronDown, Search, ShieldCheck } from 'lucide-react';
import { Panel, EmptyState } from './Panel';

export function InvestigationAssistant() {
  const [question, setQuestion] = useState('');
  const [result, setResult] = useState(null);
  const [loading, setLoading] = useState(false);
  const [approved, setApproved] = useState(false);
  const [notice, setNotice] = useState('');

  const investigate = async event => {
    event.preventDefault();
    if (!question.trim() || loading) return;
    setLoading(true);
    setNotice('');
    try {
      const response = await fetch('/api/ai/agent', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ question: question.trim(), approved })
      });
      const body = await response.json();
      if (!response.ok) throw new Error(body.error?.message || 'Investigation failed');
      setResult(body);
    } catch (error) {
      setNotice(error.message);
    } finally {
      setLoading(false);
    }
  };

  return <Panel title="AI investigation assistant" eyebrow="Controlled agent">
    <div className="assistant-intro"><div className="assistant-avatar"><Search size={16} /></div><div><strong>Investigate the actual engine</strong><p>Combines live C++ engine state with cited source evidence.</p></div></div>
    {!result && !loading && <EmptyState>Try “Why was order 7001 partially filled?”</EmptyState>}
    {loading && <div className="chat-message assistant"><div className="chat-bubble typing">Selecting controlled tools…</div></div>}
    {result && <div className="investigation-result">
      <div className="chat-bubble">{result.answer}</div>
      <p className="investigation-summary"><ShieldCheck size={13} /> {result.reasoning_summary}</p>
      {result.approval_required && <p className="assistant-notice">Simulation requires approval and was not executed.</p>}
      <h4>Tools called</h4>
      <div className="tool-list">{result.tools.map((tool, index) => <details key={`${tool.name}-${index}`}><summary><code>{tool.name}</code><span>{tool.status}</span></summary><pre>{JSON.stringify(tool.output || tool.error, null, 2)}</pre></details>)}</div>
      {result.evidence?.length > 0 && <><h4>Retrieved evidence</h4><div className="citations">{result.evidence.map(citation => <div className="citation" key={`${citation.path}-${citation.start_line}`}><button><span><code>{citation.path}:{citation.start_line}-{citation.end_line}</code>{citation.symbol && <small>{citation.symbol}</small>}</span><ChevronDown size={14} /></button><pre>{citation.snippet}</pre></div>)}</div></>}
    </div>}
    {notice && <p className="assistant-notice">{notice}</p>}
    <label className="approval-toggle"><input type="checkbox" checked={approved} onChange={event => setApproved(event.target.checked)} /> Approve state-changing simulations</label>
    <form className="assistant-form" onSubmit={investigate}><input value={question} onChange={event => setQuestion(event.target.value)} placeholder="Ask why an engine event happened…" /><button disabled={!question.trim() || loading}><Search size={15} /></button></form>
  </Panel>;
}
