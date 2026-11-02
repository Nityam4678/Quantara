const test = require('node:test');
const assert = require('node:assert/strict');
const { signToken } = require('../src/security');
const { createApp } = require('../src/app');

function serviceStub() {
  return {
    health: async () => ({ ok: true }),
    metricsSnapshot: () => ({}),
    subscribe: () => () => {},
    executions: [],
    events: []
  };
}

test('rejects malformed JWTs as unauthorized', async () => {
  const previous = {
    required: process.env.AUTH_REQUIRED,
    secret: process.env.JWT_SECRET
  };
  process.env.AUTH_REQUIRED = 'true';
  process.env.JWT_SECRET = 'test-secret';
  const app = createApp(serviceStub());
  const server = app.listen(0, '127.0.0.1');
  await new Promise(resolve => server.once('listening', resolve));
  const response = await fetch(`http://127.0.0.1:${server.address().port}/api/health`, {
    headers: { authorization: 'Bearer not.a.jwt' }
  });
  server.close();
  process.env.AUTH_REQUIRED = previous.required;
  process.env.JWT_SECRET = previous.secret;
  assert.equal(response.status, 401);
});

test('accepts a valid JWT when authentication is required', async () => {
  const previous = {
    required: process.env.AUTH_REQUIRED,
    secret: process.env.JWT_SECRET
  };
  process.env.AUTH_REQUIRED = 'true';
  process.env.JWT_SECRET = 'test-secret';
  const app = createApp(serviceStub());
  const server = app.listen(0, '127.0.0.1');
  await new Promise(resolve => server.once('listening', resolve));
  const token = signToken({ sub: 'test' }, 'test-secret', 60);
  const response = await fetch(`http://127.0.0.1:${server.address().port}/api/health`, {
    headers: { authorization: `Bearer ${token}` }
  });
  server.close();
  process.env.AUTH_REQUIRED = previous.required;
  process.env.JWT_SECRET = previous.secret;
  assert.equal(response.status, 200);
});
