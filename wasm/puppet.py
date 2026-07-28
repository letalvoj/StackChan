#!/usr/bin/env python3
"""Drive a live StackChan: move its head, and make it talk using macOS TTS.

Connects to a device running the USB-NCM firmware (which listens), does the
handshake, then performs a scripted routine: look around, call people over, and
speak generated audio through the device's own speaker.

    ./.venv/bin/python puppet.py --connect 192.168.7.1
    ./.venv/bin/python puppet.py --connect 192.168.7.1 --say "custom line"
    ./.venv/bin/python puppet.py --connect 127.0.0.1 --port 8099   # against the mock

Speech uses the built-in `say` command, so it needs no network and no API key --
the voice is whatever macOS ships. Audio is resampled to the device's negotiated
rate and encoded with the format it advertised, so it arrives ready to play.
"""

import argparse
import asyncio
import json
import subprocess
import sys
import tempfile
import time
import uuid
import wave
from pathlib import Path

import websockets

sys.path.insert(0, str(Path(__file__).resolve().parent))
from audio_codec import codec_from_hello  # noqa: E402

DIM, BOLD, GREEN, RESET = "\033[2m", "\033[1m", "\033[32m", "\033[0m"


class Device:
    def __init__(self, ws, session_id, codec, device_id):
        self.ws, self.session_id, self.codec = ws, session_id, codec
        self.device_id = device_id
        self._seq = 0
        self._replies = {}

    async def _send(self, obj):
        await self.ws.send(json.dumps(obj))

    async def call(self, name, arguments=None, timeout=20.0):
        """Invoke an MCP tool on the device and wait for its reply."""
        self._seq += 1
        rpc_id = self._seq
        await self._send({"session_id": self.session_id, "type": "mcp", "payload": {
            "jsonrpc": "2.0", "id": rpc_id, "method": "tools/call",
            "params": {"name": name, "arguments": arguments or {}}}})
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if rpc_id in self._replies:
                return self._replies.pop(rpc_id)
            await asyncio.sleep(0.03)
        raise TimeoutError(f"no reply to {name}")

    async def head(self, yaw=None, pitch=None, speed=200, settle=0.0):
        """Yaw -128..128 (negative is the robot's left), pitch 0..90 (90 is up).

        -9999 is the firmware's sentinel for "leave this axis alone".
        """
        args = {"yaw": -9999 if yaw is None else int(yaw),
                "pitch": -9999 if pitch is None else int(pitch),
                "speed": int(speed)}
        await self.call("self.robot.set_head_angles", args)
        if settle:
            await asyncio.sleep(settle)

    async def say(self, text, voice=None, rate=None):
        """Speak through the device's speaker using macOS TTS.

        `say` writes a WAV at the device's own sample rate, so nothing has to be
        resampled afterwards; it is then framed and encoded with the negotiated
        codec (Opus on real hardware).
        """
        sr = self.codec.sample_rate
        with tempfile.TemporaryDirectory() as tmp:
            wav = Path(tmp) / "line.wav"
            cmd = ["say", "-o", str(wav), "--file-format=WAVE",
                   f"--data-format=LEI16@{sr}", "--channels=1"]
            if voice:
                cmd += ["-v", voice]
            if rate:
                cmd += ["-r", str(rate)]
            cmd.append(text)
            subprocess.run(cmd, check=True, capture_output=True)

            with wave.open(str(wav), "rb") as w:
                assert w.getnchannels() == 1 and w.getsampwidth() == 2
                pcm = w.readframes(w.getnframes())

        frames = self.codec.encode(pcm)
        print(f"{DIM}   speaking {len(pcm)/2/sr:.1f}s as {len(frames)} "
              f"{self.codec.name} frames: “{text}”{RESET}")

        await self._send({"session_id": self.session_id, "type": "tts",
                          "state": "start", "sample_rate": sr})
        await self._send({"session_id": self.session_id, "type": "tts",
                          "state": "sentence_start", "text": text})
        # Paced at real time: the device plays as it receives, and blasting the whole
        # utterance at once would overrun its jitter buffer.
        for frame in frames:
            await self.ws.send(frame)
            await asyncio.sleep(self.codec.frame_ms / 1000.0)
        await self._send({"session_id": self.session_id, "type": "tts", "state": "stop"})

    async def reader(self):
        try:
            async for msg in self.ws:
                if isinstance(msg, bytes):
                    continue
                d = json.loads(msg)
                if d.get("type") == "mcp":
                    p = d.get("payload", {})
                    if "id" in p:
                        self._replies[p["id"]] = p
        except websockets.exceptions.ConnectionClosed:
            pass


async def routine(dev, line, voice):
    print(f"{BOLD}▶ waking up{RESET}")
    await dev.head(yaw=0, pitch=45, speed=300, settle=0.6)

    print(f"{BOLD}▶ looking around{RESET}")
    for yaw, pitch in ((-60, 55), (60, 55), (0, 70), (0, 40)):
        await dev.head(yaw=yaw, pitch=pitch, speed=250, settle=0.55)

    print(f"{BOLD}▶ calling them over{RESET}")
    # Turn toward each name as it is spoken, so the gesture matches the words.
    await dev.head(yaw=-45, pitch=60, speed=400, settle=0.25)
    await dev.say("Vojta!", voice=voice)
    await dev.head(yaw=45, pitch=60, speed=400, settle=0.25)
    await dev.say("Gardar!", voice=voice)
    await dev.head(yaw=0, pitch=65, speed=350, settle=0.2)
    await dev.say("Come here guys!", voice=voice)

    print(f"{BOLD}▶ beckoning{RESET}")
    for _ in range(2):
        await dev.head(pitch=35, speed=600, settle=0.28)
        await dev.head(pitch=70, speed=600, settle=0.28)

    if line:
        await dev.say(line, voice=voice)

    print(f"{BOLD}▶ resting{RESET}")
    await dev.head(yaw=0, pitch=45, speed=200, settle=0.5)
    print(f"{GREEN}done{RESET}")


async def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--connect", default="192.168.7.1", metavar="HOST")
    ap.add_argument("--port", type=int, default=8081)
    ap.add_argument("--say", dest="line", default="", help="extra line to speak at the end")
    ap.add_argument("--voice", default=None, help="macOS voice name (see: say -v '?')")
    args = ap.parse_args()

    url = f"ws://{args.connect}:{args.port}/ws"
    print(f"{DIM}connecting to {url}…{RESET}")
    async with websockets.connect(url, max_size=None, open_timeout=30) as ws:
        hello = json.loads(await asyncio.wait_for(ws.recv(), timeout=30))
        if hello.get("type") != "hello":
            sys.exit(f"expected hello, got {hello.get('type')!r}")

        session_id = str(uuid.uuid4())
        codec = codec_from_hello(hello)
        await ws.send(json.dumps({
            "type": "hello", "transport": "websocket", "session_id": session_id,
            "audio_params": {"format": codec.name, "sample_rate": codec.sample_rate,
                             "frame_duration": codec.frame_ms}}))

        dev = Device(ws, session_id, codec, hello.get("device_id", "?"))
        print(f"{BOLD}connected{RESET} to {dev.device_id}  "
              f"audio {codec.name}@{codec.sample_rate}Hz\n")

        pump = asyncio.create_task(dev.reader())
        try:
            await routine(dev, args.line, args.voice)
        finally:
            pump.cancel()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except (OSError, asyncio.TimeoutError) as exc:
        sys.exit(f"could not reach the device: {exc}\nis it plugged in? see TESTING.md §4")
    except KeyboardInterrupt:
        pass
