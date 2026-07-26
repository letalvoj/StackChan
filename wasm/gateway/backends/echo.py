"""Echo backend: receive PCM audio, trim silence, reverse, and stream back."""

import asyncio
import json
import logging
import struct

from audio_dsp import compute_rms_normalized, RMS_UI_SCALE
from gateway.session import SessionState
from gateway.transport import Transport

logger = logging.getLogger("gateway")


async def echo_respond(
    transport: Transport,
    session: SessionState,
    received_chunks: list[bytes],
    tenet_invert: bool = True,
) -> None:
    """Echo backend: reverse received PCM audio and stream it back.

    Steps:
        1. Trim trailing silence (RMS < 0.015)
        2. Reverse samples (tenet-style echo)
        3. Send protocol messages: stt, llm, tts start, sentence_start
        4. Stream reversed audio in 1920-byte chunks at 60ms intervals
        5. Send tts stop
    """
    sid = session.session_id[:8]

    if not received_chunks:
        return

    # Trim trailing silence
    trimmed = list(received_chunks)
    while len(trimmed) > 5 and compute_rms_normalized(trimmed[-1]) < 0.015:
        trimmed.pop()

    trimmed_count = len(received_chunks) - len(trimmed)
    logger.info(
        "[ECHO:%s...] Speech burst complete. Trimmed %d silent tail chunks. "
        "Responding to %d active audio chunks.",
        sid,
        trimmed_count,
        len(trimmed),
    )

    # Send protocol sequence
    await transport.send(json.dumps({"type": "stt", "text": "Echo received"}))
    await asyncio.sleep(0.01)
    await transport.send(json.dumps({"type": "llm", "emotion": "happy"}))
    await asyncio.sleep(0.01)
    await transport.send(json.dumps({"type": "tts", "state": "start"}))
    await asyncio.sleep(0.01)
    await transport.send(
        json.dumps({
            "type": "tts",
            "state": "sentence_start",
            "text": "Echo Bounce Complete ~",
        })
    )

    if trimmed:
        full_pcm = b"".join(trimmed)
        num_total = len(full_pcm) // 2
        if num_total > 0:
            samples = list(struct.unpack(f"<{num_total}h", full_pcm))
            if tenet_invert:
                samples.reverse()
            reversed_pcm = struct.pack(f"<{num_total}h", *samples)

            chunk_size = 1920  # 960 samples * 2 bytes
            for i in range(0, len(reversed_pcm), chunk_size):
                chunk = reversed_pcm[i : i + chunk_size]
                await transport.send(chunk)
                await asyncio.sleep(0.06)

    await asyncio.sleep(0.1)
    await transport.send(json.dumps({"type": "tts", "state": "stop"}))
    logger.info("[ECHO:%s...] Echo response complete.", sid)
