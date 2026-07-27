"""Asynchronous HTTP static file server and AI duplex WebSocket server for WasmChan.

Supports two backend modes:
  --mode=echo        (default) Echo/TENET backend — accumulates speech, echoes back
  --mode=gemini-live  Gemini Live API — real-time audio-to-audio AI conversation
"""

import argparse
import asyncio
import collections
from dataclasses import dataclass, field
import json
import math
import os
import struct
import sys
import logging
import time
import urllib.parse
import uuid
from typing import Optional
import websockets
import websockets.http11
from debug_telemetry import FlushedFileHandler, WebSocketLogHandler
from gateway.backends.gemini_api import GeminiLiveSession
from audio_dsp import compute_rms_normalized, RMS_UI_SCALE

# Patch websockets.http11.Request.parse to permit OPTIONS, POST, and HEAD methods for CORS preflight & REST endpoints
@classmethod
def _tolerant_request_parse(cls, read_line):
    request_line = yield from websockets.http11.parse_line(read_line)
    try:
        method, raw_path, protocol = request_line.split(b" ", 2)
    except ValueError:
        raise ValueError(f"invalid HTTP request line: {request_line}") from None
    if protocol != b"HTTP/1.1":
        raise ValueError(f"unsupported protocol; expected HTTP/1.1: {request_line}")
    if method not in [b"GET", b"OPTIONS", b"POST", b"HEAD"]:
        raise ValueError(f"unsupported HTTP method: {method}")
    path = raw_path.decode("ascii", "surrogateescape")
    headers = yield from websockets.http11.parse_headers(read_line)
    
    req = cls(path, headers)
    req.method = method.decode("ascii", "surrogateescape")
    return req

websockets.http11.Request.parse = _tolerant_request_parse

logger = logging.getLogger("wasmchan") # Root reference for module functions; configured in main()

PORT = 8081
DIRECTORY = "build_wasm"

# Module-level config populated by argparse in main()
server_config: argparse.Namespace = argparse.Namespace(
    mode="echo", api_key="", model="models/gemini-2.5-flash-live",
    voice="Puck", system_instruction="", port=PORT,
)


@dataclass
class ClientSession:
    device_id: str
    client_id: str
    token: str
    session_id: str
    protocol_ws: Optional[websockets.WebSocketServerProtocol] = None
    monitor_ws: Optional[websockets.WebSocketServerProtocol] = None
    debug_ws: Optional[websockets.WebSocketServerProtocol] = None
    chunks: list = field(default_factory=list)
    phase: str = "idle"
    last_logged_phase: str = "idle"
    speech_active: bool = False
    tenet: bool = False
    max_duration: int = 45
    response_task: Optional[asyncio.Task] = None
    abort_event: asyncio.Event = field(default_factory=asyncio.Event)
    last_audio_time: float = field(default_factory=time.monotonic)
    gemini_session: Optional[GeminiLiveSession] = None


# Keyed by ephemeral session_id UUID string
sessions: dict[str, ClientSession] = {}


def process_request(connection, request):
    """Serve static web files or upgrade WebSocket requests."""
    req_path = request.path.split("?")[0]
    if req_path in ["/ws", "/ws_monitor", "/ws_debug"]:
        return None

    origin = request.headers.get("Origin", "*")
    cors_headers = [
        ("Access-Control-Allow-Origin", origin),
        ("Access-Control-Allow-Methods", "GET, POST, OPTIONS, HEAD, PUT, DELETE"),
        ("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With, Range, Accept"),
        ("Access-Control-Allow-Credentials", "true"),
        ("Access-Control-Expose-Headers", "Content-Length, Content-Range, Content-Type"),
        ("Cross-Origin-Resource-Policy", "cross-origin"),
        ("Cross-Origin-Opener-Policy", "same-origin-allow-popups"),
        ("Cross-Origin-Embedder-Policy", "unsafe-none"),
    ]

    method = getattr(request, "method", "GET")
    if method == "OPTIONS":
        return websockets.http11.Response(
            204,
            "No Content",
            websockets.Headers(cors_headers),
            b"",
        )

    if req_path == "/vision":
        body = json.dumps({
            "explanation": "Simulated Vision Response from Dev Server (port 8081): Active WebRTC webcam stream processed cleanly over HTTPS/HTTP dev server.",
            "status": "success"
        }).encode("utf-8")
        vision_headers = cors_headers + [
            ("Content-Type", "application/json"),
            ("Cache-Control", "no-cache"),
        ]
        return websockets.http11.Response(200, "OK", websockets.Headers(vision_headers), body)

    path = request.path.lstrip("/")
    if not path:
        path = "index.html"
    file_path = os.path.join(DIRECTORY, path)
    if not os.path.exists(file_path) or os.path.isdir(file_path):
        return websockets.http11.Response(
            404,
            "Not Found",
            websockets.Headers(cors_headers + [("Content-Type", "text/plain")]),
            b"404 Not Found",
        )

    ext = os.path.splitext(file_path)[1]
    ct_map = {
        ".html": "text/html",
        ".js": "application/javascript",
        ".wasm": "application/wasm",
        ".css": "text/css",
        ".json": "application/json",
        ".bin": "application/octet-stream",
    }
    content_type = ct_map.get(ext, "application/octet-stream")
    with open(file_path, "rb") as f:
        body = f.read()

    headers = websockets.Headers(cors_headers + [
        ("Content-Type", content_type),
        ("Cache-Control", "no-cache, no-store, must-revalidate"),
        ("Pragma", "no-cache"),
        ("Expires", "0"),
    ])
    return websockets.http11.Response(200, "OK", headers, body)


async def emit_monitor(session: ClientSession, payload: dict):
    """Transmit telemetry payload to the session's specific monitor WebSocket."""
    if session.monitor_ws:
        try:
            await session.monitor_ws.send(json.dumps(payload))
        except (websockets.exceptions.WebSocketException, ConnectionResetError, ConnectionAbortedError, BrokenPipeError, RuntimeError) as err:
            logger.debug(f"📊 [MONITOR:{session.session_id[:8]}...] Monitor channel error during send: {type(err).__name__}")
            session.monitor_ws = None


async def emit_debug(session: ClientSession, payload: dict):
    """Transmit telemetry payload to the session's specific debug WebSocket."""
    if session.debug_ws:
        try:
            await session.debug_ws.send(json.dumps(payload))
        except (websockets.exceptions.WebSocketException, ConnectionResetError, ConnectionAbortedError, BrokenPipeError, RuntimeError) as err:
            logger.debug(f"🔧 [DEBUG:{session.session_id[:8]}...] Debug channel error during send: {type(err).__name__}")
            session.debug_ws = None


def dispatch_monitor(session: ClientSession, payload: dict):
    """Fire-and-forget non-blocking dispatch of monitor telemetry to eliminate audio streaming I/O overhead."""
    if session.monitor_ws:
        asyncio.create_task(emit_monitor(session, payload))


def dispatch_debug(session: ClientSession, payload: dict):
    """Fire-and-forget non-blocking dispatch of debug telemetry to eliminate audio streaming I/O overhead."""
    if session.debug_ws:
        asyncio.create_task(emit_debug(session, payload))


async def broadcast_all_debug(payload: dict):
    """Broadcast log payloads to all active debug UI connections across existing sessions."""
    for sess in list(sessions.values()):
        await emit_debug(sess, payload)


async def handle_monitor_client(websocket):
    """Manage lifecycle of an oscilloscope telemetry monitoring client bound to an ephemeral session."""
    parsed_url = urllib.parse.urlparse(websocket.request.path)
    query_params = urllib.parse.parse_qs(parsed_url.query)
    session_id = query_params.get("session_id", [None])[0]

    if not session_id or session_id not in sessions:
        logger.debug(f"📊 [MONITOR] Rejected unauthenticated connection from {websocket.remote_address} (missing or unknown session_id)")
        await websocket.close(1008, "Invalid or missing session_id")
        return

    session = sessions[session_id]
    session.monitor_ws = websocket
    logger.info(f"📊 [MONITOR:{session_id[:8]}...] Client bound to session: {websocket.remote_address}")
    try:
        await websocket.wait_closed()
    finally:
        if session.monitor_ws == websocket:
            session.monitor_ws = None
        logger.info(f"📊 [MONITOR:{session_id[:8]}...] Client disconnected")


async def handle_debug_client(websocket):
    """Manage lifecycle of a telemetry debug UI client bound to an ephemeral session."""
    parsed_url = urllib.parse.urlparse(websocket.request.path)
    query_params = urllib.parse.parse_qs(parsed_url.query)
    session_id = query_params.get("session_id", [None])[0]

    if not session_id or session_id not in sessions:
        logger.debug(f"🔧 [DEBUG] Rejected unauthenticated connection from {websocket.remote_address} (missing or unknown session_id)")
        await websocket.close(1008, "Invalid or missing session_id")
        return

    session = sessions[session_id]
    session.debug_ws = websocket
    logger.info(f"🔧 [DEBUG:{session_id[:8]}...] Client bound to session: {websocket.remote_address}")
    try:
        await websocket.wait_closed()
    finally:
        if session.debug_ws == websocket:
            session.debug_ws = None
        logger.info(f"🔧 [DEBUG:{session_id[:8]}...] Client disconnected")


async def respond_to_utterance(session: ClientSession, received_pcm_chunks: list[bytes], tenet_invert: bool = False, abort_event: asyncio.Event = None, on_finish = None):
    """Transmit protocol JSON and optionally time-reversed PCM audio responding to client speech."""
    websocket = session.protocol_ws
    if not websocket or not received_pcm_chunks:
        if on_finish:
            await on_finish()
        return

    # Trim leading and trailing silence (strip chunks with RMS < 0.015 from start and end of burst)
    trimmed_chunks = list(received_pcm_chunks)
    while len(trimmed_chunks) > 1 and compute_rms_normalized(trimmed_chunks[0]) < 0.015:
        trimmed_chunks.pop(0)
    while len(trimmed_chunks) > 1 and compute_rms_normalized(trimmed_chunks[-1]) < 0.015:
        trimmed_chunks.pop()

    logger.info(f"🎙️ [PROTOCOL:{session.session_id[:8]}...] Speech burst complete. Trimmed {len(received_pcm_chunks) - len(trimmed_chunks)} silent chunks. Responding to {len(trimmed_chunks)} active audio chunks...")
    await websocket.send(json.dumps({
        "type": "stt",
        "text": "Real duplex PCM stream received by port 8081",
    }))
    await asyncio.sleep(0.01)
    await websocket.send(json.dumps({"type": "llm", "emotion": "happy"}))
    await asyncio.sleep(0.01)
    await websocket.send(json.dumps({"type": "tts", "state": "start", "duration_ms": int(len(trimmed_chunks) * 60), "total_chunks": len(trimmed_chunks)}))
    await asyncio.sleep(0.01)
    await websocket.send(json.dumps({
        "type": "tts",
        "state": "sentence_start",
        "text": "Echo Bounce Complete ~",
    }))

    if trimmed_chunks:
        full_pcm = b"".join(trimmed_chunks)
        num_total = len(full_pcm) // 2
        total_speech_duration_in = num_total / 16000.0  # Exact input duration @ 16kHz

        if num_total > 0:
            samples = list(struct.unpack(f"<{num_total}h", full_pcm))
            if tenet_invert:
                samples.reverse()  # TENET time-inversion playback
            reversed_pcm = struct.pack(f"<{num_total}h", *samples)

            # Standard 60ms framing: 1920 bytes = 960 samples @ 16kHz per network transmission
            chunk_size = 1920
            total_chunks = (len(reversed_pcm) + chunk_size - 1) // chunk_size
            start_time = time.monotonic()

            for chunk_idx, i in enumerate(range(0, len(reversed_pcm), chunk_size)):
                if abort_event and abort_event.is_set():
                    logger.info(f"🛑 [SERVER:TTS:{session.session_id[:8]}...] Response streaming interrupted mid-flight via abort event!")
                    break
                chunk = reversed_pcm[i : i + chunk_size]
                
                # Await strictly the main audio protocol transmission
                await websocket.send(chunk)
                
                # Fire-and-forget non-blocking telemetry dispatch (eliminating sequential I/O bottlenecks over HTTPS)
                rms_val = compute_rms_normalized(chunk)
                dispatch_monitor(session, {"type": "downlink_rms", "rms": rms_val})
                dispatch_debug(session, {
                    "type": "progress",
                    "sent": chunk_idx + 1,
                    "total": total_chunks,
                    "remaining_seconds": round(max(0.0, total_speech_duration_in - (chunk_idx + 1) * 0.06), 2),
                })

            # Await remaining physical playback duration so tts.stop arrives precisely as the final PCM chunk finishes playing
            final_target = start_time + total_speech_duration_in
            now = time.monotonic()
            if now < final_target:
                await asyncio.sleep(final_target - now)

            end_time = time.monotonic()
            total_playback_duration_out = end_time - start_time
            diff_sec = abs(total_speech_duration_in - total_playback_duration_out)
            diff_pct = (diff_sec / max(0.001, total_speech_duration_in)) * 100.0
            logger.info(
                f"⚖️ [SERVER:TIMING:{session.session_id[:8]}...] Audio Round-Trip Parity Confirmed! "
                f"Total speech duration IN: {total_speech_duration_in:.3f}s | "
                f"Total playback duration OUT: {total_playback_duration_out:.3f}s | "
                f"Difference: {diff_sec:.3f}s ({diff_pct:.2f}%)"
            )

    await asyncio.sleep(0.05)
    try:
        await websocket.send(json.dumps({"type": "tts", "state": "stop", "duration_ms": int(len(trimmed_chunks) * 60), "total_chunks": len(trimmed_chunks)}))
    except (websockets.exceptions.WebSocketException, ConnectionError, RuntimeError):
        pass
    if on_finish:
        await on_finish()


async def handle_protocol_client(websocket):
    """Handle C++ WasmProtocol duplex WebSocket session driven by application handshake and Silero VAD events."""
    logger.info(f"🔌 [PROTOCOL] Connection initiated on /ws from {websocket.remote_address}. Awaiting application hello handshake...")

    try:
        raw_hello = await asyncio.wait_for(websocket.recv(), timeout=3.0)
    except (asyncio.TimeoutError, websockets.exceptions.ConnectionClosed):
        logger.warning(f"⏳ [PROTOCOL] Timed out or closed waiting for initial hello handshake from {websocket.remote_address}. Closing connection.")
        await websocket.close(1008, "Handshake timeout")
        return

    if not isinstance(raw_hello, str):
        logger.warning(f"🚫 [PROTOCOL] Received unexpected binary chunk during handshake from {websocket.remote_address}. Closing connection.")
        await websocket.close(1008, "Expected JSON hello message")
        return

    try:
        hello_data = json.loads(raw_hello)
    except json.JSONDecodeError as err:
        logger.warning(f"⚠️ [PROTOCOL] Failed to parse hello JSON from {websocket.remote_address}: {err}")
        await websocket.close(1008, "Invalid JSON in handshake")
        return

    if hello_data.get("type") != "hello":
        logger.warning(f"🚫 [PROTOCOL] Expected message type 'hello', received '{hello_data.get('type')}' from {websocket.remote_address}")
        await websocket.close(1008, "Expected message type hello")
        return

    device_id = str(hello_data.get("device_id", "unknown_mac"))
    client_id = str(hello_data.get("client_id", "unknown_uuid"))
    token = str(hello_data.get("token", ""))
    session_id = str(uuid.uuid4())

    session = ClientSession(
        device_id=device_id,
        client_id=client_id,
        token=token,
        session_id=session_id,
        protocol_ws=websocket,
    )
    sessions[session_id] = session

    await websocket.send(json.dumps({
        "type": "hello",
        "transport": "websocket",
        "session_id": session_id,
        "audio_params": {"sample_rate": 16000, "frame_duration": 60}
    }))
    logger.info(f"🤝 [PROTOCOL:{session_id[:8]}...] Handshake complete with device '{device_id}' -> Assigned session '{session_id[:8]}...'")

    # Initialize Gemini Live session if in gemini-live mode
    is_gemini_mode = server_config.mode == "gemini-live"
    if is_gemini_mode:
        if not server_config.api_key:
            logger.error(f"🚫 [PROTOCOL:{session_id[:8]}...] --mode=gemini-live requires --api-key. Falling back to echo mode.")
            is_gemini_mode = False
        else:
            async def send_json_to_firmware(json_payload: dict) -> None:
                try:
                    await websocket.send(json.dumps(json_payload))
                except (websockets.exceptions.WebSocketException, ConnectionError, RuntimeError):
                    pass

            async def send_audio_to_firmware(pcm_bytes: bytes) -> None:
                try:
                    await websocket.send(pcm_bytes)
                except (websockets.exceptions.WebSocketException, ConnectionError, RuntimeError):
                    pass

            async def on_gemini_monitor(payload: dict) -> None:
                await emit_monitor(session, payload)

            gemini_session = GeminiLiveSession(
                api_key=server_config.api_key,
                send_json_to_firmware=send_json_to_firmware,
                send_audio_to_firmware=send_audio_to_firmware,
                on_monitor_emit=on_gemini_monitor,
                model=server_config.model,
                voice=server_config.voice,
                system_instruction=server_config.system_instruction or "You are StackChan, a cute desk robot companion. Keep responses brief and cheerful.",
            )
            session.gemini_session = gemini_session
            try:
                await gemini_session.connect()
                logger.info(f"🤖 [PROTOCOL:{session_id[:8]}...] Gemini Live session established (mode=gemini-live)")
            except (websockets.exceptions.WebSocketException, ConnectionError, OSError) as gemini_err:
                logger.error(f"🚫 [PROTOCOL:{session_id[:8]}...] Failed to connect to Gemini Live API: {gemini_err}. Falling back to echo mode.")
                session.gemini_session = None
                is_gemini_mode = False

    async def emit_state():
        buffered_bytes = sum(len(c) for c in session.chunks)
        buffered_seconds = buffered_bytes / 2 / 16000  # int16 @ 16kHz

        # STRICT GRADIENT LOGGING: Only write on phase boundary transitions (Component 5)
        if session.phase != session.last_logged_phase:
            logger.info(
                f"🔄 [SERVER:STATE:{session.session_id[:8]}...] Phase gradient: "
                f"'{session.last_logged_phase}' -> '{session.phase}' "
                f"| Buffer Snapshot: {len(session.chunks)} chunks ({buffered_seconds:.2f}s)"
            )
            session.last_logged_phase = session.phase

        await emit_debug(session, {
            "type": "state",
            "phase": session.phase,
            "buffered_seconds": round(buffered_seconds, 2),
            "chunk_count": len(session.chunks),
            "tenet": session.tenet,
            "max_duration": session.max_duration,
        })

    async def trigger_response(reason: str):
        if not session.chunks:
            return
        logger.info(f"🎤 [SERVER:VAD:{session_id[:8]}...] Speech end triggered via {reason} ({len(session.chunks)} chunks)")
        chunks_to_send = list(session.chunks)
        session.phase = "responding"
        session.speech_active = False
        await emit_state()
        session.chunks.clear()

        if session.response_task and not session.response_task.done():
            session.abort_event.set()
            session.response_task.cancel()
        session.abort_event = asyncio.Event()

        async def on_finish():
            if session.phase == "responding":
                session.phase = "idle"
                await emit_state()

        session.response_task = asyncio.create_task(
            respond_to_utterance(session, chunks_to_send, session.tenet, abort_event=session.abort_event, on_finish=on_finish)
        )

    await emit_state()

    async def connectivity_watchdog():
        while True:
            await asyncio.sleep(5)
            if session.phase != "responding" and session.speech_active:
                elapsed = time.monotonic() - session.last_audio_time
                if elapsed > 30.0:
                    logger.warning(f"⏰ [SERVER:{session_id[:8]}...] No audio for {elapsed:.0f}s. Connection stale.")
                    session.phase = "idle"
                    session.speech_active = False
                    session.chunks.clear()
                    await emit_state()

    watchdog_task = asyncio.create_task(connectivity_watchdog())

    try:
        async for message in websocket:
            if isinstance(message, bytes):
                rms_val = compute_rms_normalized(message)
                dispatch_monitor(session, {"type": "uplink_rms", "rms": rms_val})

                if is_gemini_mode and session.gemini_session:
                    # Gemini Live mode: forward audio to Gemini in real-time
                    # Drop audio during active response (barge-in handled by Gemini)
                    await session.gemini_session.forward_audio_chunk(message)
                    session.last_audio_time = time.monotonic()
                    if not session.speech_active:
                        session.speech_active = True
                        session.phase = "streaming_to_gemini"
                        logger.info(f"🗣️ [SERVER:RX:{session_id[:8]}...] Streaming audio to Gemini Live.")
                        await emit_state()
                else:
                    # Echo mode: accumulate chunks for batch response
                    # In-flight audio rejection guarantee: drop frames received while response is active
                    if session.phase == "responding":
                        logger.debug(f"🔇 [SERVER:RX:{session_id[:8]}...] Dropping in-flight audio chunk received while in 'responding' phase.")
                        continue

                    # Server trusts firmware VAD gate: all received audio is speech-relevant
                    session.chunks.append(message)

                    # Check max recording duration limit (at 60ms per Opus chunk)
                    if session.max_duration > 0 and (len(session.chunks) * 0.06) >= session.max_duration:
                        logger.info(f"⏰ [SERVER:VAD:{session_id[:8]}...] Max recording duration limit ({session.max_duration}s) reached! Automatically closing turn.")
                        await trigger_response(f"server max recording duration cap ({session.max_duration}s)")
                        continue

                    # Phase gradient logging
                    if not session.speech_active:
                        session.speech_active = True
                        session.phase = "accumulating"
                        logger.info(f"🗣️ [SERVER:RX:{session_id[:8]}...] First audio frame received. Accumulating.")
                        await emit_state()
                    else:
                        await emit_state()

                    # Connectivity safety net: reset last-audio timestamp
                    session.last_audio_time = time.monotonic()
            elif isinstance(message, str):
                logger.info(f"📨 [PROTOCOL:RX:{session_id[:8]}...] {message}")
                try:
                    data = json.loads(message)
                except json.JSONDecodeError as err:
                    logger.warning(f"⚠️ [PROTOCOL:{session_id[:8]}...] JSON decode error: {err}")
                    continue
                if data.get("type") == "listen" and data.get("state") == "start":
                    logger.info(f"👂 [PROTOCOL:{session_id[:8]}...] listen.start received! Clean turn.")
                    session.chunks.clear()
                    session.speech_active = False
                    session.phase = "idle"
                    if session.response_task and not session.response_task.done():
                        session.abort_event.set()
                        try:
                            session.response_task.cancel()
                        except asyncio.CancelledError:
                            pass
                    await emit_state()
                elif data.get("type") == "listen" and (data.get("state") == "detect_end" or data.get("state") == "stop"):
                    if session.chunks:
                        await trigger_response(f"protocol hint {data.get('state')}")
                    else:
                        logger.info(f"⚠️ [SERVER:{session_id[:8]}...] detect_end received but no audio accumulated. Ignoring.")
                        session.phase = "idle"
                        session.speech_active = False
                        await emit_state()
                elif data.get("type") == "vad" and data.get("state") == "speech_end":
                    if session.chunks:
                        await trigger_response("legacy VAD hint")
                elif data.get("type") == "config":
                    if "tenet" in data:
                        session.tenet = bool(data["tenet"])
                        logger.info(f"⚙️ [PROTOCOL:{session_id[:8]}...] Tenet invert mode set to: {session.tenet}")
                    if "max_duration" in data or "max_recording_duration" in data:
                        session.max_duration = int(data.get("max_duration", data.get("max_recording_duration", 45)))
                        logger.info(f"⚙️ [PROTOCOL:{session_id[:8]}...] Max recording duration cap set to: {session.max_duration}s")
                    await emit_state()
                elif data.get("type") == "abort":
                    logger.info(f"🛑 [PROTOCOL:{session_id[:8]}...] Session aborted by client! Cancelling streaming task and sending tts.stop.")
                    session.chunks.clear()
                    session.speech_active = False
                    session.phase = "idle"
                    if is_gemini_mode and session.gemini_session:
                        await session.gemini_session.abort_response()
                    if session.response_task and not session.response_task.done():
                        session.abort_event.set()
                        try:
                            session.response_task.cancel()
                        except asyncio.CancelledError:
                            pass
                    try:
                        await websocket.send(json.dumps({"type": "tts", "state": "stop"}))
                    except (websockets.exceptions.WebSocketException, ConnectionError, RuntimeError):
                        pass
                    await emit_state()
    except (websockets.exceptions.WebSocketException, ConnectionResetError, ConnectionAbortedError, BrokenPipeError, RuntimeError):
        logger.info(f"🔌 [PROTOCOL:{session_id[:8]}...] Client disconnected cleanly")
    finally:
        if watchdog_task and not watchdog_task.done():
            watchdog_task.cancel()
        if session.response_task and not session.response_task.done():
            session.abort_event.set()
            try:
                session.response_task.cancel()
            except asyncio.CancelledError:
                pass
        if session.gemini_session:
            try:
                await session.gemini_session.disconnect()
            except (websockets.exceptions.WebSocketException, ConnectionError, RuntimeError, asyncio.CancelledError):
                pass
        if session.monitor_ws:
            try:
                await session.monitor_ws.close(1000, "Protocol session cleanly terminated")
            except (websockets.exceptions.WebSocketException, ConnectionError, RuntimeError):
                pass
        if session.debug_ws:
            try:
                await session.debug_ws.close(1000, "Protocol session cleanly terminated")
            except (websockets.exceptions.WebSocketException, ConnectionError, RuntimeError):
                pass
        sessions.pop(session_id, None)
        logger.info(f"🔌 [PROTOCOL:{session_id[:8]}...] WasmProtocol session purged from registry")


async def handle_websocket(websocket):
    """Route incoming WebSocket connections by request path."""
    req_path = websocket.request.path.split("?")[0]
    if req_path == "/ws_debug":
        await handle_debug_client(websocket)
    elif req_path == "/ws_monitor":
        await handle_monitor_client(websocket)
    elif req_path == "/ws":
        await handle_protocol_client(websocket)


def parse_cli_args() -> argparse.Namespace:
    """Parse command-line arguments for server configuration."""
    parser = argparse.ArgumentParser(
        description="WasmChan duplex AI server with pluggable backends.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 serve.py                                    # Echo mode (default)
  python3 serve.py --mode=gemini-live --api-key=AIza.. # Gemini Live API
  python3 serve.py --mode=gemini-live --api-key=AIza.. --voice=Kore --port=9090
""",
    )
    parser.add_argument(
        "--mode", choices=["echo", "gemini-live"], default="echo",
        help="Backend mode: 'echo' (default) or 'gemini-live' for real AI conversation.",
    )
    parser.add_argument(
        "--api-key", default=os.environ.get("GEMINI_API_KEY", ""),
        help="Gemini API key. Falls back to GEMINI_API_KEY env var.",
    )
    parser.add_argument(
        "--model", default="models/gemini-2.5-flash-live",
        help="Gemini model name (default: models/gemini-2.5-flash-live).",
    )
    parser.add_argument(
        "--voice", default="Puck",
        help="Gemini voice name for TTS output (default: Puck).",
    )
    parser.add_argument(
        "--system-instruction", default="",
        help="System instruction for the Gemini model. Overrides the default.",
    )
    parser.add_argument(
        "--port", type=int, default=PORT,
        help=f"Server port (default: {PORT}).",
    )
    return parser.parse_args()


async def main():
    """Launch asynchronous static file handler and AI WebSocket server."""
    global server_config
    server_config = parse_cli_args()
    effective_port = server_config.port

    logger.setLevel(logging.DEBUG)
    if not logger.handlers:
        console_handler = logging.StreamHandler(sys.stdout)
        console_handler.setLevel(logging.INFO)
        console_handler.setFormatter(logging.Formatter("%(message)s"))
        logger.addHandler(console_handler)

        log_dir = "/tmp/serve_logs"
        os.makedirs(log_dir, exist_ok=True)
        run_log_file = f"{log_dir}/serve_{time.strftime('%Y%m%d_%H%M%S')}.log"
        latest_symlink = "/tmp/serve_latest.log"

        try:
            if os.path.exists(latest_symlink) or os.path.islink(latest_symlink):
                os.unlink(latest_symlink)
            os.symlink(run_log_file, latest_symlink)
        except OSError as err:
            sys.stderr.write(f"[WARNING] Failed to update symlink {latest_symlink}: {err}\n")

        file_handler = FlushedFileHandler(run_log_file, mode="w", encoding="utf-8")
        file_handler.setLevel(logging.DEBUG)
        file_handler.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(message)s"))
        logger.addHandler(file_handler)

        ws_handler = WebSocketLogHandler(
            emitter_callback=lambda payload: asyncio.create_task(broadcast_all_debug(payload)),
            has_clients_callback=lambda: any(s.debug_ws is not None for s in sessions.values())
        )
        ws_handler.setLevel(logging.INFO)
        ws_handler.setFormatter(logging.Formatter("%(message)s"))
        logger.addHandler(ws_handler)

    if not os.path.exists(DIRECTORY):
        logger.error(
            f"[ERROR] Directory '{DIRECTORY}' does not exist. Please run 'make build' first."
        )
        sys.exit(1)

    mode_label = f"mode={server_config.mode}"
    if server_config.mode == "gemini-live":
        if not server_config.api_key:
            logger.error("[ERROR] --mode=gemini-live requires --api-key or GEMINI_API_KEY env var.")
            sys.exit(1)
        mode_label += f" model={server_config.model} voice={server_config.voice}"

    async with websockets.serve(
        handle_websocket, "0.0.0.0", effective_port, process_request=process_request
    ):
        logger.info(
            f"🌐 [DEV SERVER] Duplex AI Server & HTTP Static Handler running on http://0.0.0.0:{effective_port}/ ({mode_label})"
        )
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
