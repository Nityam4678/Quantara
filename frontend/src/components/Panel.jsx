export function Panel({ title, eyebrow, action, children, className = '' }) {
  return (
    <section className={`panel ${className}`}>
      <div className="panel-header">
        <div>
          {eyebrow && <p className="eyebrow">{eyebrow}</p>}
          <h2>{title}</h2>
        </div>
        {action}
      </div>
      {children}
    </section>
  );
}

export function EmptyState({ children = 'No data available yet.' }) {
  return <div className="empty-state">{children}</div>;
}

export function StatusPill({ children, tone = 'neutral' }) {
  return <span className={`status-pill status-${tone.toLowerCase()}`}>{children}</span>;
}
