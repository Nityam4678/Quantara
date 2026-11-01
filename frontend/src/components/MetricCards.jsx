import { Activity, CircleDollarSign, ClipboardList, Layers3, XCircle } from 'lucide-react';

const cards = [
  ['Total orders', 'totalOrders', ClipboardList, 'blue'],
  ['Active orders', 'activeOrders', Layers3, 'amber'],
  ['Filled orders', 'filledOrders', CircleDollarSign, 'green'],
  ['Cancelled', 'cancelledOrders', XCircle, 'red'],
  ['Executions', 'totalExecutions', Activity, 'purple']
];

export function MetricCards({ metrics }) {
  return (
    <div className="metric-grid">
      {cards.map(([label, key, Icon, tone]) => (
        <div className="metric-card" key={key}>
          <div className={`metric-icon metric-${tone}`}><Icon size={17} /></div>
          <div>
            <p>{label}</p>
            <strong>{metrics?.[key] ?? '—'}</strong>
          </div>
        </div>
      ))}
    </div>
  );
}
