const express = require('express');
const crypto = require('node:crypto');
const { AppError, errorHandler } = require('./errors');
const { orderInput, idParameter, boundedQuery } = require('./validation');
const { authentication, issueToken, createRateLimiter, cors } = require('./security');
const { createMetrics } = require('./metrics');
const { trace } = require('@opentelemetry/api');

function createApp(service, aiClient = null) {
  const app = express();
  const metrics = createMetrics();
  const tracer = trace.getTracer('trademind-api-gateway');
  app.use(express.json({ limit: process.env.JSON_BODY_LIMIT || '64kb' }));
  app.use(cors());
  app.use(createRateLimiter({
    windowMs: Number(process.env.RATE_LIMIT_WINDOW_MS || 60_000),
    max: Number(process.env.RATE_LIMIT_MAX || 240)
  }));
  app.post('/api/auth/token', issueToken);
  app.use(authentication());
  app.use((request, response, next) => {
    request.id = request.header('x-request-id') || crypto.randomUUID();
    response.setHeader('x-request-id', request.id);
    next();
  });
  app.use((request, response, next) => {
    const started = performance.now();
    const span = tracer.startSpan(`HTTP ${request.method}`);
    span.setAttribute('http.route', request.path);
    response.on('finish', () => {
      const duration = performance.now() - started;
      metrics.observe(request.route?.path || request.path, duration);
      span.setAttribute('http.status_code', response.statusCode);
      span.end();
    });
    next();
  });

  app.get('/api/health', async (request, response, next) => {
    try {
      response.json(await service.health());
    } catch (error) { next(error); }
  });

  app.get('/api/ai/health', async (request, response, next) => {
    if (!aiClient) return next(new AppError(503, 'AI_UNAVAILABLE', 'AI service is not configured'));
    try { response.json(await aiClient.health()); } catch (error) { next(error); }
  });

  app.post('/api/ai/ingest', async (request, response, next) => {
    if (!aiClient) return next(new AppError(503, 'AI_UNAVAILABLE', 'AI service is not configured'));
    try { response.status(202).json(await aiClient.ingest()); } catch (error) { next(error); }
  });

  app.get('/api/ai/retrieval', async (request, response, next) => {
    if (!aiClient) return next(new AppError(503, 'AI_UNAVAILABLE', 'AI service is not configured'));
    try { response.json(await aiClient.retrieval(request.query)); } catch (error) { next(error); }
  });

  app.post('/api/ai/evaluate', async (request, response, next) => {
    if (!aiClient) return next(new AppError(503, 'AI_UNAVAILABLE', 'AI service is not configured'));
    try { response.json(await aiClient.evaluate(request.body?.k || 5)); } catch (error) { next(error); }
  });

  app.post('/api/ai/chat', async (request, response, next) => {
    if (!aiClient) return next(new AppError(503, 'AI_UNAVAILABLE', 'AI service is not configured'));
    try { response.json(await aiClient.chat(request.body)); } catch (error) { next(error); }
  });

  app.post('/api/ai/agent', async (request, response, next) => {
    if (!aiClient) return next(new AppError(503, 'AI_UNAVAILABLE', 'AI service is not configured'));
    try { response.json(await aiClient.agent(request.body)); } catch (error) { next(error); }
  });

  app.post('/api/ai/agent/stream', async (request, response, next) => {
    if (!aiClient) return next(new AppError(503, 'AI_UNAVAILABLE', 'AI service is not configured'));
    try {
      const upstream = await aiClient.agentStream(request.body);
      response.status(200).set({
        'Content-Type': 'text/event-stream',
        'Cache-Control': 'no-cache',
        Connection: 'keep-alive',
        'X-Accel-Buffering': 'no'
      });

      response.flushHeaders();
      for await (const chunk of upstream.body) response.write(chunk);
      response.end();
    } catch (error) { next(error); }
  });

  app.post('/api/ai/benchmarks/run', async (request, response, next) => {
    if (!aiClient) return next(new AppError(503, 'AI_UNAVAILABLE', 'AI service is not configured'));
    try { response.status(201).json(await aiClient.runBenchmark(request.body)); } catch (error) { next(error); }
  });
  app.get('/api/ai/benchmarks', async (request, response, next) => {
    if (!aiClient) return next(new AppError(503, 'AI_UNAVAILABLE', 'AI service is not configured'));
    try { response.json(await aiClient.benchmarks(request.query.limit || 100)); } catch (error) { next(error); }
  });
  app.post('/api/ai/benchmarks/analyze', async (request, response, next) => {
    if (!aiClient) return next(new AppError(503, 'AI_UNAVAILABLE', 'AI service is not configured'));
    try { response.json(await aiClient.benchmarkAnalysis(request.body)); } catch (error) { next(error); }
  });

  app.post('/api/ai/chat/stream', async (request, response, next) => {
    if (!aiClient) return next(new AppError(503, 'AI_UNAVAILABLE', 'AI service is not configured'));
    try {
      const upstream = await aiClient.stream(request.body);
      response.status(200).set({
        'Content-Type': 'text/event-stream',
        'Cache-Control': 'no-cache',
        Connection: 'keep-alive',
        'X-Accel-Buffering': 'no'
      });
      response.flushHeaders();
      for await (const chunk of upstream.body) response.write(chunk);
      response.end();
    } catch (error) { next(error); }
  });

  app.get('/api/metrics', (request, response) => {
    response.json({ ok: true, metrics: service.metricsSnapshot(), http: metrics.snapshot() });
  });
  app.get('/metrics', (request, response) => {
    const metricLabel = value => String(value).replace(/\\/g, '\\\\')
      .replace(/"/g, '\\"').replace(/\r?\n/g, '');
    const lines = metrics.snapshot().flatMap(item => [
      `trademind_http_requests_total{route="${metricLabel(item.route)}"} ${item.requests}`,
      `trademind_http_request_latency_average_ms{route="${metricLabel(item.route)}"} ${item.averageMs}`
    ]);
    response.type('text/plain').send(`${lines.join('\n')}\n`);
  });

  app.get('/api/stream', async (request, response, next) => {
    response.status(200);
    response.set({
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      Connection: 'keep-alive',
      'X-Accel-Buffering': 'no'
    });
    response.flushHeaders();
    response.write(`event: ready\ndata: ${JSON.stringify({
      metrics: service.metricsSnapshot()
    })}\n\n`);
    const unsubscribe = service.subscribe(event => {
      response.write(`id: ${event.sequence || ''}\nevent: engine\ndata: ${JSON.stringify(event)}\n\n`);
    });
    const heartbeat = setInterval(() => response.write(': heartbeat\n\n'), 15000);
    request.on('close', () => {
      clearInterval(heartbeat);
      unsubscribe();
      response.end();
    });
  });

  app.post('/api/orders', orderInput, async (request, response, next) => {
    try {
      response.status(201).json(await service.submitOrder(request.validatedOrder));
    } catch (error) { next(error); }
  });

  app.delete('/api/orders/:orderId', idParameter, async (request, response, next) => {
    try {
      response.json(await service.cancelOrder(request.validatedOrderId));
    } catch (error) { next(error); }
  });

  app.get('/api/orders/:orderId', idParameter, async (request, response, next) => {
    try {
      const result = await service.order(request.validatedOrderId);
      if (!result.order) throw new AppError(404, 'ORDER_NOT_FOUND', 'order is not resting');
      response.json(result);
    } catch (error) { next(error); }
  });

  app.get('/api/orderbook', boundedQuery('depth', 20, 1000), async (request, response, next) => {
    try { response.json(await service.snapshot(request.query.depth)); }
    catch (error) { next(error); }
  });

  app.get('/api/executions', boundedQuery('limit', 100, 1000), async (request, response, next) => {
    try { response.json({ ok: true, executions: service.executions.slice(-request.query.limit) }); }
    catch (error) { next(error); }
  });

  app.get('/api/events', boundedQuery('limit', 100, 1000), async (request, response, next) => {
    try { response.json({ ok: true, events: service.events.slice(-request.query.limit) }); }
    catch (error) { next(error); }
  });

  app.use((request, response, next) => next(new AppError(404, 'NOT_FOUND', 'route not found')));
  app.use(errorHandler);
  return app;
}

module.exports = { createApp };
