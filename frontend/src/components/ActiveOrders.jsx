import { X } from 'lucide-react';
import { EmptyState, Panel, StatusPill } from './Panel';

export function ActiveOrders({ orders, onCancel }) {
  const visible = orders.filter(order => order.status === 'ACTIVE');
  return (
    <Panel title="Order management" eyebrow="Your resting orders">
      {!visible.length ? <EmptyState>No active orders.</EmptyState> : (
        <div className="scroll-table"><table><thead><tr><th>Order</th><th>Side</th><th>Price</th><th>Qty</th><th /></tr></thead>
          <tbody>{visible.map(order => <tr key={order.orderId}><td className="mono">#{order.orderId}</td><td><StatusPill tone={order.side}>{order.side}</StatusPill></td><td className="mono">{order.price}</td><td>{order.quantity}</td><td><button className="icon-button" title="Cancel order" onClick={() => onCancel(order.orderId)}><X size={15} /></button></td></tr>)}</tbody>
        </table></div>
      )}
    </Panel>
  );
}
