import { useCallback, useEffect, useRef, useState } from 'react';
import { api } from '../api';

export function useTradingTerminal() {
  const [book, setBook] = useState(null);
  const [metrics, setMetrics] = useState(null);
  const [events, setEvents] = useState([]);
  const [executions, setExecutions] = useState([]);
  const [orders, setOrders] = useState([]);
  const [connection, setConnection] = useState('connecting');
  const [error, setError] = useState('');
  const [submitting, setSubmitting] = useState(false);
  const refreshBook = useCallback(async () => setBook(await api.book()), []);
  const refreshMetrics = useCallback(async () => setMetrics((await api.metrics()).metrics), []);

  useEffect(() => {
    Promise.all([api.book(), api.metrics(), api.events(), api.executions()])
      .then(([nextBook, nextMetrics, nextEvents, nextExecutions]) => {
        setBook(nextBook);
        setMetrics(nextMetrics.metrics);
        setEvents(nextEvents.events);
        setExecutions(nextExecutions.executions);
      })
      .catch(nextError => setError(nextError.message));
  }, []);

  useEffect(() => {
    let source;
    let retry;
    let stopped = false;
    const connect = () => {
      if (stopped) return;
      source = new EventSource('/api/stream');
      source.onopen = () => setConnection('live');
      source.onerror = () => {
        setConnection('reconnecting');
        source.close();
        retry = window.setTimeout(connect, 1500);
      };
      source.addEventListener('engine', event => {
        const nextEvent = JSON.parse(event.data);
        setEvents(previous => [nextEvent, ...previous].slice(0, 50));
        if (nextEvent.trades?.length) {
          setExecutions(previous => [nextEvent, ...previous].slice(0, 50));
        }
        refreshBook().catch(nextError => setError(nextError.message));
        refreshMetrics().catch(nextError => setError(nextError.message));
      });
    };
    connect();
    return () => {
      stopped = true;
      window.clearTimeout(retry);
      source?.close();
    };
  }, [refreshBook, refreshMetrics]);

  const submitOrder = async order => {
    setSubmitting(true);
    setError('');
    try {
      const result = await api.submitOrder(order);
      setOrders(previous => [{
        orderId: String(order.orderId),
        side: order.side,
        price: order.price,
        quantity: result.report.resting,
        status: result.report.resting > 0 ? 'ACTIVE' : 'FILLED'
      }, ...previous.filter(item => item.orderId !== String(order.orderId))]);
      await Promise.all([refreshBook(), refreshMetrics()]);
      return result;
    } catch (nextError) {
      setError(nextError.message);
      throw nextError;
    } finally {
      setSubmitting(false);
    }
  };

  const cancelOrder = async orderId => {
    setError('');
    try {
      const result = await api.cancelOrder(orderId);
      setOrders(previous => previous.map(order => order.orderId === String(orderId)
        ? { ...order, status: 'CANCELLED', quantity: 0 }
        : order));
      await Promise.all([refreshBook(), refreshMetrics()]);
      return result;
    } catch (nextError) {
      setError(nextError.message);
      throw nextError;
    }
  };

  return {
    book, metrics, events, executions, orders, connection, error, submitting,
    submitOrder, cancelOrder
  };
}
