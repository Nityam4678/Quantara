import { Panel, EmptyState } from './Panel';

function Levels({ levels, max, tone }) {
  if (!levels?.length) return <EmptyState>No resting {tone} orders.</EmptyState>;
  return (
    <div className="levels">
      {levels.map(level => (
        <div className={`level-row ${tone}`} key={level.price}>
          <span>{level.price.toLocaleString()}</span>
          <span>{level.quantity.toLocaleString()}</span>
          <span className="level-bar" style={{ width: `${Math.max(4, (level.quantity / max) * 100)}%` }} />
        </div>
      ))}
    </div>
  );
}

export function OrderBook({ book }) {
  const max = Math.max(...(book?.bids || []).concat(book?.asks || []).map(level => level.quantity), 1);
  return (
    <Panel title="Order book" eyebrow="Live market depth" action={<span className="live-dot">● Live</span>}>
      <div className="book-summary">
        <div><small>BEST BID</small><strong className="text-green">{book?.bestBid?.price ?? '—'}</strong></div>
        <div className="spread"><small>SPREAD</small><strong>{book?.spread ?? '—'}</strong></div>
        <div><small>BEST ASK</small><strong className="text-red">{book?.bestAsk?.price ?? '—'}</strong></div>
      </div>
      <div className="book-columns">
        <div><div className="table-heading"><span>BIDS</span><span>QTY</span></div><Levels levels={book?.bids} max={max} tone="bid" /></div>
        <div><div className="table-heading"><span>ASKS</span><span>QTY</span></div><Levels levels={book?.asks} max={max} tone="ask" /></div>
      </div>
    </Panel>
  );
}
