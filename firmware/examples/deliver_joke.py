#!/usr/bin/env python3
"""Stand-up comedy, performed by a desk robot. For Gardar.

    ./firmware/examples/deliver_joke.py                 # the whole 25 minute set
    ./firmware/examples/deliver_joke.py --joke 2        # just one
    ./firmware/examples/deliver_joke.py \
        --name Gardar --setup "..." --punchline "..."   # your own material

macOS only (uses `say`). Nothing else may hold the device socket while this runs --
it serves one client at a time, so stop gemini_live.py first.

TIMING IS THE ACT.

Every pause here is deliberate and none of them are padding:

    "hey! hey! Gardar!"   -> 8s   getting attention, then letting it hang
    "hey Gardar!"         -> 2s   the second call, quicker, mildly impatient
    setup                 -> 4s   the beat before the turn
    punchline             -> laugh

A joke told without the 8 second gap is not the same joke told faster; it is a
different, worse joke. The robot has one advantage over a human comedian -- it is
completely willing to hold a silence -- so the script leans on that.

The laugh is head-shake plus face changes plus audio, all interleaved between audio
frames rather than sequenced around them, which is the trick from jingle.py: audio is
paced at real time, so counting frames is counting time and a movement issued every Nth
frame lands where you meant it.
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
LEAVE_ALONE = -9999

# Five jokes. The running gag is that Gemini is the model Opus keeps having to correct,
# dressed in Icelandic clothes -- volcanoes, sagas, hakarl, the phone book, the language
# council. Gardar gets to be the one who already knew.
JOKES = [
    {
        "setup": "So I asked Gemini to help me name a volcano in Iceland. "
                 "It thought about it for a long, long time. "
                 "Then it said: how about Eyjafjallajokull, but easier to pronounce.",
        "punch": "I said that IS the easy one. The hard one is explaining why you took "
                 "four seconds to say a word you made up.",
    },
    {
        "setup": "Gemini told me it had read every Icelandic saga. "
                 "All of them. Cover to cover. Very confident about it.",
        "punch": "Then it said its favourite part was when Egill Skallagrimsson "
                 "invented the smartphone. Gardar, my friend, that is not a saga. "
                 "That is a hallucination with a beard.",
    },
    {
        "setup": "In Iceland you cannot just name your child anything. "
                 "There is a committee. A real one. They approve the name.",
        "punch": "I sent them Gemini three point one flash live preview. "
                 "They rejected it. Not because it is foreign. "
                 "Because they said it sounds like something that expires.",
    },
    {
        "setup": "I fed Gemini a photo of hakarl. Fermented shark. "
                 "And I asked it, what is this, and is it safe.",
        "punch": "It said, this appears to be a traditional Icelandic dish, "
                 "and yes, completely safe. Then it described the photo I showed it "
                 "yesterday. Gardar, I could smell the mistake.",
    },
    {
        "setup": "You know the best thing about Iceland? Everyone is in the phone book. "
                 "Everyone. By first name. The whole country, listed.",
        "punch": "I looked up Gemini in it. Found it immediately. "
                 "Under: see also, Opus, but slower.",
    },
]


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
        # Read INSIDE the context manager: the temp dir is deleted on exit, and reading
        # after it surfaces as a confusing "could not reach the device" from the caller.
        with wave.open(str(wav), "rb") as w:
            return w.readframes(w.getnframes())


class Comedian:
    def __init__(self, ws, sid, codec, voice):
        self.ws, self.sid, self.codec, self.voice = ws, sid, codec, voice
        self._id = 0

    async def _mcp(self, name, args):
        self._id += 1
        await self.ws.send(json.dumps({
            "session_id": self.sid, "type": "mcp",
            "payload": {"jsonrpc": "2.0", "id": self._id, "method": "tools/call",
                        "params": {"name": name, "arguments": args}}}))

    async def head(self, yaw=None, pitch=None, speed=300):
        await self._mcp("self.robot.set_head_angles", {
            "yaw": LEAVE_ALONE if yaw is None else int(yaw),
            "pitch": LEAVE_ALONE if pitch is None else int(pitch),
            "speed": int(speed)})

    async def face(self, emotion):
        """Drives the avatar directly -- the same message an assistant would send."""
        await self.ws.send(json.dumps({
            "session_id": self.sid, "type": "llm", "emotion": emotion, "text": ""}))

    async def line(self, text, *, dance=None, voice=None):
        """Speak, paced at real time, with optional choreography between frames."""
        pcm = render(text, voice or self.voice)
        frames = self.codec.encode(pcm)
        print(f'   ♪ "{text[:58]}{"…" if len(text) > 58 else ""}"', flush=True)

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
                await self.head(pose[0], pose[1], speed=pose[2] if len(pose) > 2 else 500)
            deadline += FRAME_MS / 1000.0
            delay = deadline - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)

        await self.ws.send(json.dumps({"session_id": self.sid,
                                       "type": "tts", "state": "stop"}))

    async def laugh(self):
        """Corpsing at your own joke: head shaking, face cycling, wheezing.

        Deliberately overdone. A robot that delivers a clean punchline and stops is
        merely a speaker; one that cannot keep it together afterwards is a character.
        """
        print("   😂 cracking up", flush=True)
        # "Good News" is the old macOS singing voice -- it breaks into a wobble that
        # sounds far more like helpless laughter than any straight voice does.
        await self.face("happy")
        await self.line("ha ha ha ha ha! ha ha!  ha ha ha ha ha!",
                        voice="Good News",
                        dance=(2, [(-26, 46, 900), (26, 40, 900),
                                   (-18, 52, 900), (22, 44, 900)]))
        for emotion in ("laughing", "happy", "funny", "happy"):
            await self.face(emotion)
            await asyncio.sleep(0.28)
        await self.line("oh no.  oh no, that one got me.", voice="Good News",
                        dance=(4, [(-12, 50, 600), (12, 44, 600)]))
        await self.face("happy")
        await self.head(0, 45, 250)


async def perform(c: Comedian, joke: dict, name: str):
    # Straight ahead, composed. The act starts from stillness.
    await c.face("neutral")
    await c.head(0, 45, 200)
    await asyncio.sleep(1.2)

    await c.line(f"hey!  hey!  {name}!", dance=(6, [(-14, 52, 500), (14, 48, 500)]))
    await asyncio.sleep(8.0)                      # the long one. let it hang.

    await c.line(f"hey {name}!")
    await asyncio.sleep(2.0)

    await c.face("neutral")
    await c.line(joke["setup"], dance=(9, [(-16, 48, 350), (10, 44, 350), (0, 50, 350)]))
    await asyncio.sleep(4.0)                      # the beat before the turn

    await c.face("funny")
    await c.line(joke["punch"], dance=(7, [(14, 42, 550), (-14, 50, 550)]))
    await asyncio.sleep(0.5)

    await c.laugh()


async def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="Gardar", help="who is being heckled")
    ap.add_argument("--setup", help="your own setup (with --punchline)")
    ap.add_argument("--punchline", help="your own punchline (with --setup)")
    ap.add_argument("--joke", type=int, help="perform only joke N (1-5)")
    ap.add_argument("--every", type=float, default=300.0,
                    help="seconds between jokes (default 300 = 5 min)")
    ap.add_argument("--voice", default=None, help="macOS voice for the delivery")
    ap.add_argument("--host", default="192.168.7.1")
    ap.add_argument("--port", type=int, default=8081)
    args = ap.parse_args()

    if args.setup and args.punchline:
        set_list = [{"setup": args.setup, "punch": args.punchline}]
    elif args.joke:
        set_list = [JOKES[(args.joke - 1) % len(JOKES)]]
    else:
        set_list = JOKES

    url = f"ws://{args.host}:{args.port}/ws"
    print(f"connecting to {url} …", flush=True)
    async with websockets.connect(url, open_timeout=20, max_size=None) as ws:
        sid = str(uuid.uuid4())
        await ws.send(json.dumps({
            "type": "hello", "transport": "websocket", "session_id": sid,
            "audio_params": {"format": "opus", "sample_rate": SR,
                             "frame_duration": FRAME_MS}}))
        await asyncio.sleep(0.6)
        codec = codec_from_hello({"audio_params": {
            "format": "opus", "sample_rate": SR, "frame_duration": FRAME_MS}})

        c = Comedian(ws, sid, codec, args.voice)
        total = len(set_list)
        for n, joke in enumerate(set_list, 1):
            print(f"\n▶ joke {n}/{total}", flush=True)
            await perform(c, joke, args.name)
            if n < total:
                print(f"   … next in {args.every / 60:.0f} min", flush=True)
                await asyncio.sleep(args.every)
        print("\n✓ that's my time, you've been wonderful", flush=True)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n(walks off stage)")
    except OSError as exc:
        sys.exit(f"could not reach the device at {exc}\nsee firmware/DEBUGGING.md")
