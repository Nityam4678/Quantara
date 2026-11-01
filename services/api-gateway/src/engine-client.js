const { spawn } = require('node:child_process');
const readline = require('node:readline');
const crypto = require('node:crypto');
const { AppError } = require('./errors');

class EngineClient {
  constructor({ executable, capacity, timeoutMs = 5000 }) {
    this.executable = executable;
    this.capacity = String(capacity);
    this.timeoutMs = timeoutMs;
    this.pending = new Map();
    this.queue = Promise.resolve();
    this.closed = false;
    this.start();
  }

  start() {
    this.child = spawn(this.executable, [this.capacity], {
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true
    });
    this.lines = readline.createInterface({ input: this.child.stdout });
    this.lines.on('line', line => this.onLine(line));
    this.child.stderr.on('data', chunk => {
      console.error(JSON.stringify({ level: 'error', component: 'engine', message: chunk.toString().trim() }));
    });
    this.child.on('error', error => this.failAll(new AppError(503, 'ENGINE_UNAVAILABLE', error.message)));
    this.child.on('close', (code, signal) => {
      this.closed = true;
      this.failAll(new AppError(503, 'ENGINE_UNAVAILABLE',
        `engine adapter exited (code=${code}, signal=${signal || 'none'})`));
    });
  }

  onLine(line) {
    let response;
    try {
      response = JSON.parse(line);
    } catch {
      this.failAll(new AppError(503, 'ENGINE_PROTOCOL_ERROR', 'engine returned malformed JSON'));
      return;
    }
    const pending = this.pending.get(response.requestId);
    if (!pending) return;
    this.pending.delete(response.requestId);
    clearTimeout(pending.timer);
    if (response.error) {
      pending.reject(new AppError(400, response.error.code || 'ENGINE_ERROR', response.error.message));
    } else {
      pending.resolve(response);
    }
  }

  failAll(error) {
    for (const [requestId, pending] of this.pending) {
      clearTimeout(pending.timer);
      pending.reject(error);
      this.pending.delete(requestId);
    }
  }

  request(payload) {
    const execute = () => new Promise((resolve, reject) => {
      if (this.closed || !this.child || this.child.stdin.destroyed) {
        reject(new AppError(503, 'ENGINE_UNAVAILABLE', 'engine adapter is not running'));
        return;
      }
      const requestId = crypto.randomUUID();
      const message = JSON.stringify({ requestId, ...payload }) + '\n';
      const timer = setTimeout(() => {
        this.pending.delete(requestId);
        reject(new AppError(504, 'ENGINE_TIMEOUT', 'engine adapter did not respond in time'));
      }, this.timeoutMs);
      this.pending.set(requestId, { resolve, reject, timer });
      if (!this.child.stdin.write(message)) {
        this.child.stdin.once('drain', () => {});
      }
    });
    const result = this.queue.then(execute, execute);
    this.queue = result.then(() => undefined, () => undefined);
    return result;
  }

  async close() {
    if (this.closed) return;
    this.child.stdin.end();
    await new Promise(resolve => this.child.once('close', resolve));
  }
}

module.exports = { EngineClient };
