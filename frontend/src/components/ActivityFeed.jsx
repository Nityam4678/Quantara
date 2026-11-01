import { Activity, CheckCircle2, CircleAlert } from 'lucide-react';
import { EmptyState, Panel, StatusPill } from './Panel';

function label(event) {
  if (event.operation === 'cancel') return 'Order cancelled';
  if (event.trades?.length && event.report?.resting) return 'Partial fill';
  if (event.trades?.length) return 'Order filled';
  return 'New order';
}

export function ActivityFeed({ events }) {
  return <Panel title="Recent activity" eyebrow="Engine event stream" action={<Activity size={17} className="muted" />}>
    {!events.length ? <EmptyState>Waiting for engine events.</EmptyState> : <div className="activity-feed">{events.slice(0, 8).map((event, index) => <div className="activity-item" key={`${event.sequence}-${index}`}>
      <div className="activity-symbol">{event.report?.status === 'OK' ? <CheckCircle2 size={16} /> : <CircleAlert size={16} />}</div>
      <div><strong>{label(event)}</strong><p className="muted">Order #{event.report?.orderId} · {event.report?.status?.replaceAll('_', ' ')}</p></div>
      <time>{new Date(event.receivedAt).toLocaleTimeString()}</time>
      <StatusPill tone={event.report?.status === 'OK' ? 'green' : 'red'}>{event.report?.status === 'OK' ? 'OK' : 'REJECTED'}</StatusPill>
    </div>)}</div>}
  </Panel>;
}
