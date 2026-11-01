import { BarChart3, Bell, Radio, Settings2, ShieldCheck, Sparkles } from 'lucide-react';
import { useTradingTerminal } from './hooks/useTradingTerminal';
import { MetricCards } from './components/MetricCards';
import { Panel } from './components/Panel';
import { OrderBook } from './components/OrderBook';
import { OrderTicket } from './components/OrderTicket';
import { ActiveOrders } from './components/ActiveOrders';
import { ExecutionHistory } from './components/ExecutionHistory';
import { ActivityFeed } from './components/ActivityFeed';
import { KnowledgeAssistant } from './components/KnowledgeAssistant';
import { EvaluationDashboard } from './components/EvaluationDashboard';
import { InvestigationAssistant } from './components/InvestigationAssistant';
import { BenchmarkDashboard } from './components/BenchmarkDashboard';

function App() {
  const terminal = useTradingTerminal();
  return <div className="app-shell">
    <header className="topbar">
      <div className="brand"><div className="brand-mark"><Sparkles size={18} /></div><div><strong>TRADEMIND</strong><span>Execution intelligence</span></div></div>
      <div className="instrument"><span className="instrument-dot" /> BTC-USD <span className="muted">/</span> Spot <span className="live-dot">● {terminal.connection === 'live' ? 'LIVE' : terminal.connection.toUpperCase()}</span></div>
      <div className="top-actions"><button className="top-icon"><Bell size={17} /></button><button className="top-icon"><Settings2 size={17} /></button><div className="avatar">TM</div></div>
    </header>
    <main>
      <div className="page-heading"><div><p className="eyebrow">MARKET OPERATIONS / SESSION 01</p><h1>Trading terminal</h1><p className="subtitle">Real-time matching engine observability and execution control.</p></div><div className="engine-status"><ShieldCheck size={16} /> Native engine connected</div></div>
      {terminal.error && <div className="error-banner">{terminal.error}</div>}
      <MetricCards metrics={terminal.metrics} />
      <div className="main-grid"><OrderBook book={terminal.book} /><OrderTicket onSubmit={terminal.submitOrder} submitting={terminal.submitting} /></div>
      <div className="main-grid lower-grid"><ActiveOrders orders={terminal.orders} onCancel={terminal.cancelOrder} /><ActivityFeed events={terminal.events} /></div>
      <ExecutionHistory executions={terminal.executions} />
      <KnowledgeAssistant />
      <EvaluationDashboard />
      <InvestigationAssistant />
      <BenchmarkDashboard />
      <div className="footer-line"><span><Radio size={13} /> Connected to actual C++ price-time engine</span><span><BarChart3 size={13} /> Fixed tick grid · 100,000 max price</span></div>
    </main>
  </div>;
}

export default App;
