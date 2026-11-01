class AppError extends Error {
  constructor(status, code, message) {
    super(message);
    this.name = 'AppError';
    this.status = status;
    this.code = code;
  }
}

function errorHandler(error, request, response, next) {
  if (response.headersSent) return next(error);
  const status = Number.isInteger(error.status) ? error.status : 500;
  const code = error.code || 'INTERNAL_ERROR';
  if (status >= 500) console.error(JSON.stringify({
    level: 'error',
    requestId: request.id,
    code,
    message: error.message
  }));
  response.status(status).json({
    requestId: request.id,
    error: { code, message: error.message }
  });
}

module.exports = { AppError, errorHandler };
