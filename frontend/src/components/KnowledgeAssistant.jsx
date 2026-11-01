import { useMemo, useState } from 'react';
import { BookOpen, ChevronDown, Send, Sparkles } from 'lucide-react';
import { Panel, EmptyState } from './Panel';

export function KnowledgeAssistant() {
  const [question, setQuestion] = useState('');
  const [messages, setMessages] = useState([]);
  const [loading, setLoading] = useState(false);
  const [ingesting, setIngesting] = useState(false);
  const [notice, setNotice] = useState('');
  const [openCitation, setOpenCitation] = useState(null);
  const canSend = question.trim() && !loading;

  const ask = async event => {
    event.preventDefault();
    if (!canSend) return;
    const prompt = question.trim();
    setQuestion('');
    setMessages(previous => [...previous, { role: 'user', text: prompt }]);
    setLoading(true);
    setNotice('');
    try {
      const response = await fetch('/api/ai/chat', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ question: prompt })
      });
      const body = await response.json();
      if (!response.ok) throw new Error(body.error?.message || 'Assistant unavailable');
      setMessages(previous => [...previous, { role: 'assistant', ...body }]);
    } catch (error) {
      setNotice(error.message);
    } finally {
      setLoading(false);
    }
  };

  const ingest = async () => {
    setIngesting(true);
    setNotice('');
    try {
      const response = await fetch('/api/ai/ingest', { method: 'POST' });
      const body = await response.json();
      if (!response.ok) throw new Error(body.error?.message || 'Ingestion failed');
      setNotice(`Indexed ${body.chunks} chunks (${body.changed} changed, ${body.unchanged} unchanged).`);
    } catch (error) {
      setNotice(error.message);
    } finally {
      setIngesting(false);
    }
  };

  return <Panel title="Codebase knowledge assistant" eyebrow="Grounded RAG" action={
    <button className="assistant-ingest" disabled={ingesting} onClick={ingest}><BookOpen size={13} /> {ingesting ? 'Indexing…' : 'Sync repository'}</button>
  }>
    <div className="assistant-intro"><div className="assistant-avatar"><Sparkles size={16} /></div><div><strong>Ask about the matching engine</strong><p>Answers cite indexed C++ symbols, tests, and architecture documents.</p></div></div>
    <div className="assistant-messages">
      {!messages.length && <EmptyState>Try “How does cancellation remain O(1)?”</EmptyState>}
      {messages.map((message, index) => <div className={`chat-message ${message.role}`} key={`${message.role}-${index}`}>
        <div className="chat-bubble">{message.text || message.answer}</div>
        {message.citations?.length > 0 && <div className="citations"><p>Sources</p>{message.citations.map((citation, citationIndex) => <div className="citation" key={`${citation.path}-${citation.start_line}`}><button onClick={() => setOpenCitation(openCitation === `${index}-${citationIndex}` ? null : `${index}-${citationIndex}`)}><span><code>{citation.path}:{citation.start_line}-{citation.end_line}</code>{citation.symbol && <small>{citation.symbol}</small>}</span><ChevronDown size={14} /></button>{openCitation === `${index}-${citationIndex}` && <pre>{citation.snippet}</pre>}</div>)}</div>}
        {message.grounded === false && <small className="uncertainty">Insufficient indexed evidence</small>}
      </div>)}
      {loading && <div className="chat-message assistant"><div className="chat-bubble typing">Searching source evidence…</div></div>}
    </div>
    {notice && <p className="assistant-notice">{notice}</p>}
    <form className="assistant-form" onSubmit={ask}><input value={question} onChange={event => setQuestion(event.target.value)} placeholder="Ask a question about the codebase…" /><button disabled={!canSend} title="Ask assistant"><Send size={15} /></button></form>
  </Panel>;
}
