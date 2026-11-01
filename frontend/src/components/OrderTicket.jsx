import { useState } from 'react';
import { ArrowDownToLine, ArrowUpFromLine } from 'lucide-react';
import { Panel } from './Panel';

export function OrderTicket({ onSubmit, submitting }) {
  const [side, setSide] = useState('BUY');
  const [orderId, setOrderId] = useState('');
  const [price, setPrice] = useState('');
  const [quantity, setQuantity] = useState('');
  const [message, setMessage] = useState('');
  const submit = async event => {
    event.preventDefault();
    setMessage('');
    try {
      const result = await onSubmit({ orderId: Number(orderId), side, price: Number(price), quantity: Number(quantity) });
      setMessage(result.report.status === 'OK'
        ? `${result.report.filled ? `Filled ${result.report.filled}` : ''}${result.report.resting ? ` Resting ${result.report.resting}` : ''}`.trim() || 'Order accepted'
        : result.report.status.replaceAll('_', ' '));
      setOrderId('');
      setPrice('');
      setQuantity('');
    } catch {}
  };
  return (
    <Panel title="Place limit order" eyebrow="Execution ticket">
      <div className="side-toggle">
        <button className={side === 'BUY' ? 'selected-buy' : ''} onClick={() => setSide('BUY')} type="button"><ArrowUpFromLine size={15} /> Buy</button>
        <button className={side === 'SELL' ? 'selected-sell' : ''} onClick={() => setSide('SELL')} type="button"><ArrowDownToLine size={15} /> Sell</button>
      </div>
      <form className="ticket-form" onSubmit={submit}>
        <label>Order ID<input required min="1" type="number" value={orderId} onChange={event => setOrderId(event.target.value)} placeholder="10001" /></label>
        <label>Price<input required min="1" max="99999" type="number" value={price} onChange={event => setPrice(event.target.value)} placeholder="10025" /></label>
        <label>Quantity<input required min="1" type="number" value={quantity} onChange={event => setQuantity(event.target.value)} placeholder="50" /></label>
        <button className={`submit-order ${side.toLowerCase()}`} disabled={submitting} type="submit">{submitting ? 'Submitting…' : `Place ${side}`}</button>
      </form>
      {message && <p className="ticket-message">{message}</p>}
    </Panel>
  );
}
