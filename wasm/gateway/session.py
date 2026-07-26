"""Session lifecycle: hello handshake, session ID generation, and state management."""

import asyncio
import json
import logging
import uuid
from dataclasses import dataclass

from gateway.transport import Transport

logger = logging.getLogger("gateway")


@dataclass
class SessionState:
    """Tracks the state of an active device session."""

    session_id: str
    device_id: str = "unknown"
    client_id: str = "unknown"
    sample_rate: int = 16000
    frame_duration: int = 60
    is_active: bool = True


async def perform_handshake(transport: Transport) -> SessionState:
    """Wait for device hello, respond with server hello, return session state.

    Raises:
        asyncio.TimeoutError: If no hello is received within 10 seconds.
        ValueError: If the hello message is malformed.
    """
    # Wait for hello from device (10s timeout)
    raw_hello = await asyncio.wait_for(transport.recv(), timeout=10.0)
    if not isinstance(raw_hello, str):
        raise ValueError("Expected JSON hello message, got binary")

    hello_data = json.loads(raw_hello)
    if hello_data.get("type") != "hello":
        raise ValueError(f"Expected hello, got {hello_data.get('type')}")

    session_id = str(uuid.uuid4())
    device_id = str(hello_data.get("device_id", "unknown"))
    client_id = str(hello_data.get("client_id", "unknown"))

    # Parse audio params from device hello
    audio_params = hello_data.get("audio_params", {})
    sample_rate = int(audio_params.get("sample_rate", 16000))
    frame_duration = int(audio_params.get("frame_duration", 60))

    session = SessionState(
        session_id=session_id,
        device_id=device_id,
        client_id=client_id,
        sample_rate=sample_rate,
        frame_duration=frame_duration,
    )

    sid_prefix = session_id[:8]
    logger.info(
        "[SESSION:%s...] Hello from device '%s', transport='%s'",
        sid_prefix,
        device_id,
        hello_data.get("transport", "?"),
    )

    # Send server hello response
    server_hello = json.dumps({
        "type": "hello",
        "transport": hello_data.get("transport", "unknown"),
        "session_id": session_id,
        "audio_params": {
            "sample_rate": sample_rate,
            "frame_duration": frame_duration,
        },
    })
    await transport.send(server_hello)
    logger.info("[SESSION:%s...] Handshake complete. Session established.", sid_prefix)

    return session
