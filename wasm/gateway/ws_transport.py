"""WebSocket Transport Implementation for WebAssembly Gateway."""

import asyncio
import logging
import mimetypes
import os
from typing import Optional, Union, List, Tuple
import websockets
from websockets.server import ServerConnection
from websockets.exceptions import WebSocketException
from websockets.http11 import Response
from websockets.datastructures import Headers
from gateway.transport import Transport

logger = logging.getLogger("gateway.ws_transport")

WEB_ROOT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "build_wasm",
)


class WebSocketTransport(Transport):
    """WebSocket server transport supporting both JSON text and binary Opus frames."""

    def __init__(self, host: str = "0.0.0.0", port: int = 8081) -> None:
        self._host = host
        self._port = port
        self._server: Optional[websockets.server.WebSocketServer] = None
        self._active_ws: Optional[ServerConnection] = None
        self._queue: asyncio.Queue[Union[str, bytes]] = asyncio.Queue()
        self._connected = False
        self._peer_addr = ""

    def get_transport_name(self) -> str:
        return (
            f"ws:{self._peer_addr}" if self._peer_addr else f"ws:{self._port}"
        )

    def is_connected(self) -> bool:
        return self._connected and self._active_ws is not None

    async def connect(self) -> None:
        if self._server is None:
            self._server = await websockets.serve(
                self._ws_handler,
                self._host,
                self._port,
                process_request=self._process_request,
                max_size=None,
            )
            logger.info(
                "[WS:LISTEN] 🌐 WebSocket Transport & Static Handler listening on http://%s:%d/",
                self._host,
                self._port,
            )

        while not self._connected:
            await asyncio.sleep(0.5)

    def _process_request(
        self, connection: ServerConnection, request: websockets.http11.Request
    ) -> Optional[Response]:
        req_path = request.path.split("?")[0]
        if req_path in ["/ws", "/ws_monitor", "/ws_debug"]:
            return None  # Upgrade to WebSocket

        rel_path = request.path.lstrip("/")
        if not rel_path or rel_path.split("?")[0] == "":
            rel_path = "index.html"
        rel_path = rel_path.split("?")[0]

        target_path = os.path.normpath(os.path.join(WEB_ROOT, rel_path))
        if not target_path.startswith(WEB_ROOT):
            return Response(
                403,
                "Forbidden",
                Headers([("Content-Type", "text/plain")]),
                b"403 Forbidden",
            )

        if not os.path.exists(target_path) or os.path.isdir(target_path):
            return Response(
                404,
                "Not Found",
                Headers([("Content-Type", "text/plain")]),
                b"404 Not Found",
            )

        mime_type, _ = mimetypes.guess_type(target_path)
        if not mime_type:
            mime_type = "application/octet-stream"

        try:
            with open(target_path, "rb") as file_handle:
                content = file_handle.read()
            headers = Headers([
                ("Content-Type", mime_type),
                ("Content-Length", str(len(content))),
                ("Cache-Control", "no-cache, no-store, must-revalidate"),
                ("Pragma", "no-cache"),
                ("Expires", "0"),
            ])
            return Response(200, "OK", headers, content)
        except OSError as err:
            logger.error("[WS:HTTP] Error reading file %s: %s", target_path, err)
            return Response(
                500,
                "Internal Server Error",
                Headers([("Content-Type", "text/plain")]),
                b"500 Internal Server Error",
            )

    async def _ws_handler(self, websocket: ServerConnection) -> None:
        peer = websocket.remote_address
        self._peer_addr = str(peer)
        logger.info(
            "[WS:CONN] 🔌 WebSocket connected from %s. Initializing channel.",
            self._peer_addr,
        )
        self._active_ws = websocket
        self._connected = True

        try:
            async for message in websocket:
                await self._queue.put(message)
        except (
            WebSocketException,
            ConnectionResetError,
            ConnectionAbortedError,
            BrokenPipeError,
        ) as err:
            logger.warning(
                "[WS:CONN] 🔌 Connection closed by peer %s (%s)",
                self._peer_addr,
                err,
            )
        except asyncio.CancelledError:
            pass
        finally:
            self._connected = False
            self._active_ws = None
            logger.info("[WS:CONN] 🔌 Channel teardown complete for %s", self._peer_addr)

    async def disconnect(self) -> None:
        self._connected = False
        if self._active_ws is not None:
            try:
                await self._active_ws.close()
            except (WebSocketException, OSError, ConnectionError):
                pass
            self._active_ws = None
        if self._server:
            self._server.close()
            await self._server.wait_closed()
            self._server = None
        logger.info("[WS:CONN] 🔌 WebSocket transport shutdown.")

    async def recv(self) -> Union[str, bytes]:
        while self.is_connected() or not self._queue.empty():
            try:
                return await asyncio.wait_for(self._queue.get(), timeout=1.0)
            except (asyncio.TimeoutError, asyncio.CancelledError):
                if not self.is_connected():
                    raise ConnectionError("WebSocket transport disconnected.")
        raise ConnectionError("WebSocket transport offline.")

    async def send(self, data: Union[str, bytes]) -> None:
        if not self.is_connected() or self._active_ws is None:
            raise ConnectionError("Cannot send: WebSocket transport offline.")
        try:
            await self._active_ws.send(data)
        except (
            WebSocketException,
            OSError,
            BrokenPipeError,
            ConnectionError,
        ) as err:
            self._connected = False
            logger.error("[WS:TX] Transmission failure to %s: %s", self._peer_addr, err)
            raise ConnectionError(f"WebSocket send error: {err}")
