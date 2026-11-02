const crypto = require('node:crypto');
const { AppError } = require('./errors');

function base64Url(value) {
  return Buffer.from(value).toString('base64url');
}

function signToken(payload, secret, expiresInSeconds) {
  const header = base64Url(JSON.stringify({ alg: 'HS256', typ: 'JWT' }));
  const body = base64Url(JSON.stringify({
    ...payload,
    iat: Math.floor(Date.now() / 1000),
    exp: Math.floor(Date.now() / 1000) + expiresInSeconds
  }));
  const unsigned = `${header}.${body}`;
  const signature = crypto.createHmac('sha256', secret).update(unsigned).digest('base64url');
  return `${unsigned}.${signature}`;
}

function verifyToken(token, secret) {
  if (!secret) throw new AppError(503, 'AUTH_NOT_CONFIGURED', 'JWT_SECRET is not configured');
  const [header, body, signature] = String(token || '').split('.');
  if (!header || !body || !signature) throw new AppError(401, 'UNAUTHORIZED', 'invalid bearer token');
  const expected = crypto.createHmac('sha256', secret).update(`${header}.${body}`).digest('base64url');
  if (signature.length !== expected.length ||
      !crypto.timingSafeEqual(Buffer.from(signature), Buffer.from(expected))) {
    throw new AppError(401, 'UNAUTHORIZED', 'invalid bearer token');
  }
  let payload;
  try {
    const parsedHeader = JSON.parse(Buffer.from(header, 'base64url').toString('utf8'));
    if (parsedHeader.alg !== 'HS256' || parsedHeader.typ !== 'JWT') {
      throw new Error('unsupported JWT header');
    }
    payload = JSON.parse(Buffer.from(body, 'base64url').toString('utf8'));
  } catch {
    throw new AppError(401, 'UNAUTHORIZED', 'invalid bearer token');
  }
  if (!payload.exp || payload.exp <= Math.floor(Date.now() / 1000)) {
    throw new AppError(401, 'UNAUTHORIZED', 'expired bearer token');
  }
  return payload;
}

function authentication(options = {}) {
  const required = options.required ?? process.env.AUTH_REQUIRED === 'true';
  const secret = options.secret || process.env.JWT_SECRET;
  return (request, response, next) => {
    const header = request.header('authorization');
    if (!header) {
      if (!required) return next();
      return next(new AppError(401, 'UNAUTHORIZED', 'bearer token required'));
    }
    try {
      request.user = verifyToken(header.replace(/^Bearer\s+/i, ''), secret);
      next();
    } catch (error) { next(error); }
  };
}

function issueToken(request, response, next) {
  const secret = process.env.JWT_SECRET;
  const username = process.env.AUTH_USERNAME;
  const password = process.env.AUTH_PASSWORD;
  if (!secret || !username || !password || password.startsWith('replace-with-')) {
    return next(new AppError(503, 'AUTH_NOT_CONFIGURED', 'authentication is not configured'));
  }
  if (request.body?.username !== username || request.body?.password !== password) {
    return next(new AppError(401, 'UNAUTHORIZED', 'invalid credentials'));
  }
  response.json({
    token: signToken({ sub: username }, secret, Number(process.env.JWT_EXPIRES_SECONDS || 3600)),
    tokenType: 'Bearer'
  });
}

function createRateLimiter({ windowMs = 60_000, max = 120 } = {}) {
  const buckets = new Map();
  return (request, response, next) => {
    const now = Date.now();
    if (buckets.size > 10_000) {
      for (const [key, bucket] of buckets) {
        if (now >= bucket.resetAt) buckets.delete(key);
      }
    }
    const key = request.ip || request.socket.remoteAddress || 'unknown';
    const current = buckets.get(key);
    if (!current || now >= current.resetAt) {
      buckets.set(key, { count: 1, resetAt: now + windowMs });
      return next();
    }
    current.count += 1;
    response.setHeader('Retry-After', Math.ceil((current.resetAt - now) / 1000));
    if (current.count > max) return next(new AppError(429, 'RATE_LIMITED', 'too many requests'));
    next();
  };
}

function cors(options = {}) {
  const allowed = new Set((options.origins || process.env.CORS_ORIGINS || 'http://127.0.0.1:5173')
    .split(',').map(value => value.trim()).filter(Boolean));
  return (request, response, next) => {
    const origin = request.header('origin');
    if (origin && allowed.has(origin)) response.setHeader('Access-Control-Allow-Origin', origin);
    response.setHeader('Vary', 'Origin');
    response.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization, X-Internal-Token, X-Request-Id');
    response.setHeader('Access-Control-Allow-Methods', 'GET,POST,DELETE,OPTIONS');
    if (request.method === 'OPTIONS') return response.status(204).end();
    next();
  };
}

module.exports = { authentication, issueToken, createRateLimiter, cors, signToken };
