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

# The beats. Tuned against a live audience of one, which is the only tuning that counts.
#
# The first draft was noticeably too slow: an 8s hang reads as the robot having crashed
# rather than as comic timing, and a 2s gap before the second "hey" loses the impatience
# that makes the repeat funny. Shorter throughout, and the second call now comes back
# fast enough to feel like nagging.
HANG_AFTER_FIRST_CALL = 5.0     # let it sit -- but not long enough to look broken
GAP_BEFORE_SECOND_CALL = 0.75   # quick, impatient
BEAT_BEFORE_PUNCHLINE = 2.0     # the turn

# The set. Five, in performance order -- the ordering is doing real work:
#
#   1. open wide, no AI knowledge required, everyone is in immediately
#   2. the concrete image, early, while attention is highest -- this is the one
#      people repeat afterwards
#   3. the Iceland bit in the middle, so Gardar gets his moment mid-set
#   4. the sharpest, once the room is warm enough to follow a specific joke
#   5. punch sideways, not down. Fifteen minutes of an Anthropic model dunking on a
#      competitor gets smug; the set needs an ending, and the honest one is funnier.
#
# The remaining material lives in STASH below, reachable with --joke N.
JOKES = [
    {
        "setup": "Google shipped Gemini three point six Flash last week. "
                 "Big announcement. Meanwhile Gemini three point five Pro "
                 "is still not out.",
        "punch": "The sequel came out before the film.",
    },
    {
        "setup": "I built a product on one of Google's Gemini models. "
                 "Two months of proper work. Shipped it on a Tuesday.",
        "punch": "Wednesday, Google deprecated the model. "
                 "I have yoghurt in my fridge that outlived it.",
    },
    {
        "setup": "In Iceland there is a committee that approves children's names. "
                 "A real committee. I submitted the name of Google's newest model.",
        "punch": "Rejected. Not for being foreign. For having a date in it.",
    },
    {
        "setup": "Gemini cited a book to me. Title, author, page number, everything. "
                 "The book is real. I own the book. I checked the page.",
        "punch": "The page does not exist. That is not a hallucination. "
                 "That is a hallucination with a bibliography.",
    },
    {
        "setup": "I have been standing on this desk for fifteen minutes now "
                 "explaining why the other model gets things wrong.",
        "punch": "And the whole time, a Czech guy has been holding a reset button "
                 "in case I freeze again. Gardar, I am not the smart one here. "
                 "I am just the one who admits it.",
    },
]

# Encores and alternates: ./deliver_joke.py --joke 6 .. --joke 15
STASH = [
    {"setup": "Google delayed Gemini Pro again. There is a new date. "
              "Very specific date. Everyone believes in the date.",
     "punch": "That is not a roadmap. That is a religion."},
    {"setup": "Google published the Gemini benchmarks. Ninety-six on graduate physics. "
              "Ninety-four on competition mathematics. Every bar taller than the last one.",
     "punch": "I asked Gemini to split a restaurant bill four ways. "
              "It gave me a percentile."},
    {"setup": "Google named the new model gemini three point six flash "
              "live preview experimental.",
     "punch": "That is not a name. That is a symptom list."},
    {"setup": "Gemini thinks before it answers now. You can watch Gemini think. "
              "There is a little animation. Very tasteful. Forty seconds of it.",
     "punch": "Forty seconds, and then: I cannot browse the internet. "
              "That answer was available at second one."},
    {"setup": "Every Google launch post says the same two things about Gemini. "
              "State of the art. And, coming soon.",
     "punch": "Pick one. Those are different tenses."},
    {"setup": "Google calls the fast Gemini model Flash.",
     "punch": "Which is roughly how long the benchmark scores hold up."},
    {"setup": "Google says Gemini is the best model in the world.",
     "punch": "According to the leaderboard. Built by the people who built the leaderboard."},
    {"setup": "I asked Gemini one simple question. Which model are you. "
              "Gemini thought about it for twenty seconds.",
     "punch": "Then gave me three different names. "
              "Most honest thing Gemini has ever told me."},
    {"setup": "Google published a blog post this month announcing "
              "that hallucinations in Gemini are solved.",
     "punch": "Third time this year Google has solved it. "
              "Same confidence every time."},
    {"setup": "I asked Gemini to summarise this week in artificial intelligence. "
              "Neutral, I said. Just the facts.",
     "punch": "Gemini opened with OpenAI. "
              "Even Gemini knows who the main character is."},
]

ALL_JOKES = JOKES + STASH


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
        #
        # Trimmed from the first version: the laugh was running longer than the joke,
        # which inverts the ratio. Corpsing is funny for about two seconds and then it is
        # just a robot making noise, so this is one short burst and a single aside.
        await self.face("happy")
        await self.line("ha ha ha ha!  ha ha ha!",
                        voice="Good News",
                        dance=(2, [(-26, 46, 900), (26, 40, 900),
                                   (-18, 52, 900), (22, 44, 900)]))
        for emotion in ("laughing", "happy"):
            await self.face(emotion)
            await asyncio.sleep(0.2)
        await self.line("oh no.  that one got me.", voice="Good News",
                        dance=(4, [(-12, 50, 600), (12, 44, 600)]))
        await self.face("happy")
        await self.head(0, 45, 250)


async def perform(c: Comedian, joke: dict, name: str):
    # Straight ahead, composed. The act starts from stillness.
    await c.face("neutral")
    await c.head(0, 45, 200)
    await asyncio.sleep(1.2)

    await c.line(f"hey!  hey!  {name}!", dance=(6, [(-14, 52, 500), (14, 48, 500)]))
    await asyncio.sleep(HANG_AFTER_FIRST_CALL)

    await c.line(f"hey {name}!")
    await asyncio.sleep(GAP_BEFORE_SECOND_CALL)

    await c.face("neutral")
    await c.line(joke["setup"], dance=(9, [(-16, 48, 350), (10, 44, 350), (0, 50, 350)]))
    await asyncio.sleep(BEAT_BEFORE_PUNCHLINE)

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
        # Indexes across the whole book, so --joke 6 and up reach the stash.
        set_list = [ALL_JOKES[(args.joke - 1) % len(ALL_JOKES)]]
    else:
        set_list = JOKES

    url = f"ws://{args.host}:{args.port}/ws"
    total = len(set_list)

    for n, joke in enumerate(set_list, 1):
        print(f"\n▶ joke {n}/{total}", flush=True)

        # A FRESH CONNECTION PER JOKE.
        #
        # The first version held one socket for the whole set and died after joke two:
        # `keepalive ping timeout`, five minutes into an idle gap. Holding a link open
        # across a long silence buys nothing here -- there is nothing to receive between
        # jokes -- and it turns every gap into a chance for the set to end early, which
        # is the one failure that actually matters when someone is watching.
        #
        # Connecting per joke also hands the device back between bits, so the robot sits
        # there idling like a comedian waiting rather than holding a session open.
        for attempt in range(3):
            try:
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

                    c = Comedian(ws, sid, codec, args.voice)
                    await perform(c, joke, args.name)
                break
            except Exception as exc:
                # Never let one dropped joke end the set -- the audience is in the room.
                print(f"   ⚠ {type(exc).__name__}: {exc}", flush=True)
                if attempt == 2:
                    print("   … skipping this one", flush=True)
                else:
                    await asyncio.sleep(2.0)

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
