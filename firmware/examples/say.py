#!/usr/bin/env python3
"""Say something through StackChan's own speaker, in your Mac's voice.

    ./firmware/examples/say.py --text "Good night, Vojta!"
    ./firmware/examples/say.py --text "Ta-daa" --voice "Good News" --nod
    ./firmware/examples/say.py --list-voices

macOS only (uses `say`). The device must be reachable -- see firmware/DEBUGGING.md.

Two things here are load-bearing rather than decorative:

* **Audio is rendered straight at the device's sample rate.** `say` can write
  LEI16@16000 directly, so nothing needs resampling on the way out.

* **Frames leave on a real-time deadline, one per frame_ms.** The device decodes at
  exactly realtime; sending eagerly overruns its queue and the surplus is dropped,
  which sounds like stuttering. Timing against a monotonic deadline rather than
  sleeping a fixed amount each pass keeps encode cost from accumulating into drift.

Connecting evicts whatever client currently holds the device -- it serves one at a
time, by design (AGENT.md 6). Your gateway will reconnect on its own.
"""

import argparse
import asyncio
import json
import subprocess
import sys
import tempfile
import uuid
import wave
from pathlib import Path

import websockets

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "wasm"))
from audio_codec import codec_from_hello  # noqa: E402

SR, FRAME_MS = 16000, 60
LEAVE_ALONE = -9999          # firmware sentinel for "do not move this axis"


def render(text: str, voice: str | None) -> bytes:
    """macOS TTS as PCM16 at the device's rate."""
    with tempfile.TemporaryDirectory() as tmp:
        wav = Path(tmp) / "line.wav"
        cmd = ["say", "-o", str(wav), "--file-format=WAVE",
               f"--data-format=LEI16@{SR}", "--channels=1"]
        if voice:
            cmd += ["-v", voice]
        cmd.append(text)
        try:
            subprocess.run(cmd, check=True, capture_output=True)
        except FileNotFoundError:
            sys.exit("`say` not found -- this script is macOS only.")
        except subprocess.CalledProcessError as exc:
            sys.exit(f"say failed: {exc.stderr.decode(errors='replace').strip()}")
        with wave.open(str(wav), "rb") as w:
            return w.readframes(w.getnframes())


async def head(ws, sid, req_id, yaw=None, pitch=None, speed=300):
    await ws.send(json.dumps({"session_id": sid, "type": "mcp", "payload": {
        "jsonrpc": "2.0", "id": req_id, "method": "tools/call",
        "params": {"name": "self.robot.set_head_angles", "arguments": {
            "yaw": LEAVE_ALONE if yaw is None else int(yaw),
            "pitch": LEAVE_ALONE if pitch is None else int(pitch),
            "speed": int(speed)}}}}))


async def run(args):
    pcm = render(args.text, args.voice)
    url = f"ws://{args.host}:{args.port}/ws"
    sid = str(uuid.uuid4())

    async with websockets.connect(url, open_timeout=15, max_size=None) as ws:
        await ws.send(json.dumps({
            "type": "hello", "transport": "websocket", "session_id": sid,
            "audio_params": {"format": "opus", "sample_rate": SR,
                             "frame_duration": FRAME_MS}}))
        await asyncio.sleep(0.5)

        codec = codec_from_hello({"audio_params": {
            "format": "opus", "sample_rate": SR, "frame_duration": FRAME_MS}})
        frames = codec.encode(pcm)
        print(f'"{args.text}" — {len(frames)} frames, {len(pcm)/2/SR:.1f}s', flush=True)

        if args.nod:
            await head(ws, sid, 1, 0, 55, 250)
            await asyncio.sleep(0.7)

        await ws.send(json.dumps({"session_id": sid, "type": "tts",
                                  "state": "start", "sample_rate": SR}))
        await ws.send(json.dumps({"session_id": sid, "type": "tts",
                                  "state": "sentence_start", "text": args.text}))

        loop = asyncio.get_running_loop()
        deadline = loop.time()
        # Counting frames is counting musical time, so a gesture keyed to a frame index
        # lands where you meant it to in the sentence.
        nod_at = max(1, int(len(frames) * 0.55)) if args.nod else -1
        for i, frame in enumerate(frames):
            await ws.send(frame)
            if i == nod_at:
                await head(ws, sid, 2, 0, 30, 350)
            deadline += FRAME_MS / 1000.0
            delay = deadline - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)

        await ws.send(json.dumps({"session_id": sid, "type": "tts", "state": "stop"}))
        await asyncio.sleep(0.5)

        if args.nod:
            await head(ws, sid, 3, 0, 45, 200)
            await asyncio.sleep(0.7)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--text", "-t", help="what to say")
    ap.add_argument("--voice", "-v", default=None,
                    help='macOS voice, e.g. "Samantha" or "Good News" (default: system voice)')
    ap.add_argument("--nod", action="store_true",
                    help="look up, nod mid-sentence, settle")
    ap.add_argument("--host", default="192.168.7.1")
    ap.add_argument("--port", type=int, default=8081)
    ap.add_argument("--list-voices", action="store_true", help="list macOS voices and exit")
    args = ap.parse_args()

    if args.list_voices:
        subprocess.run(["say", "-v", "?"])
        return
    if not args.text:
        ap.error("--text is required (or use --list-voices)")

    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
    except OSError as exc:
        sys.exit(f"could not reach the device at {args.host}:{args.port}: {exc}\n"
                 f"see firmware/DEBUGGING.md")


if __name__ == "__main__":
    main()
