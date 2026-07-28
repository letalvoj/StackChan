"""End-to-end: does serve.py actually speak each client's audio format?

Runs the real server as a subprocess and connects as both kinds of client. This
is the test that would have caught the "tests pass but audio is garbage" gap --
the unit tests prove the codec is correct in isolation, this proves it is
actually wired into the server on both the uplink and the downlink.

    cd wasm && ./.venv/bin/python -m pytest tests -q
"""

import asyncio
import json
import math
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest
import websockets

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from audio_codec import OpusCodec, PcmCodec, pcm_to_samples  # noqa: E402

SR, FRAME_MS = 16000, 60


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture(scope="module")
def server():
    port = free_port()
    proc = subprocess.Popen(
        [str(ROOT / ".venv/bin/python"), "serve.py", "--port", str(port)],
        cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    # Wait for the port to accept, rather than sleeping a guessed interval.
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            pytest.fail(f"server exited early:\n{proc.stdout.read()}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                break
        except OSError:
            time.sleep(0.1)
    else:
        proc.kill()
        pytest.fail("server did not start listening in time")

    yield port
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()


def tone(ms, freq=440.0, amplitude=0.3):
    n = int(SR * ms / 1000)
    return b"".join(
        struct.pack("<h", int(amplitude * 32767 * math.sin(2 * math.pi * freq * i / SR)))
        for i in range(n)
    )


def rms(pcm):
    s = pcm_to_samples(pcm)
    return math.sqrt(sum(v * v for v in s) / len(s)) if s else 0.0


async def _echo_turn(port, fmt, codec, speech_ms=420):
    """Full turn: hello, a listen bracket with audio, then collect the reply."""
    async with websockets.connect(f"ws://127.0.0.1:{port}/ws", max_size=None) as ws:
        await ws.send(json.dumps({
            "type": "hello", "version": 1, "transport": "websocket",
            "device_id": "TE:ST:00:00:00:01",
            "audio_params": {"format": fmt, "sample_rate": SR, "frame_duration": FRAME_MS},
        }))
        server_hello = json.loads(await asyncio.wait_for(ws.recv(), timeout=15))
        assert server_hello["type"] == "hello"

        await ws.send(json.dumps({"type": "listen", "state": "start", "mode": "manual"}))
        for frame in codec.encode(tone(speech_ms)):
            await ws.send(frame)
            await asyncio.sleep(FRAME_MS / 1000.0)
        await ws.send(json.dumps({"type": "listen", "state": "detect_end"}))

        wire_frames, texts = [], []
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=5)
            except asyncio.TimeoutError:
                break
            if isinstance(msg, bytes):
                wire_frames.append(msg)
            else:
                d = json.loads(msg)
                texts.append(d)
                if d.get("type") == "tts" and d.get("state") in ("stop", "end"):
                    break
        return server_hello, wire_frames, texts


def test_pcm_client_gets_pcm_back(server):
    codec = PcmCodec(SR, FRAME_MS)
    hello, frames, _ = asyncio.run(_echo_turn(server, "pcm", codec))

    assert hello["audio_params"]["format"] == "pcm"
    assert frames, "server sent no audio"
    # PCM frames are exactly one frame wide; anything else means it re-encoded.
    assert all(len(f) == codec.bytes_per_frame for f in frames)


def test_opus_client_gets_opus_back(server):
    """The regression that mattered: an ESP32 must not be handed raw PCM."""
    codec = OpusCodec(SR, FRAME_MS)
    hello, frames, _ = asyncio.run(_echo_turn(server, "opus", codec))

    assert hello["audio_params"]["format"] == "opus"
    assert frames, "server sent no audio"

    # Opus frames are compressed, so they must be well under a raw frame...
    assert all(len(f) < codec.bytes_per_frame for f in frames)
    # ...and must actually decode, which raw PCM handed back would not.
    decoded = b"".join(codec.decode(f) for f in frames)
    assert len(decoded) == len(frames) * codec.bytes_per_frame
    assert rms(decoded) > 100, "round-tripped audio is silent"


def test_server_announces_the_format_it_will_use(server):
    """The device reads audio_params back; disagreeing here is silent garbage."""
    for fmt in ("pcm", "opus"):
        codec = PcmCodec(SR, FRAME_MS) if fmt == "pcm" else OpusCodec(SR, FRAME_MS)
        hello, _, _ = asyncio.run(_echo_turn(server, fmt, codec, speech_ms=120))
        assert hello["audio_params"]["format"] == fmt
        assert hello["audio_params"]["sample_rate"] == SR
        assert hello["audio_params"]["frame_duration"] == FRAME_MS


# Each of these used to escape serve.py's `except CodecError` as a different
# exception type and take down the connection handler.
HOSTILE_HELLOS = [
    pytest.param({"format": "mp3"}, id="unknown-format"),
    pytest.param({"format": 123}, id="format-is-a-number"),
    pytest.param({"format": "opus", "sample_rate": 44100}, id="opus-unsupported-rate"),
    pytest.param({"format": "opus", "sample_rate": "abc"}, id="sample_rate-not-a-number"),
    pytest.param({"format": "opus", "frame_duration": 0}, id="zero-frame-duration"),
]


@pytest.mark.parametrize("audio_params", HOSTILE_HELLOS)
def test_bad_hello_is_refused_cleanly(server, audio_params):
    async def go():
        async with websockets.connect(f"ws://127.0.0.1:{server}/ws") as ws:
            await ws.send(json.dumps({
                "type": "hello", "version": 1, "transport": "websocket",
                "device_id": "TE:ST:00:00:00:02", "audio_params": audio_params,
            }))
            with pytest.raises(websockets.exceptions.ConnectionClosed):
                await asyncio.wait_for(ws.recv(), timeout=10)

    asyncio.run(go())


def test_server_survives_a_hostile_client(server):
    """The point of the whole robustness exercise: one bad client must not take the
    server down for the next one."""
    async def go():
        for audio_params in [p.values[0] for p in HOSTILE_HELLOS]:
            try:
                async with websockets.connect(f"ws://127.0.0.1:{server}/ws") as ws:
                    await ws.send(json.dumps({
                        "type": "hello", "version": 1, "transport": "websocket",
                        "device_id": "TE:ST:00:00:00:03", "audio_params": audio_params,
                    }))
                    await asyncio.wait_for(ws.recv(), timeout=10)
            except websockets.exceptions.ConnectionClosed:
                pass

    asyncio.run(go())

    # …and a well-formed client still gets a full turn afterwards.
    codec = OpusCodec(SR, FRAME_MS)
    hello, frames, _ = asyncio.run(_echo_turn(server, "opus", codec, speech_ms=180))
    assert hello["audio_params"]["format"] == "opus"
    assert frames, "server stopped serving audio after hostile clients"
