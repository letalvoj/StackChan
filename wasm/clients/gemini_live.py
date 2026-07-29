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
import os
import random
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

def resample(pcm: bytes, src_rate: int, dst_rate: int) -> bytes:
    """Linear-interpolating resampler over PCM16 mono.

    Hand-rolled because `audioop` was removed in Python 3.13 and pulling numpy or
    soxr in for one downsample is not worth the install. Linear interpolation
    aliases above Nyquist, which for 24k -> 16k speech is inaudible next to Opus
    at this bitrate; revisit if the ratio ever gets aggressive.
    """
    if src_rate == dst_rate or not pcm:
        return pcm

    src = memoryview(pcm).cast("h")
    n_out = int(len(src) * dst_rate / src_rate)
    step = len(src) / n_out if n_out else 0
    out = bytearray(n_out * 2)
    view = memoryview(out).cast("h")
    for i in range(n_out):
        pos = i * step
        j = int(pos)
        if j + 1 < len(src):
            frac = pos - j
            view[i] = int(src[j] * (1.0 - frac) + src[j + 1] * frac)
        else:
            view[i] = src[j] if j < len(src) else 0
    return bytes(out)


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
                "Yaw -128..128 (negative is your left), pitch 0..90 (90 is up). "
                "Stay within +/-45 yaw for normal conversation. Speed 100-1000, "
                "150 is natural, 700 is excited."
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
        return TOOLS
    kept = [f for f in TOOLS[0].function_declarations if f.name != "take_photo"]
    return [types.Tool(function_declarations=kept)]


# Gemini's flat tool names -> the device's namespaced MCP names.
MCP_NAMES = {
    "set_head_angles": "self.robot.set_head_angles",
    "get_head_angles": "self.robot.get_head_angles",
    # Our own tool, registered in hal_mcp.cpp. NOT upstream's self.camera.take_photo,
    # which POSTs the frame to a remote VLM and returns prose about it.
    "take_photo": "self.camera.capture",
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
        # A JPEG waiting to be sent as its own turn, once the tool call that produced
        # it has been answered. See handle_tool_call.
        self.pending_photo = None
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
        self._pcm_residual += pcm24
        step = self._src_bytes_per_frame()
        n = len(self._pcm_residual) // step
        if n == 0:
            return
        whole, self._pcm_residual = self._pcm_residual[:n * step], self._pcm_residual[n * step:]

        def work():
            pcm = resample(whole, GEMINI_RECEIVE_RATE, self.codec.sample_rate)
            return self.codec.encode(pcm)

        for frame in await asyncio.to_thread(work):
            self.tx.put_nowait(frame)

    async def flush_audio(self):
        """End of turn: emit the tail, padded. Padding is correct HERE and only here."""
        if not self._pcm_residual:
            return
        tail, self._pcm_residual = self._pcm_residual, b""

        def work():
            pcm = resample(tail, GEMINI_RECEIVE_RATE, self.codec.sample_rate)
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
        speaking = False
        deadline = None
        while True:
            # Pre-roll before the first frame of a turn. The device plays in hard real
            # time, so if the queue ever runs dry mid-utterance it underruns and clicks.
            # Gemini delivers in bursts, and starting on the very first frame means the
            # first network hiccup is audible. kPreroll frames of head start costs that
            # much latency once per turn and absorbs the jitter for the rest of it.
            if not speaking and self.tx.qsize() < PREROLL_FRAMES:
                try:
                    await asyncio.wait_for(asyncio.shield(self._preroll_wait()), timeout=0.4)
                except asyncio.TimeoutError:
                    pass          # short utterance: play what we have rather than stall

            frame = await self.tx.get()

            if frame is None:                     # end-of-turn marker
                if speaking:
                    await self.ws.send(json.dumps({"session_id": self.session_id,
                                                   "type": "tts", "state": "stop"}))
                    speaking = False
                deadline = None
                continue

            if not speaking:
                await self.ws.send(json.dumps({
                    "session_id": self.session_id, "type": "tts", "state": "start",
                    "sample_rate": self.codec.sample_rate}))
                speaking = True
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
    if mcp_name is None:
        return types.FunctionResponse(id=call.id, name=call.name,
                                      response={"error": f"unknown tool {call.name}"})

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

    await asyncio.gather(uplink(), downlink(), device.pace(), video_uplink())


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

    api_key = os.environ.get("GEMINI_API_KEY")
    if not api_key:
        sys.exit("GEMINI_API_KEY is not set. Put it in .env.local (see .env).")

    client = genai.Client(api_key=api_key, http_options={"api_version": "v1beta"})
    config = types.LiveConnectConfig(
        response_modalities=["AUDIO"],
        speech_config=types.SpeechConfig(voice_config=types.VoiceConfig(
            prebuilt_voice_config=types.PrebuiltVoiceConfig(voice_name=args.voice))),
        system_instruction=os.environ.get("STACKCHAN_SYSTEM_PROMPT"),
        tools=TOOLS,
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
                              f"{', camera streaming' if device.video_session else ''})",
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
