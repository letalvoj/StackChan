"""Debug telemetry services and custom log handlers."""

import asyncio
import logging
import sys
import time
from typing import Callable, Optional


class FlushedFileHandler(logging.FileHandler):
    """FileHandler subclass that forces an immediate disk flush after every emitted record.

    Ensures automated external review scripts and real-time log tailing read unbuffered output
    immediately without lag or manual copying.
    """

    def emit(self, record: logging.LogRecord) -> None:
        super().emit(record)
        self.flush()


class WebSocketLogHandler(logging.Handler):
    """Logging handler that streams formatted records via an injected async broadcast callback.

    Decouples log processing from transport logic by relying on a custom emitter function
    passed upon instantiation in the application entrypoint.
    """

    def __init__(self, emitter_callback: Callable[[dict], None], has_clients_callback: Optional[Callable[[], bool]] = None):
        super().__init__()
        self.emitter_callback = emitter_callback
        self.has_clients_callback = has_clients_callback

    def emit(self, record: logging.LogRecord) -> None:
        message = self.format(record)
        payload = {
            "type": "log",
            "ts": time.time(),
            "level": record.levelname,
            "msg": message,
        }
        try:
            loop = asyncio.get_running_loop()
            if not self.has_clients_callback or self.has_clients_callback():
                loop.call_soon_threadsafe(self.emitter_callback, payload)
        except RuntimeError:
            sys.stderr.write(f"[STARTUP: {record.levelname}] {message}\n")
            sys.stderr.flush()
