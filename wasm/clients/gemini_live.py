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
import json
import os
import sys
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
# Deliberately NOT exposed:
#   self.camera.take_photo   -- its firmware handler POSTs the JPEG to a remote VLM
#                               and returns prose. That is the call-home this project
#                               exists to remove, and it is strictly worse than
#                               letting a natively multimodal model see the pixels.
#                               Needs a firmware path that returns the JPEG instead.
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

# Gemini's flat tool names -> the device's namespaced MCP names.
MCP_NAMES = {
    "set_head_angles": "self.robot.set_head_angles",
    "get_head_angles": "self.robot.get_head_angles",
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

    async def connect(self):
        self.ws = await websockets.connect(self.url, open_timeout=20, max_size=None)
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
                    self.listening.set()
                    print("● listening (tap registered)", flush=True)
                elif data.get("state") == "stop":
                    self.listening.clear()
            elif kind == "tts":
                pass
            elif kind == "goodbye":
                self.listening.clear()

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

    async def enqueue_audio(self, pcm24: bytes):
        """Resample + encode OFF the event loop, then queue for paced sending.

        Both steps are CPU-bound: the resampler is pure Python (see resample) and Opus
        encoding is a C call that still blocks. Doing them inline stalls the whole
        client -- including the microphone uplink, since it shares this event loop.
        That made the choppiness bidirectional rather than just an output problem.
        """
        def work():
            pcm = resample(pcm24, GEMINI_RECEIVE_RATE, self.codec.sample_rate)
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

    async def end_turn(self):
        await self.tx.put(None)

    def drop_pending_audio(self):
        """Barge-in: discard queued audio that has not been sent yet."""
        while not self.tx.empty():
            try:
                self.tx.get_nowait()
            except asyncio.QueueEmpty:
                break


# ------------------------------------------------------------------------ bridge

async def handle_tool_call(device: Device, call) -> types.FunctionResponse:
    args = dict(call.args or {})
    mcp_name = MCP_NAMES.get(call.name)
    if mcp_name is None:
        return types.FunctionResponse(id=call.id, name=call.name,
                                      response={"error": f"unknown tool {call.name}"})

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
                    responses = [await handle_tool_call(device, c)
                                 for c in response.tool_call.function_calls]
                    await session.send_tool_response(function_responses=responses)

    await asyncio.gather(uplink(), downlink(), device.pace())


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
    print(f"connecting to {url} …", flush=True)
    device = await Device(url).connect()
    print("connected. Tap StackChan's face to start talking. Ctrl-C to quit.", flush=True)

    async with asyncio.TaskGroup() as tg:
        tg.create_task(device.pump())

        async def run_session():
            await device.listening.wait()
            async with client.aio.live.connect(model=args.model, config=config) as session:
                print(f"▶ Gemini Live session open ({args.model}, voice {args.voice})",
                      flush=True)
                await converse(device, session)

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
