class QuantaraService {
  constructor(engine) {
    this.engine = engine;
    this.events = [];
    this.executions = [];
    this.subscribers = new Set();
    this.metrics = {
      totalOrders: 0,
      activeOrders: 0,
      filledOrders: 0,
      cancelledOrders: 0,
      totalExecutions: 0
    };
    this.activeOrderIds = new Set();
  }

  async submitOrder(order) {
    const result = await this.engine.request({ type: 'add_order', ...order });
    this.record(result, 'add');
    return result;
  }

  async cancelOrder(orderId) {
    const result = await this.engine.request({ type: 'cancel_order', orderId });
    this.record(result, 'cancel');
    return result;
  }

  async snapshot(depth) {
    return this.engine.request({ type: 'snapshot', depth });
  }

  async order(orderId) {
    return this.engine.request({ type: 'order', orderId });
  }

  async recentEvents(limit) {
    const result = await this.engine.request({ type: 'recent_events', limit });
    return result.events;
  }

  async health() {
    return this.engine.request({ type: 'health' });
  }

  metricsSnapshot() {
    return { ...this.metrics, activeOrders: this.activeOrderIds.size };
  }

  subscribe(listener) {
    this.subscribers.add(listener);
    return () => this.subscribers.delete(listener);
  }

  record(result, operation) {
    const report = result.report;
    if (operation === 'add' && report) {
      this.metrics.totalOrders += 1;
      this.metrics.totalExecutions += result.trades?.length || 0;
      if (report.resting > 0) this.activeOrderIds.add(String(report.orderId));
      if (report.filled > 0 && report.resting === 0) {
        this.metrics.filledOrders += 1;
        this.activeOrderIds.delete(String(report.orderId));
      }
    }
    if (operation === 'cancel' && report?.status === 'OK') {
      this.metrics.cancelledOrders += 1;
      this.activeOrderIds.delete(String(report.orderId));
    }
    this.metrics.activeOrders = this.activeOrderIds.size;

    const event = {
      type: result.type,
      operation,
      sequence: result.sequence,
      report,
      trades: result.trades || [],
      receivedAt: new Date().toISOString()
    };
    this.events.push(event);
    this.executions.push(event);
    if (this.events.length > 1000) this.events.shift();
    if (this.executions.length > 1000) this.executions.shift();
    for (const subscriber of this.subscribers) subscriber(event);
  }
}

module.exports = { QuantaraService };
