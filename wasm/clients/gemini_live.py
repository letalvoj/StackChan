#!/usr/bin/env python3
"""Gemini Live <-> StackChan: tap the face, talk to the robot.

Standalone by design. It connects straight to the device, holds the one socket the
firmware allows, and speaks to Gemini itself -- no broker, no relay, no gateway in
between (AGENT.md 6). Run one client at a time; connecting a second one hangs this
one up, deliberately and visibly.

    cp .env .env.local && $EDITOR .env.local      # put your key in
    ./wasm/.venv/bin/python wasm/clients/gemini_live.py

Then tap StackChan's face. That is the whole trigger: the tap moves the device into
listening, which is what opens its audio channel and starts mic frames flowing. Until
then no audio leaves the device at all -- see the privacy note under SESSION below.

Flow, once a session is live:

    device mic --Opus--> decode -> 16k PCM --> Gemini Live
    Gemini Live --24k PCM--> resample -> encode Opus --> device speaker
    Gemini function call --> MCP tools/call on the device --> result back to Gemini

SESSION / PRIVACY
    A session exists only between a tap and the end of the conversation. The device
    sends nothing when idle, so the microphone boundary is the tap, not a VAD
    threshold -- deterministic and user-controlled rather than probabilistic. See
    the VAD note in ARCHITECTURE.md 5.4.
"""

import argparse
import asyncio
import base64
import binascii
import json
import math
import os
import random
import struct
import sys
import time
import uuid
from pathlib import Path

import websockets
from google import genai
from google.genai import types

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "wasm"))
from audio_codec import codec_from_hello  # noqa: E402

GEMINI_RECEIVE_RATE = 24000     # Live API always returns 24 kHz PCM16 mono
GEMINI_SEND_RATE = 16000        # and expects 16 kHz in

# How long a resumption handle stays usable after the link drops.
#
# Resumption exists to survive a transport hiccup -- a yanked cable, a dead tunnel, a
# device reboot -- not to make the robot remember an arbitrary amount of time later.
# Without a bound, a handle taken this morning would still be replayed this evening and
# the robot would carry on a conversation from hours ago as though nothing happened,
# in front of whoever happens to be standing there now. Past this window we drop the
# handle and the next tap starts a genuinely fresh conversation.
RESUME_WINDOW_S = 90

# Frames of head start before a turn begins playing. 4 x 60 ms = 240 ms of cushion --
# enough to ride out ordinary network jitter, small enough not to feel like lag.
PREROLL_FRAMES = 4

# Camera streaming. 1 fps is the ceiling the Live API documents for video, and it is also
# about as fast as the device can capture and JPEG-encode without disturbing audio. The
# poll interval is what the gate is checked at; a frame only actually leaves when the
# device's VAD says someone is speaking.
VIDEO_POLL_S = 1.0

# Playback level. Gemini's TTS is mastered close to full scale, and the CoreS3 drives a
# 1 W speaker in a plastic head -- so the last few dB before the rails are where the
# analog side starts buzzing, which reads as "too loud" rather than as distortion.
# Anything above SOFT_KNEE gets rounded off instead of sheared flat; see
# Downsampler._to_pcm. Turn OUTPUT_GAIN down if the speaker itself is being overdriven,
# which digital headroom cannot fix.
FULL_SCALE = 32767.0
SOFT_KNEE = FULL_SCALE * 0.80
OUTPUT_GAIN = 1.0               # overridden from env in main()

# Auto-gain. Fixing the aliasing (see Downsampler) took real energy above 8 kHz out
# of the signal along with the alias artifacts -- and that energy, alias or not, was
# most of what made speech sound loud. The honest fix for loudness is gain, not
# distortion, so rather than hand-tune OUTPUT_GAIN per voice this tracks how loud
# recent speech actually was and boosts quiet passages toward the ceiling on its
# own. The soft limiter is still the safety net for anything faster than the
# envelope has caught up to -- a sudden loud transient can outrun a smoothed
# average by design, and that is what the limiter is for.
AGC_TARGET     = 0.92    # fraction of full scale the envelope is steered toward
AGC_MIN_GAIN   = 0.6     # never push already-loud speech down further
AGC_MAX_GAIN   = 3.0     # cap on quiet-passage boost, so hiss cannot run away
AGC_RELEASE    = 0.90    # envelope decay per feed() call (~60 ms of audio)
AGC_ENABLED    = True    # overridden from env in main()


# --------------------------------------------------------------------------- env

def load_env(root: Path) -> None:
    """.env then .env.local, the latter winning. Real values never enter git.

    Hand-rolled rather than python-dotenv: two files and `KEY=value` is the whole
    format, and the venv stays dependency-light.
    """
    for name in (".env", ".env.local"):
        path = root / name
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            value = value.strip()
            if value:                       # blank template entries must not shadow
                os.environ[key.strip()] = value


# ---------------------------------------------------------------------- resample

def _lowpass(taps: int, fs: float, fc: float) -> list[float]:
    """Windowed-sinc low pass, Hamming, normalised to unity DC gain."""
    d = (taps - 1) / 2
    h = []
    for i in range(taps):
        m = i - d
        x = 2 * fc / fs
        v = x * (1.0 if m == 0 else math.sin(math.pi * x * m) / (math.pi * x * m))
        v *= 0.54 - 0.46 * math.cos(2 * math.pi * i / (taps - 1))
        h.append(v)
    s = sum(h)
    return [v / s for v in h]


class Downsampler:
    """24 kHz Gemini audio -> 16 kHz device audio, without the aliasing.

    THE PREVIOUS VERSION OF THIS WAS THE SECOND AUDIO BUG, and it sounded nothing
    like the first one. Linear interpolation is not a resampler, it is an
    interpolator with no anti-alias filter at all: everything above the output's
    8 kHz Nyquist folds straight back down into the voice band. Measured, feeding
    pure tones through both paths and reading the level at the fold frequency:

        input 24k    lands at    linear      this
          9000 Hz      7000 Hz    72.4 dB    15.2 dB
         10000 Hz      6000 Hz    71.5 dB    11.6 dB
         11000 Hz      5000 Hz    70.6 dB    15.5 dB

    A 10 kHz tone arrived at 6 kHz essentially undiminished -- a full-strength ghost
    at the wrong pitch. Speech from a 24 kHz TTS has real energy up there in every
    "s", "sh" and "t", so every sibilant was dumping an inharmonic buzz into the
    middle of the voice. That is the "cracking AM radio" quality: it rides on the
    voice rather than punctuating it, which is what makes it sound different from the
    frame-boundary clicks fixed earlier (see enqueue_audio).

    Done properly: upsample by 2, low-pass, decimate by 3. Polyphase, so the zero
    samples are never actually multiplied -- 2.3 ms per 60 ms frame, in a worker
    thread. 81 taps at 7.3 kHz keeps the passband flat to 7 kHz and puts the stop
    band 60 dB down.

    Stateful on purpose. The filter history carries across blocks, so there is no
    discontinuity at chunk boundaries -- which would be a new click in place of the
    old one.
    """

    L, M = 2, 3          # 24000 * 2 / 3 = 16000
    TAPS, CUTOFF = 81, 7300.0

    def __init__(self, src_rate: int, dst_rate: int):
        if src_rate * self.L != dst_rate * self.M:
            raise ValueError(f"{src_rate}->{dst_rate} is not the 3:2 ratio this "
                             f"resampler implements")
        # Gain L compensates the zero stuffing of the upsample.
        self.h = [v * self.L for v in _lowpass(self.TAPS, src_rate * self.L, self.CUTOFF)]
        self.peak = 0                 # |sample| high-water mark, for headroom checks
        self.clipped = 0              # samples the limiter had to round off
        # Neutral (gain 1.0) until real audio has informed it -- starting from 0 would
        # read the first, quietest block as silence and slam the gain to AGC_MAX_GAIN
        # before there is any signal to justify it.
        self._envelope = AGC_TARGET * FULL_SCALE
        self.agc_gain = 1.0
        self.reset()

    def reset(self):
        """Forget the filter history, at a real discontinuity such as a barge-in.

        Level statistics deliberately survive: they describe the speaker, not the
        stream, and zeroing them on every interrupt would hide exactly the loud turns
        worth knowing about.
        """
        self.buf: list[int] = []      # buf[0] is absolute input index self.base
        self.base = 0
        self.n_in = 0
        self.n_out = 0

    def take_levels(self) -> tuple[float, int, float]:
        """Peak as a fraction of full scale, samples limited, and the current AGC gain."""
        peak, clipped = self.peak / FULL_SCALE, self.clipped
        self.peak = self.clipped = 0
        return peak, clipped, self.agc_gain

    def feed(self, pcm: bytes) -> bytes:
        """One block in, whole samples out. 1440 in -> exactly 960 out, always."""
        self.buf.extend(memoryview(pcm).cast("h"))
        self.n_in += len(pcm) // 2
        h, taps, out = self.h, self.TAPS, []

        while True:
            k0 = self.M * self.n_out          # index into the virtual 48 kHz signal
            if k0 // self.L > self.n_in - 1:
                break
            # u[k] = x[k/2] for even k and 0 otherwise, so only the taps whose parity
            # matches k0 can contribute -- half the multiplies, exactly.
            acc = 0.0
            t = k0 % self.L
            while t < taps:
                xi = (k0 - t) // self.L
                if xi < self.base:
                    break                     # start-up: missing history is silence
                acc += h[t] * self.buf[xi - self.base]
                t += self.L
            out.append(acc)
            self.n_out += 1

        # Retain only the history future outputs still need.
        need = (self.M * self.n_out - taps + 1) // self.L
        if need > self.base:
            del self.buf[:need - self.base]
            self.base = need

        if AGC_ENABLED and out:
            # Measured on the filtered signal BEFORE any gain -- the envelope must
            # track what the source actually contains, not last block's already-boosted
            # output, or the loop would compound its own gain onto itself.
            block_peak = max(abs(v) for v in out)
            # Attack instantly on a louder block (a sudden quiet-to-loud jump should not
            # clip while the envelope catches up); release slowly on a quieter one,
            # so a single soft syllable does not un-boost the whole passage around it.
            self._envelope = (block_peak if block_peak > self._envelope
                              else self._envelope * AGC_RELEASE + block_peak * (1 - AGC_RELEASE))
            wanted = AGC_TARGET * FULL_SCALE / max(self._envelope, 1.0)
            self.agc_gain = min(max(wanted, AGC_MIN_GAIN), AGC_MAX_GAIN)

        return self._to_pcm(out)

    def _to_pcm(self, samples: list[float]) -> bytes:
        """Gain, soft limit, clamp -- and keep score."""
        gain, out = OUTPUT_GAIN * self.agc_gain, []
        for v in samples:
            v *= gain
            a = abs(v)
            if a > self.peak:
                self.peak = int(a)
            if a > SOFT_KNEE:
                # Round the top off rather than shearing it. A hard clamp turns a
                # loud vowel into a square wave, which is broadband buzz -- the
                # digital half of "too loud". tanh is expensive and only the few
                # samples that are actually near the rails ever reach it.
                over = (a - SOFT_KNEE) / (FULL_SCALE - SOFT_KNEE)
                a = SOFT_KNEE + (FULL_SCALE - SOFT_KNEE) * math.tanh(over)
                v = a if v > 0 else -a
                self.clipped += 1
            out.append(int(v))
        return struct.pack(f"<{len(out)}h", *out)


# ------------------------------------------------------------------------- tools
#
# The subset exposed to the model is deliberately "self-control only": the things
# that make the robot feel embodied while it talks. Each maps 1:1 onto an MCP tool
# the firmware already serves, so there is no bespoke command vocabulary to keep in
# sync -- the device remains the source of truth for what it can do.
#
# The camera is exposed as take_photo, but it maps to self.camera.CAPTURE, a tool added
# in hal_mcp.cpp -- not upstream's self.camera.take_photo. Upstream captures the same
# frame and then POSTs it to a remote VLM, returning that service's prose; the URL is a
# call-home this project removes and is unset over USB anyway. Ours returns the JPEG as
# an MCP image block, and the client hands the pixels to Gemini as realtime media so the
# model looks at the photo itself rather than reading someone's description of it.
#
# Deliberately NOT exposed:
#   self.camera.take_photo   -- the remote-VLM path described above.
#   self.screen.set_theme    -- cosmetic, and easy for a chatty model to fiddle with.
#   self.robot.*reminder*    -- scheduling wants persistence and a story about what
#                               happens when the session ends. Later.

TOOLS = [
    types.Tool(function_declarations=[
        types.FunctionDeclaration(
            name="set_head_angles",
            description=(
                "Move your head. Use this often and naturally while talking -- tilt "
                "when curious, look up when thinking, turn toward what you mention. "
                "YAW -128..128: NEGATIVE turns toward the LEFT of the person facing "
                "you, POSITIVE toward their RIGHT. Phrased from the viewer's side on "
                "purpose: when someone says 'look left' they mean their own left, and "
                "any wording like 'your left' gets mirrored into the opposite motion. "
                "Verified on hardware -- yaw -120 physically turns to the viewer's left. "
                "PITCH 0..90, where 90 is up. Stay within +/-45 yaw for normal "
                "conversation. Speed 100-1000, 150 is natural, 700 is excited."
            ),
            parameters=types.Schema(
                type=types.Type.OBJECT,
                properties={
                    "yaw": types.Schema(type=types.Type.INTEGER,
                                        description="-128..128, omit to leave unchanged"),
                    "pitch": types.Schema(type=types.Type.INTEGER,
                                          description="0..90, omit to leave unchanged"),
                    "speed": types.Schema(type=types.Type.INTEGER,
                                          description="100..1000, default 150"),
                },
            ),
        ),
        types.FunctionDeclaration(
            name="set_face",
            description=(
                "Change your facial expression for a few seconds, then it returns to "
                "neutral on its own. Use it as punctuation while you speak -- happy when "
                "something pleases you, laughing at a joke, sleepy late in the day, sad "
                "or crying when commiserating, angry only in jest. Do not narrate it and "
                "do not use it on every turn; a face that is always doing something reads "
                "as a screensaver. One well-timed expression beats five."
            ),
            parameters=types.Schema(
                type=types.Type.OBJECT,
                properties={
                    "emotion": types.Schema(
                        type=types.Type.STRING,
                        description="neutral, happy, laughing, angry, sad, crying, sleepy"),
                    "seconds": types.Schema(
                        type=types.Type.INTEGER,
                        description="how long to hold it, 2-10, default 4"),
                },
                required=["emotion"],
            ),
        ),
        types.FunctionDeclaration(
            name="get_head_angles",
            description="Where your head is pointing right now, in degrees.",
            parameters=types.Schema(type=types.Type.OBJECT, properties={}),
        ),
        # set_led_color is deliberately NOT exposed. The firmware already uses the LED as
        # the conversation-state indicator -- green listening, blue speaking, off idle
        # (avatar_controller.cc) -- and a model told to "show your mood" overwrites it,
        # which reads as the state machine being broken when it is working fine. A status
        # light and an expression channel cannot be the same LED. Re-add this block if you
        # would rather have mood than state.
        types.FunctionDeclaration(
            name="take_photo",
            description=(
                "Look through your camera and see what is in front of you. Use this "
                "whenever you are asked to look at something, or what you can see. "
                "The image is given to you directly -- after calling this, describe "
                "what you actually observe. If the subject is off to one side, point "
                "your head with set_head_angles first, then take the photo."
            ),
            parameters=types.Schema(type=types.Type.OBJECT, properties={}),
        ),
        types.FunctionDeclaration(
            name="play_sound",
            description=(
                "Play a short sound effect: success, exclamation, popup, vibration. "
                "A chirp lands where a spoken 'ta-da' is just more talking. "
                "Punctuation, not decoration -- never on every turn. "
                # NOT 'welcome': that name is a translated, prerecorded human voice
                # clip (37 locale files, ~1.7s -- every other sound here is one
                # locale-independent chime a third the length). It talks over you in
                # a voice that has nothing to do with yours. Never call it.
            ),
            parameters=types.Schema(
                type=types.Type.OBJECT,
                properties={"name": types.Schema(
                    type=types.Type.STRING,
                    description="success, exclamation, popup or vibration")},
                required=["name"],
            ),
        ),
        types.FunctionDeclaration(
            name="set_reminder",
            description=(
                "Set a timer that will make you speak up when it expires. Good for "
                "'remind me in ten minutes' or a pomodoro. Duration is in SECONDS."
            ),
            parameters=types.Schema(
                type=types.Type.OBJECT,
                properties={
                    "duration_seconds": types.Schema(type=types.Type.INTEGER),
                    "message": types.Schema(type=types.Type.STRING,
                                            description="what to say when it fires"),
                    "repeat": types.Schema(type=types.Type.BOOLEAN),
                },
                required=["duration_seconds", "message"],
            ),
        ),
        types.FunctionDeclaration(
            name="list_reminders",
            description="What timers are currently running.",
            parameters=types.Schema(type=types.Type.OBJECT, properties={}),
        ),
        types.FunctionDeclaration(
            name="cancel_reminder",
            description="Cancel a timer by its id, from list_reminders.",
            parameters=types.Schema(
                type=types.Type.OBJECT,
                properties={"id": types.Schema(type=types.Type.INTEGER)},
                required=["id"],
            ),
        ),
        types.FunctionDeclaration(
            name="get_device_status",
            description=("Your own real-time status: battery, network, speaker volume, "
                         "screen brightness. Call this before changing volume or brightness."),
            parameters=types.Schema(type=types.Type.OBJECT, properties={}),
        ),
        types.FunctionDeclaration(
            name="set_volume",
            description="Set speaker volume 0-100. Check get_device_status first.",
            parameters=types.Schema(
                type=types.Type.OBJECT,
                properties={"volume": types.Schema(type=types.Type.INTEGER)},
                required=["volume"],
            ),
        ),
        types.FunctionDeclaration(
            name="set_brightness",
            description="Set screen brightness 0-100.",
            parameters=types.Schema(
                type=types.Type.OBJECT,
                properties={"brightness": types.Schema(type=types.Type.INTEGER)},
                required=["brightness"],
            ),
        ),
    ])
]


def grounding_enabled() -> bool:
    """Google Search grounding, on unless explicitly switched off.

    Read at call time rather than at import: load_env() runs inside main(), so a
    module-level constant would be decided before .env.local has been read.
    """
    return os.environ.get("GEMINI_API_SEARCH_GROUNDING", "1").lower() not in (
        "0", "false", "no", "off", "")


def tools_for(video_streaming: bool):
    """The tool set, minus take_photo when frames are already streaming.

    Leaving take_photo declared during a video session is worse than redundant. The model
    is already receiving frames, so calling it adds a second image of the same scene, a
    round trip of latency, and a capture that competes with the stream for the device's
    CPU -- and it invites the model to say "let me take a look" when it is already
    looking. Removing the declaration is the honest way to say "you can see continuously
    now"; a description asking it not to call the tool would just be a suggestion.
    """
    if not video_streaming:
        tools = list(TOOLS)
    else:
        kept = [f for f in TOOLS[0].function_declarations if f.name != "take_photo"]
        tools = [types.Tool(function_declarations=kept)]

    # Search grounding rides alongside the device tools rather than replacing them --
    # the Live API takes a list, and the model picks per turn. It is a separate Tool
    # entry because it is not a function declaration: the search runs server-side and
    # nothing comes back through handle_tool_call.
    #
    # Default on. The failure it prevents is the one that actually happens on a desk
    # robot: someone asks the time, the weather, or who won last night, and a model
    # with a training cutoff answers confidently and wrongly. Being able to say "I do
    # not know, let me check" is worth more here than the latency it costs, and the
    # cost lands only on turns where the model chooses to search.
    if grounding_enabled():
        tools.append(types.Tool(google_search=types.GoogleSearch()))
    return tools


# Gemini's flat tool names -> the device's namespaced MCP names.
MCP_NAMES = {
    "set_head_angles": "self.robot.set_head_angles",
    "get_head_angles": "self.robot.get_head_angles",
    # Our own tool, registered in hal_mcp.cpp. NOT upstream's self.camera.take_photo,
    # which POSTs the frame to a remote VLM and returns prose about it.
    "take_photo": "self.camera.capture",
    "play_sound": "self.robot.play_sound",
    "set_reminder": "self.robot.create_reminder",
    "list_reminders": "self.robot.get_reminders",
    "cancel_reminder": "self.robot.stop_reminder",
    "get_device_status": "self.get_device_status",
    "set_volume": "self.audio_speaker.set_volume",
    "set_brightness": "self.screen.set_brightness",
}

# The firmware's "leave this axis alone" sentinel. Sending a real angle for an axis
# the model did not mention would yank the head to centre on every call.
LEAVE_ALONE = -9999


# ------------------------------------------------------------------------ device

class Device:
    """The one connection to StackChan. Owns the socket and the MCP request ids."""

    def __init__(self, url: str):
        self.url = url
        self.ws = None
        self.codec = None
        self.session_id = str(uuid.uuid4())
        self._next_id = 0
        self._pending: dict[int, asyncio.Future] = {}
        self.mic = asyncio.Queue()
        self.listening = asyncio.Event()
        # Encoded frames waiting to go out, drained by pace() at real time. Gemini
        # produces audio far faster than realtime; the device decodes at exactly
        # realtime, so anything sent eagerly piles into its queue and gets dropped.
        self.tx = asyncio.Queue()
        self._turn_ending = False
        # Leftover source-rate PCM that did not fill a whole device frame. Carried across
        # chunks; see enqueue_audio for why this matters more than anything else here.
        self._pcm_residual = b""
        # Built once the codec is known (its rate is what we resample TO). Stateful, so
        # it belongs to the connection rather than to a call.
        self._down = None
        # A JPEG waiting to be sent as its own turn, once the tool call that produced
        # it has been answered. See handle_tool_call.
        self.pending_photo = None
        # Pending revert-to-neutral for set_face; a new expression cancels it.
        self._face_timer = None
        # Physical events reported by the device (petting, shaking), waiting to be folded
        # into the model's context. A queue rather than a flag: two pats are two events.
        self.sensor_events = asyncio.Queue()
        # Set by the downlink from session_resumption_update, read when reconnecting.
        # Carried across device reconnects too -- a pulled cable does not end the
        # conversation, it only interrupts the transport.
        self.resumption_handle = None
        # Set from the device's listen message when the session was started with the
        # camera button rather than a face tap. Audio-only sessions never set it, so the
        # camera cannot switch itself on.
        self.video_session = False
        # Device-side VAD, forwarded over the protocol. Camera streaming is gated on it.
        self.voice_active = False
        # Barge-in latch. Set when the user taps the face while the robot is speaking;
        # cleared when the abandoned turn finally ends server-side. While set, audio
        # arriving from Gemini is dropped on the floor instead of queued -- see
        # enqueue_audio. Without it, clearing the queue only buys a moment of quiet
        # before the rest of the generated turn refills it.
        self.barged_in = False
        # Set while pace() has a tts:start outstanding, so a barge-in knows whether
        # there is anything to interrupt. Read from the pump, written by pace().
        self.speaking = False
        # Barge-ins observed by the pump, for the session task to report upstream. The
        # pump handles the local half itself (that is the half with a latency budget)
        # and must never block on the model.
        self.aborts = asyncio.Queue()

    async def connect(self):
        # ping_interval/ping_timeout are the half-open detector. When an SSH tunnel dies
        # or the laptop is unplugged, neither end sees a FIN -- TCP will hold a corpse
        # open for many minutes, and the client sits there believing it is connected.
        # WebSocket pings turn that silence into a prompt, reconnectable failure.
        # close_timeout keeps teardown from stalling the reconnect loop.
        self.ws = await websockets.connect(
            self.url, open_timeout=15, max_size=None,
            ping_interval=5, ping_timeout=15, close_timeout=5)
        await self.ws.send(json.dumps({
            "type": "hello", "transport": "websocket", "session_id": self.session_id,
            "audio_params": {"format": "opus", "sample_rate": GEMINI_SEND_RATE,
                             "frame_duration": 60}}))
        # The device greets us too; codec_from_hello reads the rate/frame it chose
        # rather than assuming, so a firmware change to either cannot desync us.
        self.codec = codec_from_hello({"audio_params": {
            "format": "opus", "sample_rate": GEMINI_SEND_RATE, "frame_duration": 60}})
        self._down = Downsampler(GEMINI_RECEIVE_RATE, self.codec.sample_rate)
        return self

    async def pump(self):
        """Read forever: mic audio to the queue, events to the state, replies to futures."""
        async for msg in self.ws:
            if isinstance(msg, bytes):
                if self.listening.is_set():
                    try:
                        self.mic.put_nowait(self.codec.decode(msg))
                    except Exception:
                        pass          # a dropped mic frame is not worth a traceback
                continue

            data = json.loads(msg)
            kind = data.get("type")
            if kind == "mcp":
                payload = data.get("payload", {})
                fut = self._pending.pop(payload.get("id"), None)
                if fut and not fut.done():
                    fut.set_result(payload)
            elif kind == "listen":
                # The device opened the user's turn -- this is the tap, or the device
                # re-opening the turn after it finished speaking.
                if data.get("state") == "start":
                    # "video":true means the camera button was used. Absent on older
                    # firmware and on plain taps, which is exactly the audio-only default.
                    self.video_session = bool(data.get("video"))
                    self.listening.set()
                    print(f"● listening ({'camera + mic' if self.video_session else 'mic only'})",
                          flush=True)
                elif data.get("state") == "stop":
                    self.listening.clear()
                    self.voice_active = False
            elif kind == "abort":
                # The device interrupting ITSELF, on a face tap while speaking.
                # application.cc: HandleStartListening -> AbortSpeaking -> this frame.
                # It already stopped its own playback locally; if we ignore the frame we
                # keep streaming a turn nobody is listening to, and the queued frames go
                # on arriving for as long as the model keeps generating. That is exactly
                # "we have to wait for the whole thing to finish".
                await self.barge_in()
                self.aborts.put_nowait(data.get("reason") or "user")
                print("  ✋ interrupted", flush=True)
            elif kind == "sensor":
                # Something physical happened TO the robot -- petted, shaken. Queued for
                # the session task to fold into context; the pump must not block on the
                # model, and it does not own the Gemini session anyway.
                ev = data.get("event")
                if ev:
                    self.sensor_events.put_nowait(ev)
                    print(f"  ✋ {ev}", flush=True)
            elif kind == "vad":
                # Device-side VAD. Camera streaming is gated on this: frames only while
                # someone is actually speaking.
                self.voice_active = (data.get("state") == "speech")
            elif kind == "tts":
                pass
            elif kind == "goodbye":
                self.listening.clear()
                self.voice_active = False
                self.video_session = False

    async def call(self, mcp_name: str, arguments: dict, timeout=10.0):
        """One MCP tools/call, awaited by id."""
        self._next_id += 1
        req_id = self._next_id
        fut = asyncio.get_running_loop().create_future()
        self._pending[req_id] = fut
        await self.ws.send(json.dumps({
            "session_id": self.session_id, "type": "mcp",
            "payload": {"jsonrpc": "2.0", "id": req_id, "method": "tools/call",
                        "params": {"name": mcp_name, "arguments": arguments}}}))
        try:
            return await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            self._pending.pop(req_id, None)
            return {"error": {"message": f"{mcp_name} timed out"}}

    async def close(self):
        """Best-effort teardown. A link we are already abandoning must never raise."""
        try:
            if self.ws is not None:
                await self.ws.close()
        except Exception:
            pass

    def _src_bytes_per_frame(self) -> int:
        """Source-rate (24 kHz) bytes that map to exactly one device frame."""
        return int(GEMINI_RECEIVE_RATE * self.codec.frame_ms / 1000) * 2

    # The seven the avatar actually implements (avatar_controller.cc). Anything else is
    # silently ignored by the device, which would look like the tool doing nothing.
    FACES = ("neutral", "happy", "laughing", "angry", "sad", "crying", "sleepy")

    async def set_face(self, emotion: str, seconds: int = 4):
        """Hold an expression, then let it lapse back to neutral.

        Time-limited on purpose. An expression that sticks stops being an expression and
        becomes the face -- the robot ends up permanently furious because something was
        mildly annoying six minutes ago. Reverting also means the model never has to
        remember to undo anything, which is exactly the kind of bookkeeping it forgets.
        """
        emotion = emotion if emotion in self.FACES else "neutral"
        seconds = max(2, min(10, int(seconds)))

        await self.ws.send(json.dumps({
            "session_id": self.session_id, "type": "llm",
            "emotion": emotion, "text": ""}))

        # A later expression supersedes an earlier one rather than queueing behind it.
        if self._face_timer is not None:
            self._face_timer.cancel()
        if emotion == "neutral":
            self._face_timer = None
            return

        async def revert():
            try:
                await asyncio.sleep(seconds)
                await self.ws.send(json.dumps({
                    "session_id": self.session_id, "type": "llm",
                    "emotion": "neutral", "text": ""}))
            except (asyncio.CancelledError, Exception):
                pass          # superseded, or the link went away; neither is worth noise

        self._face_timer = asyncio.create_task(revert())

    async def enqueue_audio(self, pcm24: bytes):
        """Resample + encode OFF the event loop, then queue for paced sending.

        THE IMPORTANT PART IS THE RESIDUAL.

        codec.encode() zero-pads a trailing partial frame, which is right for a complete
        utterance and catastrophic per chunk: Gemini streams arbitrary-sized chunks, so
        almost every one ends mid-frame and gets padded with silence. That injects a burst
        of zeros into the middle of continuous speech several times a second -- which is
        exactly what "crackle and jitter" sounds like. It is not a scheduling problem and
        no amount of buffering downstream can repair it, because the damage is already in
        the samples.

        So we accumulate at the SOURCE rate and only ever hand whole frames onward.
        Slicing at the source rate rather than after resampling also keeps the resampler
        phase-continuous: 24k -> 16k is 3:2, so one 60 ms device frame is exactly 1440
        source samples, and resampling that exact quantity avoids a fractional-phase
        reset (another, quieter click) at every chunk boundary.

        Both steps are CPU-bound -- pure-Python resampling plus a blocking Opus call -- so
        they run in a worker thread; inline they stall the event loop the microphone
        uplink shares, which made the choppiness bidirectional.
        """
        # Interrupted: the model is still delivering a turn the user has already
        # dismissed. Drop it rather than queue it. Cleared by the server telling us the
        # turn is genuinely over (downlink), which is the only moment it is safe to
        # start playing again -- clearing on the next chunk would just resume it.
        if self.barged_in:
            return

        self._pcm_residual += pcm24
        step = self._src_bytes_per_frame()
        n = len(self._pcm_residual) // step
        if n == 0:
            return
        whole, self._pcm_residual = self._pcm_residual[:n * step], self._pcm_residual[n * step:]

        def work():
            pcm = self._down.feed(whole)
            return self.codec.encode(pcm)

        for frame in await asyncio.to_thread(work):
            self.tx.put_nowait(frame)

    async def flush_audio(self):
        """End of turn: emit the tail, padded. Padding is correct HERE and only here."""
        if not self._pcm_residual:
            return
        tail, self._pcm_residual = self._pcm_residual, b""

        def work():
            pcm = self._down.feed(tail)
            return self.codec.encode(pcm)

        for frame in await asyncio.to_thread(work):
            self.tx.put_nowait(frame)

    async def pace(self):
        """Drain the queue at exactly one frame per frame_ms. The whole fix.

        Sending faster than this is what made playback choppy: the device decodes in
        real time, so an eager sender just overruns its queue and the surplus is
        discarded. Timing off a monotonic deadline rather than sleeping frame_ms each
        pass keeps encode/send cost from accumulating into drift.
        """
        period = self.codec.frame_ms / 1000.0
        # On the instance, not a local: the pump reads it to decide whether a face tap
        # is a barge-in or just the start of a turn.
        self.speaking = False
        deadline = None
        while True:
            # Pre-roll before the first frame of a turn. The device plays in hard real
            # time, so if the queue ever runs dry mid-utterance it underruns and clicks.
            # Gemini delivers in bursts, and starting on the very first frame means the
            # first network hiccup is audible. kPreroll frames of head start costs that
            # much latency once per turn and absorbs the jitter for the rest of it.
            if not self.speaking and self.tx.qsize() < PREROLL_FRAMES:
                try:
                    await asyncio.wait_for(asyncio.shield(self._preroll_wait()), timeout=0.4)
                except asyncio.TimeoutError:
                    pass          # short utterance: play what we have rather than stall

            frame = await self.tx.get()

            if frame is None:                     # end-of-turn marker
                if self.speaking:
                    await self.ws.send(json.dumps({"session_id": self.session_id,
                                                   "type": "tts", "state": "stop"}))
                    self.speaking = False
                    # Headroom report, once per turn. Distortion is hard to attribute
                    # by ear -- digital clipping, an overdriven speaker and aliasing
                    # all sound "crackly" -- so print the one number that separates
                    # them. A peak below 1.0 with nothing limited means the level is
                    # innocent and the buzz is somewhere else.
                    if self._down is not None:
                        peak, clipped, gain = self._down.take_levels()
                        if peak:
                            note = f" ⚠ {clipped} limited" if clipped else ""
                            gain_note = f" gain x{gain:.2f}" if AGC_ENABLED else ""
                            print(f"  🔊 peak {peak:.2f} FS{gain_note}{note}", flush=True)
                deadline = None
                continue

            if not self.speaking:
                await self.ws.send(json.dumps({
                    "session_id": self.session_id, "type": "tts", "state": "start",
                    "sample_rate": self.codec.sample_rate}))
                self.speaking = True
                deadline = asyncio.get_running_loop().time()

            await self.ws.send(frame)
            deadline += period
            delay = deadline - asyncio.get_running_loop().time()
            if delay > 0:
                await asyncio.sleep(delay)
            else:
                # We fell behind (a slow encode, a long tool call). Reset rather than
                # burst to catch up -- bursting is the very thing that drops frames.
                deadline = asyncio.get_running_loop().time()

    async def _preroll_wait(self):
        while self.tx.qsize() < PREROLL_FRAMES:
            await asyncio.sleep(0.01)

    async def end_turn(self):
        await self.flush_audio()
        await self.tx.put(None)

    async def barge_in(self):
        """Stop talking NOW, and stay stopped until the abandoned turn really ends.

        Three things have to happen together or the interrupt is only cosmetic:

        1. Throw away what is queued but unsent. Up to a couple of seconds of speech
           can be sitting in `tx` -- that is the pre-roll and pacing working as
           designed, and it is exactly what would otherwise keep playing after the tap.
        2. Close the turn on the device, so it leaves `speaking` and the avatar's mouth
           stops. Done by pushing the existing end-of-turn marker rather than sending
           `tts stop` from here: pace() owns that message, and two writers racing on it
           is how you get a device stuck mid-utterance.
        3. Latch `barged_in`. Gemini has already generated the rest of the turn and will
           keep delivering it for seconds after the tap. Without the latch, step 1 buys
           a fraction of a second of silence and then the robot carries on talking --
           which looks *more* broken than not interrupting at all.
        """
        if not self.speaking and self.tx.empty():
            return
        self.barged_in = True
        self.drop_pending_audio()
        if self._down is not None:
            self._down.reset()
        await self.tx.put(None)

    def drop_pending_audio(self):
        """Barge-in: discard queued audio that has not been sent yet."""
        self._pcm_residual = b""
        # A JPEG waiting to be sent as its own turn, once the tool call that produced
        # it has been answered. See handle_tool_call.
        self.pending_photo = None
        while not self.tx.empty():
            try:
                self.tx.get_nowait()
            except asyncio.QueueEmpty:
                break


# ------------------------------------------------------------------------ bridge

def extract_jpeg(reply: dict) -> bytes | None:
    """Pull the JPEG out of an MCP image result.

    McpServer serialises image content double-encoded: the block is
    {"type":"image","image":"<json string>"} where the inner string is itself
    {"type":"image","mimeType":...,"data":"<base64>"}. Handle both that shape and the
    flat one, so this keeps working if upstream ever tidies the nesting up.
    """
    for block in reply.get("result", {}).get("content", []):
        if not isinstance(block, dict) or block.get("type") != "image":
            continue
        inner = block.get("image")
        if isinstance(inner, str):
            try:
                inner = json.loads(inner)
            except json.JSONDecodeError:
                inner = None
        payload = inner if isinstance(inner, dict) else block
        b64 = payload.get("data")
        if b64:
            try:
                return base64.b64decode(b64)
            except (ValueError, binascii.Error):
                return None
    return None


async def handle_tool_call(device: Device, call, session) -> types.FunctionResponse:
    args = dict(call.args or {})
    mcp_name = MCP_NAMES.get(call.name)
    if mcp_name is None and call.name != "set_face":
        return types.FunctionResponse(id=call.id, name=call.name,
                                      response={"error": f"unknown tool {call.name}"})

    if call.name == "set_face":
        # Handled entirely in the client: the device takes emotions over the protocol
        # ({"type":"llm","emotion":...}), so this needs no MCP tool and no firmware change.
        emotion = str(args.get("emotion", "neutral"))
        seconds = int(args.get("seconds", 4) or 4)
        await device.set_face(emotion, seconds)
        print(f"  🙂 {emotion} for {max(2, min(10, seconds))}s", flush=True)
        return types.FunctionResponse(id=call.id, name=call.name,
                                      response={"result": f"showing {emotion}"})

    if call.name == "take_photo":
        # Capture + JPEG encode on device takes a beat; the default timeout is too tight.
        reply = await device.call(mcp_name, {}, timeout=25.0)
        if "error" in reply:
            return types.FunctionResponse(id=call.id, name=call.name,
                                          response={"error": str(reply["error"])})
        jpeg = extract_jpeg(reply)
        if not jpeg:
            return types.FunctionResponse(id=call.id, name=call.name,
                                          response={"error": "camera returned no image"})
        # The pixels go in as an image part, NOT as the function response -- a tool result
        # is text, and describing an image in text is exactly what we removed the remote
        # VLM to avoid. The model sees the frame itself and answers from it.

        # ---- Hand the frame over AFTER the tool call is resolved -----------------
        #
        # Three orderings have been tried here; this is the one that is actually
        # well-defined.
        #
        #  1. realtime lane -- ordered against the AUDIO CLOCK, not conversation turns, so
        #     the frame landed after the turn committed and showed up one turn late.
        #  2. conversation lane with turn_complete=False, sent before the tool response --
        #     correctly ordered, but it leaves the user turn OPEN, and a function response
        #     does not close it. The model waits for a turn that never completes and says
        #     nothing at all. A race traded for a stall.
        #  3. this: resolve the function call, THEN send the image as a complete user
        #     turn. The call is answered so the model is unblocked, and the image arrives
        #     as an ordinary turn that triggers generation with the picture already in
        #     context. Nothing is racing and nothing is left half-open.
        #
        # The send itself happens in downlink(), right after the tool response goes out,
        # because ordering is the entire point and it cannot be guaranteed from here.
        device.pending_photo = jpeg
        print(f"  📷 {len(jpeg)} bytes of JPEG queued behind the tool response", flush=True)

        return types.FunctionResponse(
            id=call.id, name=call.name,
            response={"result": "Photo captured. The image is being sent to you now -- "
                                "wait for it, then describe what is actually in it."})

    if call.name == "set_head_angles":
        args = {"yaw": int(args.get("yaw", LEAVE_ALONE)),
                "pitch": int(args.get("pitch", LEAVE_ALONE)),
                "speed": int(args.get("speed", 150))}

    print(f"  ⚙ {call.name}({args})", flush=True)
    reply = await device.call(mcp_name, args)

    if "error" in reply:
        return types.FunctionResponse(id=call.id, name=call.name,
                                      response={"error": str(reply["error"])})
    # MCP returns content blocks; hand the model the text it carries.
    content = reply.get("result", {}).get("content", [])
    text = " ".join(b.get("text", "") for b in content if isinstance(b, dict)) or "ok"
    return types.FunctionResponse(id=call.id, name=call.name, response={"result": text})


async def converse(device: Device, session):
    """Bridge one live session: mic up, audio down, tool calls across."""

    async def abort_uplink():
        """Tell the model the turn was dismissed. The local half already happened.

        Split deliberately: the pump stops playback the instant the frame arrives,
        because that is the half with a latency budget a human can feel. This half is
        bookkeeping and can take as long as the network takes.

        It matters for two reasons even though the robot is already quiet. Gemini keeps
        generating a turn nobody is listening to, which costs tokens and delays the next
        one; and left unsaid, the model believes it delivered the whole utterance and
        will reference things the user never heard. Realtime input interrupts the
        current generation, so this both stops it and explains why.
        """
        while True:
            await device.aborts.get()
            try:
                await session.send_realtime_input(
                    text="[SENSOR] The person just cut you off mid-sentence -- they "
                         "tapped your face to stop you talking. Stop where you are. "
                         "They did not hear the rest, so do not refer back to it.")
            except Exception as exc:
                print(f"  ⚠ interrupt not delivered: {type(exc).__name__}", flush=True)

    async def sensor_uplink():
        """Fold physical events into context, and answer them when nothing else is going on.

        PHRASING MATTERS MORE THAN IT LOOKS. These arrive as role="user" content, which is
        the only channel available mid-session -- so anything in the third person ("someone
        is petting the robot") reads as the USER SAYING those words, about a robot, from
        outside. The model then answers the sentence instead of feeling the touch.

        So: second person, present tense, addressed to the model as its own sensation, and
        tagged [SENSOR] so it is unmistakably instrumentation rather than speech. The system
        instruction explains the tag; the two have to agree or the tag is just noise.

        TWO CASES, AND THE ORIGINAL VERSION ONLY HANDLED ONE.

        Mid-conversation, turn_complete=False is right: being petted should colour what
        the robot says next, not interrupt to announce itself. A robot that says "you
        touched me!" over the top of the person talking is a car alarm.

        But in SILENCE that same choice means nothing happens at all. The note sits in
        context waiting for a turn that only arrives when the person speaks -- so petting
        the robot appeared to do nothing, and then every pat that had accumulated landed
        at once and muddled a reply about something else entirely. Touch with no response
        is not subtlety, it is a broken button.

        So when the robot is quiet and nobody is speaking, close the turn and let it
        react. Guarded three ways, because the fix is only an improvement if it stays
        rare: events are coalesced over a short window (a stroke is many events, not
        many touches), a cooldown stops continuous petting turning into a monologue, and
        anything arriving mid-turn still uses the quiet path.
        """
        NOTE = {
            "head_pet": "[SENSOR] Someone is stroking the top of your head right now. "
                        "You can feel it.",
            "shaken": "[SENSOR] You have just been picked up and shaken about. "
                      "Everything is still wobbling.",
        }
        # A single stroke fires repeatedly. Gather what arrives in this window and treat
        # it as one touch, otherwise "coalescing" is just a different word for spam.
        COALESCE_S = 1.2
        # Minimum gap between two touch-triggered replies. Long enough that a child
        # rubbing the robot's head continuously gets one reaction, then a listener.
        COOLDOWN_S = 20.0
        last_reply = 0.0

        while True:
            ev = await device.sensor_events.get()
            seen = {ev}
            deadline = asyncio.get_running_loop().time() + COALESCE_S
            while True:
                remaining = deadline - asyncio.get_running_loop().time()
                if remaining <= 0:
                    break
                try:
                    seen.add(await asyncio.wait_for(
                        device.sensor_events.get(), timeout=remaining))
                except asyncio.TimeoutError:
                    break

            notes = [NOTE[e] for e in ("shaken", "head_pet") if e in seen and e in NOTE]
            if not notes:
                continue

            # Quiet means: not mid-utterance, nothing queued to play, and the person is
            # not currently talking. Anything else and a forced turn would talk over
            # somebody.
            quiet = (not device.speaking and device.tx.empty()
                     and not device.voice_active and not device.barged_in)
            now = asyncio.get_running_loop().time()
            answer = quiet and (now - last_reply) > COOLDOWN_S
            if answer:
                last_reply = now

            try:
                await session.send_client_content(
                    turns=types.Content(role="user",
                                        parts=[types.Part(text=" ".join(notes))]),
                    turn_complete=answer,
                )
                if answer:
                    print("  ✋ → answering the touch", flush=True)
            except Exception as exc:
                print(f"  ⚠ sensor note dropped: {type(exc).__name__}", flush=True)

    async def video_uplink():
        """Camera frames at <= 1 fps, only while the device's VAD hears speech.

        Three constraints shape this, and they all point the same way:

        * Every frame is permanently in the context window. A continuous 1 fps stream
          fills it with pictures of an empty room and pushes the actual conversation out.
        * Capture on the device is genuinely expensive -- a sensor read plus a JPEG
          encode -- and it shares a CPU with the audio pipeline.
        * The moment a picture is worth anything is when someone is talking about what
          they are showing.

        So VAD is the gate, not a timer. Silence costs nothing at all.
        """
        while True:
            await asyncio.sleep(VIDEO_POLL_S)
            if not (device.video_session and device.listening.is_set() and device.voice_active):
                continue

            reply = await device.call("self.camera.capture", {"stream": True}, timeout=20.0)
            if "error" in reply:
                print(f"  ⚠ camera: {reply['error']}", flush=True)
                # Back off rather than hammering a camera that is refusing.
                await asyncio.sleep(3.0)
                continue
            jpeg = extract_jpeg(reply)
            if not jpeg:
                continue

            # Realtime lane is CORRECT here, unlike the one-shot photo. A stream wants
            # audio-clock ordering: frames are continuous context for whatever is being
            # said around them, not a turn that must complete before the model may answer.
            await session.send_realtime_input(
                video=types.Blob(data=jpeg, mime_type="image/jpeg"))
            print(f"  🎥 {len(jpeg)}B", flush=True)

    async def uplink():
        while True:
            pcm = await device.mic.get()
            await session.send_realtime_input(
                audio=types.Blob(data=pcm, mime_type=f"audio/pcm;rate={GEMINI_SEND_RATE}"))

    async def downlink():
        while True:
            async for response in session.receive():
                if response.data:
                    await device.enqueue_audio(response.data)
                    continue

                # The server hands out a resumption handle periodically and refreshes it
                # as the conversation grows. Keeping the newest one is what lets a
                # reconnect continue the same conversation instead of starting a blank
                # one -- which is exactly what you notice after a cable pull.
                if response.session_resumption_update:
                    upd = response.session_resumption_update
                    if upd.resumable and upd.new_handle:
                        device.resumption_handle = upd.new_handle

                # go_away is the server warning us it is about to close (they cap session
                # length). Treat it as a normal reconnect rather than an error: we already
                # hold a resumption handle, so the user should not notice.
                if response.go_away is not None:
                    print(f"↻ server go_away ({response.go_away.time_left}); "
                          f"will resume", flush=True)

                server = response.server_content
                if server and (server.interrupted or server.turn_complete):
                    # The abandoned turn is finally over server-side. This is the only
                    # safe place to unlatch: any earlier and the tail of the very turn
                    # the user dismissed starts playing again.
                    device.barged_in = False

                if server and server.interrupted:
                    # Barge-in: drop what has not gone out yet and close the turn.
                    device.drop_pending_audio()
                    await device.end_turn()
                elif server and server.turn_complete:
                    # Queued behind the audio, so the stop lands after the last frame
                    # rather than truncating playback.
                    await device.end_turn()

                if response.tool_call:
                    responses = [await handle_tool_call(device, c, session)
                                 for c in response.tool_call.function_calls]
                    await session.send_tool_response(function_responses=responses)

                    # Now that the call is resolved, deliver the photo as its own
                    # complete turn. Order matters and is why this is not done inside
                    # handle_tool_call.
                    if device.pending_photo is not None:
                        jpeg, device.pending_photo = device.pending_photo, None
                        await session.send_client_content(
                            turns=types.Content(
                                role="user",
                                parts=[
                                    types.Part(inline_data=types.Blob(
                                        data=jpeg, mime_type="image/jpeg")),
                                    types.Part(text="This is the photo you just took "
                                                    "with your camera. Describe what you "
                                                    "actually see in it."),
                                ],
                            ),
                            turn_complete=True,
                        )

    async def watch_mode():
        """End the session if the device switches between audio-only and video.

        The tool set is decided when the session OPENS, so a session started by a face
        tap keeps take_photo for its whole life -- including after the camera button is
        pressed and frames are already streaming. Observed exactly that: the model was
        receiving live frames and still called take_photo, because the declaration was
        still on the menu from ten minutes earlier.

        Returning here unwinds converse(), and run_session reopens with the right tools.
        The resumption handle carries the conversation across, so the swap costs a
        reconnect and not the context.
        """
        opened_with = device.video_session
        while device.video_session == opened_with:
            await asyncio.sleep(0.25)
        print(f"↻ mode changed to {'camera + mic' if device.video_session else 'mic only'};"
              f" reopening session with matching tools", flush=True)

    # First task to finish ends the session -- watch_mode returning is the signal.
    done, pending = await asyncio.wait(
        [asyncio.create_task(t) for t in
         (uplink(), downlink(), device.pace(), video_uplink(),
          sensor_uplink(), abort_uplink(), watch_mode())],
        return_when=asyncio.FIRST_COMPLETED,
    )
    for t in pending:
        t.cancel()
    for t in done:
        if t.exception() is not None:
            raise t.exception()


async def main():
    load_env(ROOT)
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("STACKCHAN_HOST", "192.168.7.1"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("STACKCHAN_PORT", 8081)))
    ap.add_argument("--model", default=os.environ.get(
        "GEMINI_LIVE_MODEL", "models/gemini-3.1-flash-live-preview"))
    ap.add_argument("--voice", default=os.environ.get("GEMINI_VOICE", "Charon"))
    args = ap.parse_args()

    # Module-level because the resampler reads it per sample and an attribute lookup
    # per sample is not free in pure Python. Set once, before any audio flows.
    global OUTPUT_GAIN, AGC_ENABLED
    try:
        OUTPUT_GAIN = float(os.environ.get("GEMINI_API_OUTPUT_GAIN", "1.0"))
    except ValueError:
        sys.exit("GEMINI_API_OUTPUT_GAIN must be a number, e.g. 0.8")
    AGC_ENABLED = os.environ.get("GEMINI_API_AGC", "1").lower() not in (
        "0", "false", "no", "off", "")

    api_key = os.environ.get("GEMINI_API_KEY")
    if not api_key:
        sys.exit("GEMINI_API_KEY is not set. Put it in .env.local (see .env).")

    client = genai.Client(api_key=api_key, http_options={"api_version": "v1beta"})
    config = types.LiveConnectConfig(
        response_modalities=["AUDIO"],
        speech_config=types.SpeechConfig(voice_config=types.VoiceConfig(
            prebuilt_voice_config=types.PrebuiltVoiceConfig(voice_name=args.voice))),
        system_instruction=os.environ.get("STACKCHAN_SYSTEM_PROMPT"),
        # Not TOOLS: the initial session must agree with what watch_mode() rebuilds on
        # every mode change, or grounding would silently appear only after the first
        # camera-button press.
        tools=tools_for(False),
    )

    url = f"ws://{args.host}:{args.port}/ws"
    await supervise(url, client, config, args)


async def supervise(url, client, config, args):
    """Outer loop: keep a device link alive forever, whatever happens to it.

    This process is meant to run unattended behind an SSH tunnel, so *exiting is
    always the wrong answer*. Every failure below is expected in normal operation,
    not exceptional: the device reboots after a flash, the laptop is unplugged and
    replugged, the tunnel dies and comes back, the model session is rejected.

    Backoff is exponential with jitter and a 30 s cap. The jitter is not decoration --
    without it, a device and a cloud-side client that restart together retry in
    lockstep forever.
    """
    backoff = 0.5
    first = True
    # Outlives every Device instance on purpose. A pulled cable destroys the transport,
    # not the conversation -- carrying the handle across reconnects is what turns
    # "the old convo died, a fresh one started" into simply picking up where we left off.
    handle = None
    handle_at = 0.0          # monotonic time the handle was last rescued
    while True:
        device = None
        try:
            if first:
                print(f"connecting to {url} …", flush=True)
            device = await Device(url).connect()
            if handle is not None and time.monotonic() - handle_at > RESUME_WINDOW_S:
                age = time.monotonic() - handle_at
                print(f"↻ dropping resumption handle ({age:.0f}s old); starting fresh",
                      flush=True)
                handle = None
            device.resumption_handle = handle
            backoff = 0.5                      # a good connection clears the penalty
            first = False
            print("connected. Tap StackChan's face to start talking. Ctrl-C to quit.",
                  flush=True)
            await run_link(device, client, config, args)
        except asyncio.CancelledError:
            raise
        except BaseExceptionGroup as eg:
            # TaskGroup wraps everything; report the first cause, not the group.
            exc = eg.exceptions[0]
            print(f"✗ device link lost: {type(exc).__name__}: {exc}", flush=True)
        except Exception as exc:
            # Connect itself failed -- device rebooting, tunnel down, nothing listening.
            print(f"✗ device unreachable: {type(exc).__name__}: {exc}", flush=True)
        finally:
            if device is not None:
                # Rescue the handle before the Device goes away with it, and stamp it so
                # it can age out rather than living forever.
                if device.resumption_handle:
                    handle = device.resumption_handle
                handle_at = time.monotonic()
                await device.close()

        delay = backoff * (1.0 + random.random() * 0.3)
        print(f"… reconnecting in {delay:.1f}s", flush=True)
        await asyncio.sleep(delay)
        backoff = min(backoff * 2, 30.0)


async def run_link(device, client, config, args):
    """One device connection's lifetime. Returns/raises when it ends."""
    async with asyncio.TaskGroup() as tg:
        tg.create_task(device.pump())

        async def run_session():
            # One rejected frame used to take the whole client down with it, which meant
            # a restart (and another tap) for every hiccup. Reconnect instead: the device
            # link is untouched by a Gemini-side failure, so there is nothing to rebuild
            # but the model session.
            while True:
                await device.listening.wait()
                try:
                    # video_session is already known here: the session only opens once the
                    # device's listen message has arrived, and that message is what
                    # carries the flag. So the tool set can be decided per session rather
                    # than guessed up front.
                    attempt = config.model_copy(update={
                        "session_resumption": types.SessionResumptionConfig(
                            handle=device.resumption_handle),
                        "tools": tools_for(device.video_session)})
                    resumed = device.resumption_handle is not None
                    async with client.aio.live.connect(model=args.model,
                                                       config=attempt) as session:
                        print(f"▶ Gemini Live session {'resumed' if resumed else 'open'}"
                              f" ({args.model}, voice {args.voice}"
                              f"{', camera streaming' if device.video_session else ''}"
                              f"{', search grounding' if grounding_enabled() else ''})",
                              flush=True)
                        await converse(device, session)
                except asyncio.CancelledError:
                    raise
                except Exception as exc:
                    print(f"✗ session ended: {type(exc).__name__}: {exc}", flush=True)
                    device.drop_pending_audio()
                    await asyncio.sleep(2.0)
                    print("… reconnecting; tap again if it stays quiet", flush=True)

        tg.create_task(run_session())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nbye")
    except OSError as exc:
        # TaskGroup raises ExceptionGroup, so unwrap before blaming the network.
        sys.exit(f"could not reach the device: {exc}\nsee TESTING.md 4")
    except ExceptionGroup as eg:
        first = eg.exceptions[0]
        if isinstance(first, OSError):
            sys.exit(f"could not reach the device: {first}\nsee TESTING.md 4")
        raise
