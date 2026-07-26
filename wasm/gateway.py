#!/usr/bin/env python3
"""StackChan Production Gateway Server CLI Entrypoint.

Transport-agnostic server with pluggable backends for physical ESP32 devices.
Supports serial (SLIP over ttyACM), TCP (SLIP over socat bridge), and WebSocket.

Usage:
  python3 gateway.py --transport=serial --serial=/dev/ttyACM1 --mode=echo
  python3 gateway.py --transport=tcp --listen=0.0.0.0:9000 --mode=gemini-live --api-key=AIza...
  python3 gateway.py --transport=ws --listen=0.0.0.0:8081 --mode=gemini-live --api-key=AIza...
"""

import argparse
import asyncio
import json
import logging
import signal
import sys
import os
from typing import Union, Optional

from gateway.serial_transport import SerialTransport
from gateway.tcp_transport import TcpTransport
from gateway.ws_transport import WebSocketTransport
from gateway.session import perform_handshake, SessionState
from gateway.audio_pipeline import AudioPipeline
from gateway.backends.echo import echo_respond
from gateway.backends.gemini_api import GeminiGatewayBackend

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
logger = logging.getLogger("gateway")


def parse_listen_address(listen_str: str, default_port: int) -> tuple[str, int]:
    """Parse 'host:port' or ':port' or 'port' into (host, port) tuple."""
    if not listen_str:
        return ("0.0.0.0", default_port)
    if ":" in listen_str:
        parts = listen_str.split(":", 1)
        host = parts[0] or "0.0.0.0"
        try:
            port = int(parts[1])
        except ValueError:
            port = default_port
        return (host, port)
    try:
        return ("0.0.0.0", int(listen_str))
    except ValueError:
        return (listen_str, default_port)


def create_transport(args: argparse.Namespace) -> Union[SerialTransport, TcpTransport, WebSocketTransport]:
    """Construct the appropriate transport based on CLI flags."""
    if args.transport == "serial":
        return SerialTransport(port=args.serial, baudrate=args.baud)
    elif args.transport == "tcp":
        host, port = parse_listen_address(args.listen or args.tcp, 9000)
        return TcpTransport(host=host, port=port)
    elif args.transport in ("ws", "websocket"):
        host, port = parse_listen_address(args.listen or args.websocket, 8081)
        return WebSocketTransport(host=host, port=port)
    else:
        logger.error("Unsupported transport: %s", args.transport)
        sys.exit(1)


async def run_session_loop(transport: Union[SerialTransport, TcpTransport, WebSocketTransport], args: argparse.Namespace) -> None:
    """Main session processing loop: handshake → audio pipeline → response → repeat."""
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop_event.set)
        except (NotImplementedError, RuntimeError, ValueError):
            pass

    try:
        await transport.connect()
        while not stop_event.is_set():
            if not transport.is_connected():
                logger.info("Awaiting incoming transport connection...")
                await transport.connect()
                if not transport.is_connected():
                    await asyncio.sleep(1.0)
                    continue

            logger.info(
                "Transport operational (%s). Initiating hello handshake...",
                transport.get_transport_name(),
            )

            # Perform hello handshake
            try:
                session = await perform_handshake(transport)
            except (asyncio.TimeoutError, ValueError, ConnectionError, BrokenPipeError, OSError) as err:
                logger.warning("Handshake interrupted: %s", err)
                await transport.disconnect()
                await asyncio.sleep(1.0)
                continue

            # Initialize Gemini Live streaming backend if configured
            is_gemini_mode = args.mode in ("gemini-api", "gemini-live", "a2a")
            gemini_backend: Optional[GeminiGatewayBackend] = None
            if is_gemini_mode:
                gemini_backend = GeminiGatewayBackend(
                    transport=transport,
                    session=session,
                    api_key=args.api_key,
                    model=args.model,
                    voice=args.voice,
                    system_instruction=args.system_instruction or "You are StackChan, a cute desk robot companion. Keep responses brief and cheerful.",
                )
                connected = await gemini_backend.start()
                if not connected:
                    logger.warning(
                        "[GATEWAY] Gemini API live streaming setup failed (check --api-key). Falling back to echo mode."
                    )
                    is_gemini_mode = False
                    gemini_backend = None

            # Build response callback based on backend mode
            async def on_utterance_complete(chunks: list[bytes]) -> None:
                if not is_gemini_mode or gemini_backend is None:
                    await echo_respond(transport, session, chunks, tenet_invert=True)
                else:
                    await gemini_backend.handle_speech_end(chunks)

            # Run audio pipeline
            pipeline = AudioPipeline(transport, session, on_utterance_complete)

            async def process_messages() -> None:
                """Dispatch incoming transport messages to audio pipeline or JSON handlers."""
                while transport.is_connected():
                    try:
                        message = await transport.recv()
                    except ConnectionError:
                        break

                    if isinstance(message, bytes):
                        if is_gemini_mode and gemini_backend:
                            await gemini_backend.forward_audio_chunk(message)
                        await pipeline.handle_audio_chunk(message)
                    elif isinstance(message, str):
                        try:
                            data = json.loads(message)
                        except json.JSONDecodeError as err:
                            logger.warning("[SESSION:%s...] JSON decode error: %s", session.session_id[:8], err)
                            continue

                        msg_type = data.get("type")
                        msg_state = data.get("state")

                        if msg_type == "listen" and msg_state == "start":
                            pipeline.reset()
                        elif msg_type == "listen" and msg_state in ("detect_end", "stop"):
                            await pipeline.handle_speech_end(f"protocol:{msg_state}")
                        elif msg_type == "vad" and msg_state == "speech_end":
                            await pipeline.handle_speech_end("firmware_vad")
                        elif msg_type == "abort":
                            logger.info("[SESSION:%s...] Session aborted by client.", session.session_id[:8])
                            pipeline.reset()
                            if is_gemini_mode and gemini_backend:
                                await gemini_backend.handle_abort()
                            if transport.is_connected():
                                try:
                                    await transport.send(json.dumps({"type": "tts", "state": "stop"}))
                                except (ConnectionError, OSError, BrokenPipeError):
                                    pass
                        elif msg_type == "config":
                            if "max_duration" in data or "max_recording_duration" in data:
                                pipeline.state.max_duration = int(
                                    data.get("max_duration", data.get("max_recording_duration", 45))
                                )
                                logger.info(
                                    "[SESSION:%s...] Max recording duration set to %ds",
                                    session.session_id[:8],
                                    pipeline.state.max_duration,
                                )

            pipeline_task = asyncio.create_task(process_messages())
            stop_waiter = asyncio.create_task(stop_event.wait())

            done, pending = await asyncio.wait(
                [pipeline_task, stop_waiter], return_when=asyncio.FIRST_COMPLETED
            )
            for task in pending:
                task.cancel()

            if gemini_backend:
                await gemini_backend.stop()

            if not transport.is_connected():
                logger.info("Client disconnected. Cycling for reconnect...")

    except asyncio.CancelledError:
        logger.info("Gateway shutdown requested via cancellation.")
    finally:
        logger.info("Shutting down transport channel and freeing sockets...")
        await transport.disconnect()
        logger.info("🛑 Gateway exited cleanly.")


def parse_cli_args() -> argparse.Namespace:
    """Parse command-line arguments for gateway configuration."""
    parser = argparse.ArgumentParser(
        description="StackChan Transport-Agnostic Production Gateway",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  python3 gateway.py --transport=serial --serial=/dev/ttyACM1 --mode=echo
  python3 gateway.py --transport=tcp --listen=0.0.0.0:9000 --mode=gemini-live --api-key=AIza...
  python3 gateway.py --transport=ws --listen=0.0.0.0:8081 --mode=gemini-live --api-key=AIza...
""",
    )
    parser.add_argument(
        "--transport",
        choices=["serial", "tcp", "ws", "websocket"],
        default="serial",
        help="Transport interface to serve (default: serial).",
    )
    parser.add_argument(
        "--serial", default="/dev/ttyACM1",
        help="Serial port for CDC communication (default: /dev/ttyACM1).",
    )
    parser.add_argument(
        "--baud", type=int, default=921600,
        help="Ceremonial serial baud rate — ignored by USB CDC (default: 921600).",
    )
    parser.add_argument(
        "--listen", "-l", default="",
        help="Listening host:port for TCP or WebSocket servers (e.g., 0.0.0.0:8081).",
    )
    parser.add_argument(
        "--tcp", default="",
        help="Alias for TCP host:port listening address (default: 0.0.0.0:9000).",
    )
    parser.add_argument(
        "--websocket", default="",
        help="Alias for WebSocket host:port address (default: 0.0.0.0:8081).",
    )
    parser.add_argument(
        "--mode",
        choices=["echo", "gemini-api", "gemini-live", "a2a"],
        default="echo",
        help="Backend conversational mode (default: echo; gemini-live/gemini-api/a2a for live AI audio streaming).",
    )
    parser.add_argument(
        "--api-key", default=os.environ.get("GEMINI_API_KEY", ""),
        help="Gemini API key (for gemini-live/gemini-api mode). Falls back to GEMINI_API_KEY env var.",
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
        help="System instruction for the Gemini model. Overrides default if specified.",
    )
    return parser.parse_args()


def main() -> None:
    """Entry point: parse CLI args, construct transport, run main loop."""
    args = parse_cli_args()

    logger.info(
        "🚀 StackChan Gateway starting. Transport: %s | Mode: %s",
        args.transport,
        args.mode,
    )

    transport = create_transport(args)

    try:
        asyncio.run(run_session_loop(transport, args))
    except KeyboardInterrupt:
        logger.info("Gateway process killed via CLI KeyboardInterrupt.")


if __name__ == "__main__":
    main()
