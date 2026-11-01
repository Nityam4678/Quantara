const jsonRequest = async (url, options = {}) => {
  const response = await fetch(url, {
    ...options,
    headers: { 'Content-Type': 'application/json', ...(options.headers || {}) }
  });
  const body = await response.json();
  if (!response.ok) {
    throw new Error(body.error?.message || 'Request failed');
  }
  return body;
};

export const api = {
  metrics: () => jsonRequest('/api/metrics'),
  book: () => jsonRequest('/api/orderbook?depth=12'),
  events: () => jsonRequest('/api/events?limit=50'),
  executions: () => jsonRequest('/api/executions?limit=50'),
  submitOrder: order => jsonRequest('/api/orders', {
    method: 'POST',
    body: JSON.stringify(order)
  }),
  cancelOrder: orderId => jsonRequest(`/api/orders/${orderId}`, { method: 'DELETE' })
};
