"""SLIP-Framed Serial Transport Implementation over /dev/ttyACM*."""

import asyncio
import logging
import os
import termios
from typing import Optional, Union
from gateway.transport import SlipFramer, Transport

logger = logging.getLogger("gateway.serial_transport")


class SerialTransport(Transport):
    """Serial transport using raw OS file descriptor with RFC 1055 SLIP framing."""

    def __init__(self, port: str = "/dev/ttyACM1", baudrate: int = 921600) -> None:
        self._port = port
        self._baudrate = baudrate
        self._fd: Optional[int] = None
        self._framer = SlipFramer()
        self._queue: asyncio.Queue[Union[str, bytes]] = asyncio.Queue()
        self._read_task: Optional[asyncio.Task[None]] = None
        self._connected = False

    def get_transport_name(self) -> str:
        return f"serial:{self._port}"

    def is_connected(self) -> bool:
        return self._connected and self._fd is not None

    async def connect(self) -> None:
        while not self._connected:
            try:
                self._fd = os.open(self._port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                try:
                    attrs = termios.tcgetattr(self._fd)
                    attrs[0] = 0  # iflag
                    attrs[1] = 0  # oflag
                    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # cflag
                    attrs[3] = 0  # lflag
                    attrs[4] = termios.B921600  # ispeed
                    attrs[5] = termios.B921600  # ospeed
                    attrs[6][termios.VMIN] = 0
                    attrs[6][termios.VTIME] = 0
                    termios.tcsetattr(self._fd, termios.TCSANOW, attrs)
                except (termios.error, OSError):
                    logger.debug(
                        "[SERIAL:CONN] Termios setting failed or ignored on virtual port %s",
                        self._port,
                    )
                self._connected = True
                self._read_task = asyncio.create_task(self._read_loop())
                logger.info("[SERIAL:CONN] 🔌 Serial port %s opened cleanly.", self._port)
                break
            except (FileNotFoundError, OSError, PermissionError) as err:
                logger.debug(
                    "[SERIAL:CONN] 🔌 Serial port %s unavailable (%s), retrying in 2s...",
                    self._port,
                    err,
                )
                await asyncio.sleep(2.0)

    async def disconnect(self) -> None:
        self._connected = False
        if self._read_task:
            self._read_task.cancel()
            try:
                await self._read_task
            except asyncio.CancelledError:
                pass
            self._read_task = None
        if self._fd is not None:
            try:
                os.close(self._fd)
            except OSError:
                pass
            self._fd = None
        logger.info("[SERIAL:CONN] 🔌 Serial port %s closed.", self._port)

    async def _read_loop(self) -> None:
        loop = asyncio.get_running_loop()
        while self._connected and self._fd is not None:
            try:
                chunk = await loop.run_in_executor(None, self._os_read_blocking)
                if not chunk:
                    logger.warning("[SERIAL:CONN] 🔌 EOF on serial port %s", self._port)
                    break
                messages = self._framer.feed(chunk)
                for msg in messages:
                    await self._queue.put(msg)
            except (OSError, ValueError, BrokenPipeError, ConnectionError) as err:
                logger.warning(
                    "[SERIAL:CONN] 🔌 Read failure on %s (%s) — dropping connection.",
                    self._port,
                    err,
                )
                break
            except asyncio.CancelledError:
                break

        self._connected = False
        if self._fd is not None:
            try:
                os.close(self._fd)
            except OSError:
                pass
            self._fd = None

    def _os_read_blocking(self) -> bytes:
        if self._fd is None:
            return b""
        try:
            return os.read(self._fd, 8192)
        except (BlockingIOError, OSError):
            return b""

    async def recv(self) -> Union[str, bytes]:
        while self.is_connected() or not self._queue.empty():
            try:
                return await asyncio.wait_for(self._queue.get(), timeout=1.0)
            except (asyncio.TimeoutError, asyncio.CancelledError):
                if not self.is_connected():
                    raise ConnectionError("Serial transport disconnected.")
        raise ConnectionError("Serial transport offline.")

    async def send(self, data: Union[str, bytes]) -> None:
        if not self.is_connected() or self._fd is None:
            raise ConnectionError("Cannot send: serial transport offline.")
        encoded = SlipFramer.encode(data)
        loop = asyncio.get_running_loop()
        try:
            await loop.run_in_executor(None, os.write, self._fd, encoded)
        except (OSError, BrokenPipeError, ValueError) as err:
            self._connected = False
            logger.error("[SERIAL:TX] Write error on port %s: %s", self._port, err)
            raise ConnectionError(f"Serial transmission failed: {err}")
