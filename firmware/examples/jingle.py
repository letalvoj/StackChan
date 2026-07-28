#!/usr/bin/env python3
"""Make a live StackChan sing and dance over the USB-NCM link.

The first thing that ever ran end to end on real hardware. Doubles as a worked
example of driving the device from a host: MCP tool calls for the servos, and
`tts` frames for the speaker.

    ./wasm/.venv/bin/python firmware/examples/jingle.py
    ./wasm/.venv/bin/python firmware/examples/jingle.py --connect 192.168.7.1

Two things worth stealing from this:

* **Choreography is interleaved between audio frames, not sequenced around
  them.** Audio is paced at real time (one 60 ms frame every 60 ms), so counting
  frames *is* counting musical time -- issuing a head move every Nth frame lands
  the movement on the beat. Sending the moves before or after the audio makes the
  robot dance to silence.

* **Nothing waits for a reply.** Commands and audio both travel host->device, and
  at the time of writing the device->host direction still has a bug, so every
  send here is fire-and-forget. That is also why it works: the demo never blocks
  on an acknowledgement it will not get.

macOS only, for `say` -- including the old singing voices (Good News, Cellos).
"""

import argparse
import asyncio
import json
import math
import struct
import subprocess
import sys
import tempfile
import uuid
import wave
from pathlib import Path

import websockets

# audio_codec lives with the gateway; reuse it so the wire format matches whatever
# the device negotiated rather than hardcoding one here.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "wasm"))
from audio_codec import codec_from_hello  # noqa: E402

SR = 16000
FRAME_MS = 60

NOTES = {"C5": 523.25, "D5": 587.33, "E5": 659.25, "F5": 698.46,
         "G5": 783.99, "A5": 880.00, "B5": 987.77, "C6": 1046.50}


def note(freq, ms, amp=0.32, sample_rate=SR):
    """One sine note as PCM16.

    The envelope is not decoration: a raw sine that starts and stops at nonzero
    amplitude clicks audibly on a small speaker. The second harmonic gives the
    note some body, since a tiny driver reproduces almost no fundamental.
    """
    n = int(sample_rate * ms / 1000)
    attack, decay = int(n * 0.08) + 1, int(n * 0.30) + 1
    out = bytearray()
    for i in range(n):
        env = min(1.0, i / attack, (n - i) / decay)
        s = (math.sin(2 * math.pi * freq * i / sample_rate)
             + 0.25 * math.sin(4 * math.pi * freq * i / sample_rate))
        out += struct.pack("<h", int(max(-1.0, min(1.0, amp * env * s)) * 32767))
    return bytes(out)


def rest(ms, sample_rate=SR):
    return b"\x00" * (int(sample_rate * ms / 1000) * 2)


def speak(text, voice=None, sample_rate=SR):
    """macOS TTS straight to the device's sample rate, so nothing needs resampling."""
    with tempfile.TemporaryDirectory() as tmp:
        wav = Path(tmp) / "line.wav"
        cmd = ["say", "-o", str(wav), "--file-format=WAVE",
               f"--data-format=LEI16@{sample_rate}", "--channels=1"]
        if voice:
            cmd += ["-v", voice]
        cmd.append(text)
        subprocess.run(cmd, check=True, capture_output=True)
        with wave.open(str(wav), "rb") as w:
            return w.readframes(w.getnframes())


def fanfare():
    """Rising C-major arpeggio, a turn, and a resolve."""
    return b"".join([
        note(NOTES["C5"], 140), note(NOTES["E5"], 140),
        note(NOTES["G5"], 140), note(NOTES["C6"], 280), rest(60),
        note(NOTES["B5"], 120), note(NOTES["C6"], 120), note(NOTES["D5"], 120), rest(40),
        note(NOTES["G5"], 160), note(NOTES["C6"], 420),
    ])


class Stage:
    def __init__(self, ws, session_id, codec):
        self.ws, self.session_id, self.codec = ws, session_id, codec
        self._seq = 0

    async def head(self, yaw=None, pitch=None, speed=400, settle=0.0):
        """yaw -128..128 (negative is the robot's left), pitch 0..90 (90 is up).

        -9999 is the firmware's sentinel for "leave this axis alone".
        """
        self._seq += 1
        await self.ws.send(json.dumps({
            "session_id": self.session_id, "type": "mcp", "payload": {
                "jsonrpc": "2.0", "id": self._seq, "method": "tools/call",
                "params": {"name": "self.robot.set_head_angles", "arguments": {
                    "yaw": -9999 if yaw is None else int(yaw),
                    "pitch": -9999 if pitch is None else int(pitch),
                    "speed": int(speed)}}}}))
        if settle:
            await asyncio.sleep(settle)

    async def play(self, pcm, label, dance=None):
        """Stream audio, optionally moving the head every `every` frames.

        `dance` is (every, [(yaw, pitch), ...]) and steps through the poses in
        time with playback -- see the module docstring.
        """
        frames = self.codec.encode(pcm)
        print(f"   ♪ {label}  ({len(frames)} {self.codec.name} frames, "
              f"{len(pcm) / 2 / self.codec.sample_rate:.1f}s)")
        await self.ws.send(json.dumps({"session_id": self.session_id, "type": "tts",
                                       "state": "start", "sample_rate": self.codec.sample_rate}))
        await self.ws.send(json.dumps({"session_id": self.session_id, "type": "tts",
                                       "state": "sentence_start", "text": label}))
        for i, frame in enumerate(frames):
            await self.ws.send(frame)
            if dance and i % dance[0] == 0:
                pose = dance[1][(i // dance[0]) % len(dance[1])]
                await self.head(pose[0], pose[1], speed=700)
            await asyncio.sleep(self.codec.frame_ms / 1000.0)
        await self.ws.send(json.dumps({"session_id": self.session_id,
                                       "type": "tts", "state": "stop"}))
        await asyncio.sleep(0.25)


async def show(stage, name):
    await stage.head(0, 50, 300, 0.6)

    print("▶ fanfare")
    await stage.play(fanfare(), "ta-da!", dance=(3, [(-35, 65), (0, 45), (35, 65), (0, 70)]))

    print("▶ singing")
    await stage.play(speak("Stack chan is a live and well", "Good News"),
                     "singing", dance=(5, [(-25, 60), (25, 60)]))
    await stage.play(speak(f"Made by {name}", "Cellos"),
                     "cellos", dance=(6, [(0, 75), (0, 40)]))

    await stage.head(0, 45, 200, 0.5)
    print("✓ encore complete")


async def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--connect", default="192.168.7.1", metavar="HOST")
    ap.add_argument("--port", type=int, default=8081)
    ap.add_argument("--name", default="Vojta", help="who to credit in the second verse")
    args = ap.parse_args()

    url = f"ws://{args.connect}:{args.port}/ws"
    print(f"connecting to {url} …")
    async with websockets.connect(url, open_timeout=20, max_size=None) as ws:
        session_id = str(uuid.uuid4())
        # Speak first rather than waiting to be greeted: the device parses a hello from
        # whoever sends one, and its own greeting is currently unreliable.
        await ws.send(json.dumps({
            "type": "hello", "transport": "websocket", "session_id": session_id,
            "audio_params": {"format": "opus", "sample_rate": SR, "frame_duration": FRAME_MS}}))
        await asyncio.sleep(0.3)

        codec = codec_from_hello({"audio_params": {"format": "opus",
                                                   "sample_rate": SR,
                                                   "frame_duration": FRAME_MS}})
        await show(Stage(ws, session_id, codec), args.name)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except (OSError, asyncio.TimeoutError) as exc:
        sys.exit(f"could not reach the device: {exc}\nsee TESTING.md §4")
    except KeyboardInterrupt:
        pass
