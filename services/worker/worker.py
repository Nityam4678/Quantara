"""Native background worker health process.

Work is intentionally bounded to the configured Redis connection. Queue
producers can be added without granting this process shell access.
"""
import logging
import os
import time

logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"),
                    format='{"level":"%(levelname)s","message":"%(message)s"}')
log = logging.getLogger("trademind.worker")

try:
    import redis
except ImportError:
    redis = None


def main():
    redis_url = os.getenv("REDIS_URL", "redis://127.0.0.1:6379/0")
    client = redis.Redis.from_url(redis_url, socket_connect_timeout=2,
                                  socket_timeout=2) if redis else None
    while True:
        try:
            if client:
                client.ping()
            log.info("worker heartbeat redis=%s", bool(client))
        except Exception as error:
            log.error("worker dependency check failed error=%s", error)
        time.sleep(float(os.getenv("WORKER_POLL_SECONDS", "10")))


if __name__ == "__main__":
    main()
