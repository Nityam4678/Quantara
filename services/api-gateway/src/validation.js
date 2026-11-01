const { AppError } = require('./errors');

const MAX_PRICE = 100000;
const MAX_QUANTITY = 0xffffffff;

function positiveInteger(value, name, maximum = Number.MAX_SAFE_INTEGER) {
  if ((typeof value !== 'number' && typeof value !== 'string') ||
      !/^(0|[1-9]\d*)$/.test(String(value))) {
    throw new AppError(400, 'INVALID_REQUEST', `${name} must be a positive integer`);
  }
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed <= 0 || parsed > maximum) {
    throw new AppError(400, 'INVALID_REQUEST', `${name} must be a positive integer`);
  }
  return parsed;
}

function orderId(value) {
  return positiveInteger(value, 'orderId');
}

function orderInput(request, response, next) {
  try {
    const { orderId: id, side, price, quantity } = request.body || {};
    if (side !== 'BUY' && side !== 'SELL') {
      throw new AppError(400, 'INVALID_SIDE', 'side must be BUY or SELL');
    }
    request.validatedOrder = {
      orderId: orderId(id),
      side,
      price: positiveInteger(price, 'price', MAX_PRICE - 1),
      quantity: positiveInteger(quantity, 'quantity', MAX_QUANTITY)
    };
    next();
  } catch (error) {
    next(error);
  }
}

function idParameter(request, response, next) {
  try {
    request.validatedOrderId = orderId(request.params.orderId);
    next();
  } catch (error) {
    next(error);
  }
}

function boundedQuery(name, defaultValue, maximum) {
  return (request, response, next) => {
    try {
      const raw = request.query[name] === undefined ? defaultValue : request.query[name];
      request.query[name] = positiveInteger(raw, name, maximum);
      next();
    } catch (error) {
      next(error);
    }
  };
}

module.exports = { orderInput, idParameter, boundedQuery, orderId };
