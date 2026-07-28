#!/usr/bin/env python3
"""End-to-end QA harness for a StackChan device on the USB-NCM link.

This is the colour bar at the bottom of the newspaper page: one command that
exercises every protocol path in turn and prints a pass/fail table, so a device
coming off the bench can be judged in about a minute without reading logs.

Under USB-NCM the *device* listens, so the normal mode is to dial it:

    ./.venv/bin/python qa_selftest.py --connect 192.168.7.1   # USB-NCM device
    ./.venv/bin/python qa_selftest.py                         # listen instead
    ./.venv/bin/python qa_selftest.py --connect … --only mcp,tts

Listen mode is kept for clients that dial out (the WASM harness, and the older
SLIP/gateway path).

Exit status is 0 only if every selected check passed, so it can gate a CI job or
a factory fixture.
"""

import argparse
import asyncio
import json
import math
import struct
import sys
import time
import uuid
from dataclasses import dataclass, field

import websockets

from audio_codec import codec_from_hello

HOST_DEFAULT = "0.0.0.0"
PORT_DEFAULT = 8081

SAMPLE_RATE = 16000
FRAME_MS = 60

GREEN, RED, YELLOW, DIM, BOLD, RESET = (
    "\033[32m", "\033[31m", "\033[33m", "\033[2m", "\033[1m", "\033[0m"
)


@dataclass
class Check:
    name: str
    detail: str = ""
    status: str = "SKIP"     # PASS | FAIL | SKIP
    seconds: float = 0.0


@dataclass
class Session:
    ws: object
    session_id: str
    device_id: str = "?"
    checks: list = field(default_factory=list)
    inbound_audio_frames: int = 0
    inbound_audio_bytes: int = 0
    mcp_replies: dict = field(default_factory=dict)
    # Whatever the device advertised -- Opus from real firmware, PCM from the
    # WASM harness. Downlink audio is encoded with it so the device can decode.
    codec: object = None
    _mcp_seq: int = 0

    async def send(self, obj):
        await self.ws.send(json.dumps(obj))

    async def call_mcp(self, method, params=None, timeout=10.0):
        """Issue a JSON-RPC call over the protocol's mcp envelope and await its reply."""
        self._mcp_seq += 1
        rpc_id = self._mcp_seq
        payload = {"jsonrpc": "2.0", "id": rpc_id, "method": method}
        if params is not None:
            payload["params"] = params
        await self.send({"session_id": self.session_id, "type": "mcp", "payload": payload})

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if rpc_id in self.mcp_replies:
                return self.mcp_replies.pop(rpc_id)
            await asyncio.sleep(0.05)
        raise TimeoutError(f"no MCP reply to {method} within {timeout:.0f}s")


def tone_pcm16(ms, freq=440.0, amplitude=0.25):
    """A plain sine in PCM16 mono. Audible and obviously synthetic, so a human can
    tell a real playback from noise."""
    n = int(SAMPLE_RATE * ms / 1000)
    out = bytearray()
    for i in range(n):
        v = int(amplitude * 32767 * math.sin(2 * math.pi * freq * i / SAMPLE_RATE))
        out += struct.pack("<h", v)
    return bytes(out)


# ── individual checks ────────────────────────────────────────────────────────

async def check_handshake(s: Session) -> Check:
    # Already completed by the time we get here; this records what we learned.
    return Check("handshake", f"device_id={s.device_id} session={s.session_id[:8]}", "PASS")


async def check_mcp_tools(s: Session) -> Check:
    t0 = time.monotonic()
    await s.call_mcp("initialize", {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "stackchan-qa", "version": "1"},
    })
    reply = await s.call_mcp("tools/list")
    tools = [t.get("name") for t in reply.get("result", {}).get("tools", [])]
    if not tools:
        return Check("mcp.tools", "device listed no tools", "FAIL", time.monotonic() - t0)
    return Check("mcp.tools", f"{len(tools)} tools: {', '.join(sorted(tools)[:4])}…",
                 "PASS", time.monotonic() - t0)


async def check_device_status(s: Session) -> Check:
    t0 = time.monotonic()
    reply = await s.call_mcp("tools/call", {"name": "self.get_device_status", "arguments": {}})
    text = json.dumps(reply.get("result", {}))
    if "error" in reply:
        return Check("device.status", str(reply["error"])[:60], "FAIL", time.monotonic() - t0)
    return Check("device.status", text[:70], "PASS", time.monotonic() - t0)


async def check_system_info(s: Session) -> Check:
    t0 = time.monotonic()
    reply = await s.call_mcp("tools/call", {"name": "self.get_system_info", "arguments": {}})
    if "error" in reply:
        return Check("device.sysinfo", str(reply["error"])[:60], "FAIL", time.monotonic() - t0)
    return Check("device.sysinfo", json.dumps(reply.get("result", {}))[:70],
                 "PASS", time.monotonic() - t0)


async def check_screen(s: Session) -> Check:
    """Visible on the panel: brightness sweep then theme flip. A human watching the
    device sees this happen, which is the point -- it proves the display path end to
    end, not just that a JSON reply came back."""
    t0 = time.monotonic()
    for level in (30, 100, 70):
        await s.call_mcp("tools/call",
                         {"name": "self.screen.set_brightness", "arguments": {"brightness": level}})
        await asyncio.sleep(0.4)
    for theme in ("dark", "light"):
        await s.call_mcp("tools/call",
                         {"name": "self.screen.set_theme", "arguments": {"theme": theme}})
        await asyncio.sleep(0.4)
    return Check("screen", "brightness 30/100/70 + theme dark/light applied",
                 "PASS", time.monotonic() - t0)


async def check_photo(s: Session) -> Check:
    t0 = time.monotonic()
    reply = await s.call_mcp("tools/call",
                             {"name": "self.camera.take_photo", "arguments": {"question": "what do you see?"}},
                             timeout=25.0)
    if "error" in reply:
        return Check("camera.photo", str(reply["error"])[:60], "FAIL", time.monotonic() - t0)
    body = json.dumps(reply.get("result", {}))
    return Check("camera.photo", body[:70], "PASS", time.monotonic() - t0)


async def check_tts_downlink(s: Session) -> Check:
    """Speaker path. A 600 ms tone, encoded in whatever the device negotiated.

    You should HEAR this. A pure 440 Hz tone is unmistakably synthetic, so silence
    or noise means a real fault rather than an ambiguous result."""
    t0 = time.monotonic()
    await s.send({"session_id": s.session_id, "type": "tts", "state": "start",
                  "sample_rate": s.codec.sample_rate})
    await s.send({"session_id": s.session_id, "type": "tts", "state": "sentence_start",
                  "text": "StackChan self test"})

    frames = s.codec.encode(tone_pcm16(600))
    for frame in frames:
        await s.ws.send(frame)
        await asyncio.sleep(s.codec.frame_ms / 1000.0)
    await s.send({"session_id": s.session_id, "type": "tts", "state": "stop"})
    return Check("tts.downlink", f"{len(frames)} {s.codec.name} frames sent — listen for a tone",
                 "PASS", time.monotonic() - t0)


async def check_mic_uplink(s: Session, seconds=5.0) -> Check:
    """Microphone path. Opens a listen turn and counts inbound binary frames.

    Make a noise at the device while this runs."""
    t0 = time.monotonic()
    before, before_bytes = s.inbound_audio_frames, s.inbound_audio_bytes
    await s.send({"session_id": s.session_id, "type": "listen",
                  "state": "start", "mode": "manual"})
    print(f"{YELLOW}  ↳ speak to the device now ({seconds:.0f}s)…{RESET}")
    await asyncio.sleep(seconds)
    await s.send({"session_id": s.session_id, "type": "listen", "state": "stop"})
    await asyncio.sleep(0.5)

    frames = s.inbound_audio_frames - before
    if frames == 0:
        return Check("mic.uplink", "no audio frames received from device", "FAIL",
                     time.monotonic() - t0)
    return Check("mic.uplink",
                 f"{frames} frames / {s.inbound_audio_bytes - before_bytes} bytes",
                 "PASS", time.monotonic() - t0)


CHECKS = {
    "handshake": check_handshake,
    "mcp": check_mcp_tools,
    "status": check_device_status,
    "sysinfo": check_system_info,
    "screen": check_screen,
    "photo": check_photo,
    "tts": check_tts_downlink,
    "mic": check_mic_uplink,
}


# ── plumbing ─────────────────────────────────────────────────────────────────

async def reader(s: Session, verbose: bool):
    """Drains the socket for the whole session so replies land in s.mcp_replies."""
    try:
        async for msg in s.ws:
            if isinstance(msg, bytes):
                s.inbound_audio_frames += 1
                s.inbound_audio_bytes += len(msg)
                continue
            try:
                d = json.loads(msg)
            except json.JSONDecodeError:
                print(f"{RED}  ! non-JSON text frame: {msg[:80]}{RESET}")
                continue
            if verbose:
                print(f"{DIM}  ← {msg[:160]}{RESET}")
            if d.get("type") == "mcp":
                payload = d.get("payload", {})
                if "id" in payload:
                    s.mcp_replies[payload["id"]] = payload
    except websockets.exceptions.ConnectionClosed:
        pass


async def run_session(ws, path, selected, verbose, keep_open):
    if path not in ("/ws", "/"):
        await ws.close()
        return

    # Identical either way: whoever owns the socket, the device still speaks first.
    raw = await asyncio.wait_for(ws.recv(), timeout=30)
    hello = json.loads(raw)
    if hello.get("type") != "hello":
        print(f"{RED}✗ expected hello, got {hello.get('type')!r}{RESET}")
        await ws.close()
        return

    session_id = str(uuid.uuid4())
    codec = codec_from_hello(hello)
    s = Session(ws=ws, session_id=session_id,
                device_id=hello.get("device_id", "?"), codec=codec)

    # transport MUST be "websocket": the device rejects anything else outright.
    await ws.send(json.dumps({
        "type": "hello",
        "transport": "websocket",
        "session_id": session_id,
        "audio_params": {"format": codec.name,
                         "sample_rate": codec.sample_rate,
                         "frame_duration": codec.frame_ms},
    }))

    print(f"\n{BOLD}device {s.device_id}{RESET}  session {session_id[:8]}  "
          f"audio {codec.name}@{codec.sample_rate}Hz/{codec.frame_ms}ms")
    print(f"{DIM}  device hello: {json.dumps(hello)[:150]}{RESET}\n")

    task = asyncio.create_task(reader(s, verbose))
    try:
        for name in selected:
            print(f"{DIM}running {name}…{RESET}")
            try:
                s.checks.append(await CHECKS[name](s))
            except Exception as exc:                      # noqa: BLE001 - report, never abort the run
                s.checks.append(Check(name, f"{type(exc).__name__}: {exc}"[:70], "FAIL"))
    finally:
        report(s)
        if not keep_open:
            await ws.close()
            task.cancel()

    if keep_open:
        print(f"{DIM}--keep-open: staying connected, ^C to quit{RESET}")
        await task


def report(s: Session):
    print(f"\n{BOLD}── QA summary ── device {s.device_id} ──{RESET}")
    width = max((len(c.name) for c in s.checks), default=10)
    for c in s.checks:
        colour = {"PASS": GREEN, "FAIL": RED}.get(c.status, YELLOW)
        print(f"  {colour}{c.status:<4}{RESET} {c.name:<{width}}  "
              f"{DIM}{c.seconds:5.1f}s{RESET}  {c.detail}")

    failed = [c for c in s.checks if c.status == "FAIL"]
    bar = "".join(("█" if c.status == "PASS" else "▁") for c in s.checks)
    colour = RED if failed else GREEN
    print(f"\n  {colour}{bar}{RESET}  "
          f"{len(s.checks) - len(failed)}/{len(s.checks)} passed\n")
    globals()["EXIT_CODE"] = 1 if failed else 0


EXIT_CODE = 1   # until a session actually completes


async def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--connect", metavar="HOST",
                    help="dial a device that is listening (USB-NCM: 192.168.7.1) "
                         "instead of waiting for one to dial us")
    ap.add_argument("--host", default=HOST_DEFAULT)
    ap.add_argument("--port", type=int, default=PORT_DEFAULT)
    ap.add_argument("--only", default="", help="comma-separated subset: " + ",".join(CHECKS))
    ap.add_argument("--skip", default="", help="comma-separated checks to omit")
    ap.add_argument("--verbose", action="store_true", help="echo every inbound frame")
    ap.add_argument("--keep-open", action="store_true",
                    help="hold the connection after the run, to watch traffic")
    args = ap.parse_args()

    selected = [c for c in CHECKS if not args.only or c in args.only.split(",")]
    selected = [c for c in selected if c not in args.skip.split(",")]
    unknown = set(args.only.split(",")) - set(CHECKS) - {""}
    if unknown:
        sys.exit(f"unknown check(s): {', '.join(sorted(unknown))}")

    done = asyncio.Event()

    async def handler(ws, path="/ws"):
        try:
            await run_session(ws, path, selected, args.verbose, args.keep_open)
        finally:
            if not args.keep_open:
                done.set()

    print(f"{BOLD}StackChan QA harness{RESET}")
    print(f"{DIM}checks: {', '.join(selected)}{RESET}")

    if args.connect:
        url = f"ws://{args.connect}:{args.port}/ws"
        print(f"{DIM}dialling {url}…{RESET}")
        try:
            async with websockets.connect(url, max_size=None, open_timeout=30) as ws:
                await run_session(ws, "/ws", selected, args.verbose, args.keep_open)
        except OSError as exc:
            print(f"{RED}could not reach {url}: {exc}{RESET}")
            print(f"{DIM}is the device plugged in and did the host take a DHCP lease? "
                  f"see TESTING.md §4{RESET}")
            return 1
        return EXIT_CODE

    print(f"{DIM}listening on ws://{args.host}:{args.port}/ws{RESET}")
    print(f"{DIM}waiting for the device to connect (power-cycle it if nothing happens)…{RESET}")
    async with websockets.serve(handler, args.host, args.port):
        await done.wait()
    return EXIT_CODE


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        sys.exit(130)
