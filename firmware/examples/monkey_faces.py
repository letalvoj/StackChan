#!/usr/bin/env python3
"""Monkey faces, for Vincent and Vilem.

    ./firmware/examples/monkey_faces.py                  # the whole routine
    ./firmware/examples/monkey_faces.py --bit greeting   # just say hello
    ./firmware/examples/monkey_faces.py --list-bits
    ./firmware/examples/monkey_faces.py --names Ada Nils # somebody else's kids

macOS only (uses `say`). Nothing else may hold the device socket while this runs --
it serves one client at a time, so stop gemini_live.py first.

This is deliver_joke.py's machinery pointed at a much younger audience, and the
differences are all deliberate:

* **No silences.** Comic timing needs a hang; small children read a hanging pause as
  the robot being broken and wander off. Every gap here is under a second.
* **Motion is the joke.** The words barely matter -- what lands is a head whipping
  side to side while making an "ooh ooh aah aah" noise. So the dances are faster,
  wider, and run on a shorter frame stride than anything in the comedy set.
* **Each kid gets addressed by name, individually, while being looked at.** Yaw is in
  the *audience's* frame: NEGATIVE turns toward the left of whoever is facing the
  robot. Being looked at by name is the entire trick with two kids in a room; if they
  are not standing where the script guesses, pass --swap.
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

# macOS `say` mangles Czech names read as English. Spelling is what goes to the
# speaker; the display name is what gets printed. Tune the left column by ear.
SPOKEN = {
    "Vilem": "Villem",
    "Vincent": "Vinsent",
}

# Emotions the firmware actually recognises: neutral, happy, laughing, angry, sad,
# crying, sleepy, doubtful. Anything else silently becomes neutral -- which is how
# "funny" in the comedy script quietly does nothing.
HAPPY, SILLY, NEUTRAL, SURPRISED = "happy", "laughing", "neutral", "doubtful"


def render(text: str, voice=None) -> bytes:
    """macOS TTS straight at the device's rate, so nothing needs resampling."""
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
            sys.exit("`say` not found -- macOS only.")
        except subprocess.CalledProcessError as exc:
            sys.exit(f"say failed: {exc.stderr.decode(errors='replace').strip()}")
        # Read INSIDE the context manager -- the temp dir is gone on exit.
        with wave.open(str(wav), "rb") as w:
            return w.readframes(w.getnframes())


class Monkey:
    def __init__(self, ws, sid, codec, voice):
        self.ws, self.sid, self.codec, self.voice = ws, sid, codec, voice
        self._id = 0

    async def _mcp(self, name, args):
        self._id += 1
        await self.ws.send(json.dumps({
            "session_id": self.sid, "type": "mcp",
            "payload": {"jsonrpc": "2.0", "id": self._id, "method": "tools/call",
                        "params": {"name": name, "arguments": args}}}))

    async def head(self, yaw=None, pitch=None, speed=600):
        await self._mcp("self.robot.set_head_angles", {
            "yaw": LEAVE_ALONE if yaw is None else int(yaw),
            "pitch": LEAVE_ALONE if pitch is None else int(pitch),
            "speed": int(speed)})

    async def sound(self, name):
        """One of: success, exclamation, popup, welcome, vibration."""
        await self._mcp("self.robot.play_sound", {"name": name})

    async def face(self, emotion):
        """Drives the avatar directly -- the same message an assistant would send."""
        await self.ws.send(json.dumps({
            "session_id": self.sid, "type": "llm", "emotion": emotion, "text": ""}))

    async def line(self, text, *, dance=None, voice=None, spoken=None):
        """Speak, paced at real time, with choreography interleaved between frames.

        Audio leaves on a monotonic deadline because the device decodes at exactly
        realtime; sending eagerly overruns its queue and the surplus is dropped. That
        also makes frame index a clock, so a pose issued every Nth frame lands on the
        syllable you meant.
        """
        pcm = render(spoken or text, voice or self.voice)
        frames = self.codec.encode(pcm)
        print(f'   ♪ "{text}"', flush=True)

        await self.ws.send(json.dumps({"session_id": self.sid, "type": "tts",
                                       "state": "start", "sample_rate": SR}))
        await self.ws.send(json.dumps({"session_id": self.sid, "type": "tts",
                                       "state": "sentence_start", "text": text}))

        loop = asyncio.get_running_loop()
        deadline = loop.time()
        for i, f in enumerate(frames):
            await self.ws.send(f)
            if dance and i % dance[0] == 0:
                pose = dance[1][(i // dance[0]) % len(dance[1])]
                await self.head(pose[0], pose[1], speed=pose[2] if len(pose) > 2 else 800)
            deadline += FRAME_MS / 1000.0
            delay = deadline - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)

        await self.ws.send(json.dumps({"session_id": self.sid,
                                       "type": "tts", "state": "stop"}))


# Choreography. Stride first (how many 60ms frames between poses), then the poses as
# (yaw, pitch, speed). Stride 2 is a pose every 120ms, which is about as fast as the
# servos will actually track -- anything shorter just queues up and looks smoother,
# not faster.
SWING = (2, [(-42, 60, 950), (42, 38, 950), (-34, 34, 950), (36, 62, 950)])
BOUNCE = (3, [(0, 68, 900), (0, 30, 900)])
WOBBLE = (4, [(-18, 52, 700), (18, 46, 700)])
SCRATCH = (2, [(-14, 70, 900), (-22, 62, 900), (-14, 70, 900), (-20, 66, 900)])


async def bit_greeting(m: Monkey, left: str, right: str):
    """The one that was actually asked for."""
    await m.face(HAPPY)
    await m.head(0, 50, 300)
    await asyncio.sleep(0.6)
    await m.sound("welcome")
    await asyncio.sleep(0.7)

    await m.line(f"Hey {right} and {left}!",
                 spoken=f"Hey {SPOKEN.get(right, right)} and {SPOKEN.get(left, left)}!",
                 dance=BOUNCE)
    await asyncio.sleep(0.3)
    await m.line("How are you boys!", dance=WOBBLE)
    await asyncio.sleep(0.4)


async def bit_monkey(m: Monkey, left: str, right: str):
    """Head whipping side to side. This is the bit they will ask for again."""
    await m.face(SILLY)
    await m.line("ooh ooh ooh!  aah aah aah!", voice="Good News", dance=SWING)
    await m.sound("popup")
    await m.line("ooh!  ooh!  aah!  eee eee eee!", voice="Good News", dance=SWING)
    await m.face(HAPPY)
    await m.head(0, 50, 400)
    await asyncio.sleep(0.3)


async def bit_names(m: Monkey, left: str, right: str):
    """Look at one kid, say their name, then the other. Being picked out is the joke."""
    for name, yaw in ((right, 48), (left, -48)):
        await m.face(HAPPY)
        await m.head(yaw, 56, 500)
        await asyncio.sleep(0.5)
        await m.line(f"{name}!  ooh ooh!",
                     spoken=f"{SPOKEN.get(name, name)}!  ooh ooh!",
                     voice="Good News", dance=(3, [(yaw - 12, 62, 900), (yaw + 12, 46, 900)]))
        await asyncio.sleep(0.25)
    await m.head(0, 50, 400)


async def bit_scratch(m: Monkey, left: str, right: str):
    await m.face(SURPRISED)
    await m.head(-16, 66, 500)
    await m.line("hmmm.  where did I put my banana.", dance=SCRATCH)
    await asyncio.sleep(0.4)
    await m.face(SILLY)
    await m.sound("exclamation")
    await m.line("oh no.  I am a robot.  I do not have a banana!",
                 dance=(3, [(-30, 40, 900), (30, 62, 900)]))
    await asyncio.sleep(0.3)


async def bit_finale(m: Monkey, left: str, right: str):
    await m.face(SILLY)
    await m.line("eee eee eee!  ooh ooh aah!", voice="Good News", dance=SWING)
    await m.sound("success")
    await asyncio.sleep(0.5)
    await m.face(HAPPY)
    await m.line(f"okay.  your turn, {right} and {left}!",
                 spoken=f"okay.  your turn, {SPOKEN.get(right, right)} "
                        f"and {SPOKEN.get(left, left)}!",
                 dance=BOUNCE)
    await m.head(0, 50, 300)


BITS = {
    "greeting": bit_greeting,
    "monkey": bit_monkey,
    "names": bit_names,
    "scratch": bit_scratch,
    "finale": bit_finale,
}
ORDER = ["greeting", "monkey", "names", "scratch", "finale"]


async def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--names", nargs=2, default=["Vilem", "Vincent"],
                    metavar=("LEFT", "RIGHT"),
                    help="the two kids, as seen from the robot's audience "
                         "(default: Vilem Vincent)")
    ap.add_argument("--swap", action="store_true",
                    help="they swapped seats -- mirror who gets looked at")
    ap.add_argument("--bit", action="append", choices=list(BITS),
                    help="perform only this bit (repeatable)")
    ap.add_argument("--list-bits", action="store_true")
    ap.add_argument("--voice", default=None, help="macOS voice for the speaking lines")
    ap.add_argument("--host", default="192.168.7.1")
    ap.add_argument("--port", type=int, default=8081)
    args = ap.parse_args()

    if args.list_bits:
        for name in ORDER:
            print(f"  {name:9s} {BITS[name].__doc__ or ''}".rstrip())
        return

    left, right = args.names
    if args.swap:
        left, right = right, left
    running = args.bit or ORDER

    url = f"ws://{args.host}:{args.port}/ws"
    async with websockets.connect(url, open_timeout=20, max_size=None,
                                  ping_interval=20, ping_timeout=60) as ws:
        sid = str(uuid.uuid4())
        await ws.send(json.dumps({
            "type": "hello", "transport": "websocket", "session_id": sid,
            "audio_params": {"format": "opus", "sample_rate": SR,
                             "frame_duration": FRAME_MS}}))
        await asyncio.sleep(0.6)
        codec = codec_from_hello({"audio_params": {
            "format": "opus", "sample_rate": SR, "frame_duration": FRAME_MS}})

        m = Monkey(ws, sid, codec, args.voice)
        for name in running:
            print(f"\n▶ {name}", flush=True)
            await BITS[name](m, left, right)

        # Hand the device back looking like itself, not mid-gesture.
        await m.face(NEUTRAL)
        await m.head(0, 45, 250)
        await asyncio.sleep(0.8)

    print("\n🐒 ooh ooh aah aah", flush=True)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n(swings away)")
    except OSError as exc:
        sys.exit(f"could not reach the device at {exc}\nsee firmware/DEBUGGING.md")
