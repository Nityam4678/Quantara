const path = require('node:path');
const { EngineClient } = require('./engine-client');
const { QuantaraService } = require('./service');
const { createApp } = require('./app');
const { AiClient } = require('./ai-client');

const executable = process.env.ENGINE_ADAPTER_PATH
  ? path.resolve(process.env.ENGINE_ADAPTER_PATH)
  : path.resolve(__dirname, '../../../.quantara-build/quantara_engine_adapter.exe');
const engine = new EngineClient({
  executable,
  capacity: process.env.ENGINE_CAPACITY || 1000000,
  timeoutMs: Number(process.env.ENGINE_REQUEST_TIMEOUT_MS || 5000)
});
const service = new QuantaraService(engine);
const aiClient = new AiClient();
const app = createApp(service, aiClient);
const port = Number(process.env.PORT || 3000);
const requestTimeout = Number(process.env.HTTP_REQUEST_TIMEOUT_MS || 15000);
const server = app.listen(port, '127.0.0.1', () => {
  console.log(JSON.stringify({ level: 'info', message: 'Quantara API listening', port, executable }));
});
server.requestTimeout = requestTimeout;
server.headersTimeout = requestTimeout + 5000;

async function shutdown() {
  server.close();
  await engine.close();
}
process.once('SIGINT', shutdown);
process.once('SIGTERM', shutdown);

module.exports = { app, server, engine, service };
