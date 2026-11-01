import { EmptyState, Panel } from './Panel';

export function ExecutionHistory({ executions }) {
  const trades = executions.flatMap(event => event.trades.map((trade, index) => ({ ...trade, sequence: event.sequence, key: `${event.sequence}-${index}`, timestamp: event.receivedAt })));
  return <Panel title="Execution history" eyebrow="Matched trades">
    {!trades.length ? <EmptyState>No executions yet.</EmptyState> : <div className="scroll-table"><table><thead><tr><th>Seq</th><th>Maker</th><th>Taker</th><th>Price</th><th>Qty</th><th>Time</th></tr></thead><tbody>
      {trades.map(trade => <tr key={trade.key}><td className="mono">#{trade.sequence}</td><td className="mono">#{trade.makerOrderId}</td><td className="mono">#{trade.takerOrderId}</td><td>{trade.price}</td><td>{trade.quantity}</td><td className="muted">{new Date(trade.timestamp).toLocaleTimeString()}</td></tr>)}
    </tbody></table></div>}
  </Panel>;
}
