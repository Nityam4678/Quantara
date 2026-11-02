const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { EngineClient } = require('../src/engine-client');
const { TradeMindService } = require('../src/service');
const { createApp } = require('../src/app');

const adapter = process.env.ENGINE_ADAPTER_PATH ||
  path.resolve(__dirname, '../../../.trademind-build/trademind_engine_adapter.exe');

function request(server, method, url, body) {
  return fetch(`http://127.0.0.1:${server.address().port}${url}`, {
    method,
    headers: body ? { 'content-type': 'application/json' } : {},
    body: body ? JSON.stringify(body) : undefined
  }).then(async response => ({
    status: response.status,
    body: await response.json()
  }));
}

test('Phase 1 API performs actual price-time matching and cancellation', async t => {
  const engine = new EngineClient({ executable: adapter, capacity: 32, timeoutMs: 2000 });
  const service = new TradeMindService(engine);
  const app = createApp(service);
  const server = await new Promise(resolve => {
    const value = app.listen(0, '127.0.0.1', () => resolve(value));
  });
  t.after(async () => {
    await new Promise(resolve => server.close(resolve));
    await engine.close();
  });

  let result = await request(server, 'POST', '/api/orders',
    { orderId: 1, side: 'SELL', price: 100, quantity: 10 });
  assert.equal(result.status, 201);
  assert.equal(result.body.report.resting, 10);

  result = await request(server, 'POST', '/api/orders',
    { orderId: 2, side: 'BUY', price: 100, quantity: 6 });
  assert.equal(result.status, 201);
  assert.equal(result.body.report.filled, 6);
  assert.equal(result.body.report.resting, 0);
  assert.equal(result.body.trades[0].makerOrderId, 1);
  assert.equal(result.body.trades[0].price, 100);

  result = await request(server, 'GET', '/api/orderbook?depth=5');
  assert.equal(result.status, 200);
  assert.equal(result.body.bestAsk.quantity, 4);

  result = await request(server, 'GET', '/api/orders/1');
  assert.equal(result.status, 200);
  assert.equal(result.body.order.quantity, 4);

  result = await request(server, 'DELETE', '/api/orders/1');
  assert.equal(result.status, 200);
  assert.equal(result.body.report.resting, 4);

  result = await request(server, 'GET', '/api/orderbook');
  assert.equal(result.body.bestAsk, null);
});

test('Phase 1 API rejects invalid input, duplicate IDs, and pool exhaustion', async t => {
  const engine = new EngineClient({ executable: adapter, capacity: 1, timeoutMs: 2000 });
  const service = new TradeMindService(engine);
  const app = createApp(service);
  const server = await new Promise(resolve => {
    const value = app.listen(0, '127.0.0.1', () => resolve(value));
  });
  t.after(async () => {
    await new Promise(resolve => server.close(resolve));
    await engine.close();
  });

  let result = await request(server, 'POST', '/api/orders',
    { orderId: 1, side: 'BUY', price: 0, quantity: 1 });
  assert.equal(result.status, 400);

  result = await request(server, 'POST', '/api/orders',
    { orderId: 1, side: 'BUY', price: 100, quantity: 1 });
  assert.equal(result.status, 201);

  result = await request(server, 'POST', '/api/orders',
    { orderId: 1, side: 'SELL', price: 100, quantity: 1 });
  assert.equal(result.status, 201);
  assert.equal(result.body.report.status, 'REJECTED_DUPLICATE_ID');

  result = await request(server, 'POST', '/api/orders',
    { orderId: 2, side: 'BUY', price: 99, quantity: 1 });
  assert.equal(result.status, 201);
  assert.equal(result.body.report.status, 'REJECTED_POOL_EXHAUSTED');
});
