"""TCP Transport with RFC 1055 SLIP Framing for Remote Socat Bridges."""

import asyncio
import logging
from typing import Optional, Union
from gateway.transport import SlipFramer, Transport

logger = logging.getLogger("gateway.tcp_transport")


class TcpTransport(Transport):
    """TCP server transport terminating SLIP framed serial bridges (e.g., socat/autossh)."""

    def __init__(self, host: str = "0.0.0.0", port: int = 9000) -> None:
        self._host = host
        self._port = port
        self._server: Optional[asyncio.Server] = None
        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._framer = SlipFramer()
        self._queue: asyncio.Queue[Union[str, bytes]] = asyncio.Queue()
        self._read_task: Optional[asyncio.Task[None]] = None
        self._connected = False
        self._client_addr = ""

    def get_transport_name(self) -> str:
        return (
            f"tcp:{self._client_addr}" if self._client_addr else f"tcp:{self._port}"
        )

    def is_connected(self) -> bool:
        return self._connected and self._writer is not None

    async def connect(self) -> None:
        if self._server is None:
            self._server = await asyncio.start_server(
                self._handle_client, self._host, self._port
            )
            logger.info(
                "[TCP:LISTEN] 🌐 Listening for SLIP bridge connections on %s:%d",
                self._host,
                self._port,
            )

        while not self._connected:
            await asyncio.sleep(0.5)

    async def _handle_client(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        peer = writer.get_extra_info("peername")
        self._client_addr = str(peer)
        logger.info(
            "[TCP:CONN] 🔌 Client connected from %s. Accepting session.",
            self._client_addr,
        )
        if self._writer is not None:
            logger.warning("[TCP:CONN] Replacing existing connection with new peer.")
            self._connected = False
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except (OSError, ConnectionResetError, BrokenPipeError):
                pass

        self._reader = reader
        self._writer = writer
        self._framer = SlipFramer()
        self._connected = True
        self._read_task = asyncio.create_task(self._read_loop())

    async def _read_loop(self) -> None:
        while self._connected and self._reader is not None:
            try:
                chunk = await self._reader.read(8192)
                if not chunk:
                    logger.warning(
                        "[TCP:CONN] 🔌 EOF on TCP connection from %s",
                        self._client_addr,
                    )
                    break
                messages = self._framer.feed(chunk)
                for msg in messages:
                    await self._queue.put(msg)
            except (
                OSError,
                ConnectionResetError,
                ConnectionAbortedError,
                BrokenPipeError,
            ) as err:
                logger.warning(
                    "[TCP:CONN] 🔌 Read failure from %s (%s) — connection closed.",
                    self._client_addr,
                    err,
                )
                break
            except asyncio.CancelledError:
                break

        self._connected = False
        if self._writer is not None:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except (
                OSError,
                ConnectionResetError,
                ConnectionAbortedError,
                BrokenPipeError,
            ):
                pass
            self._writer = None
        self._reader = None
        logger.info(
            "[TCP:CONN] 🔌 Connection teardown complete for %s", self._client_addr
        )

    async def disconnect(self) -> None:
        self._connected = False
        if self._read_task:
            self._read_task.cancel()
            try:
                await self._read_task
            except asyncio.CancelledError:
                pass
            self._read_task = None
        if self._writer is not None:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except (
                OSError,
                ConnectionResetError,
                ConnectionAbortedError,
                BrokenPipeError,
            ):
                pass
            self._writer = None
        if self._server:
            self._server.close()
            await self._server.wait_closed()
            self._server = None
        logger.info("[TCP:CONN] 🔌 TCP transport shutdown.")

    async def recv(self) -> Union[str, bytes]:
        while self.is_connected() or not self._queue.empty():
            try:
                return await asyncio.wait_for(self._queue.get(), timeout=1.0)
            except (asyncio.TimeoutError, asyncio.CancelledError):
                if not self.is_connected():
                    raise ConnectionError("TCP transport disconnected.")
        raise ConnectionError("TCP transport offline.")

    async def send(self, data: Union[str, bytes]) -> None:
        if not self.is_connected() or self._writer is None:
            raise ConnectionError("Cannot send: TCP transport offline.")
        encoded = SlipFramer.encode(data)
        try:
            self._writer.write(encoded)
            await self._writer.drain()
        except (
            OSError,
            ConnectionResetError,
            ConnectionAbortedError,
            BrokenPipeError,
        ) as err:
            self._connected = False
            logger.error(
                "[TCP:TX] Write error to peer %s: %s", self._client_addr, err
            )
            raise ConnectionError(f"TCP transmission failed: {err}")
