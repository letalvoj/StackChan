#!/usr/bin/env python3
"""A fake StackChan that speaks just enough protocol to exercise qa_selftest.py.

Purpose is to test *the harness*, not the firmware: it lets you develop and debug
the QA checks without a board on the desk, and gives a known-good baseline so a
red bar against real hardware means the device, not a bug in the harness.

    ./.venv/bin/python qa_selftest.py --port 8099 &
    ./.venv/bin/python qa_mock_device.py --port 8099

It answers every MCP call with a stub result, so a run against this should be all
green. It deliberately does NOT emulate failure modes -- see TESTING.md §4 for
what real failures look like.
"""

import argparse
import asyncio
import json

import websockets

TOOLS = [
    "self.get_device_status",
    "self.get_system_info",
    "self.camera.take_photo",
    "self.screen.set_brightness",
    "self.screen.set_theme",
    "self.screen.snapshot",
    "self.audio_speaker.set_volume",
]


async def main(url, device_id):
    async with websockets.connect(url) as ws:
        await ws.send(json.dumps({
            "type": "hello", "version": 1, "transport": "websocket",
            "device_id": device_id,
            "audio_params": {"format": "opus", "sample_rate": 16000, "frame_duration": 60},
        }))
        hello = json.loads(await ws.recv())
        session_id = hello["session_id"]
        print(f"mock device {device_id} connected, session {session_id[:8]}")

        listening = False

        async def mic_stream():
            # Stand-in for the uplink: real firmware sends Opus, and the harness only
            # counts frames, so opaque bytes are a faithful enough stimulus.
            while True:
                await asyncio.sleep(0.06)
                if listening:
                    try:
                        await ws.send(b"\x01" * 80)
                    except websockets.exceptions.ConnectionClosed:
                        return

        pump = asyncio.create_task(mic_stream())
        try:
            async for msg in ws:
                if isinstance(msg, bytes):
                    continue                      # downlink audio; nothing to decode here
                d = json.loads(msg)
                kind = d.get("type")

                if kind == "mcp":
                    p = d.get("payload", {})
                    method, rpc_id = p.get("method"), p.get("id")
                    if method == "initialize":
                        result = {"protocolVersion": "2024-11-05",
                                  "serverInfo": {"name": "mock-stackchan"}}
                    elif method == "tools/list":
                        result = {"tools": [{"name": n, "description": "mock"} for n in TOOLS]}
                    else:
                        name = p.get("params", {}).get("name", "?")
                        result = {"content": [{"type": "text", "text": f"mock ok: {name}"}]}
                    await ws.send(json.dumps({"session_id": session_id, "type": "mcp",
                                              "payload": {"jsonrpc": "2.0", "id": rpc_id,
                                                          "result": result}}))
                elif kind == "listen":
                    listening = d.get("state") == "start"
                    print(f"  listen -> {d.get('state')}")
                elif kind == "tts":
                    print(f"  tts -> {d.get('state')}")
        finally:
            pump.cancel()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8081)
    ap.add_argument("--device-id", default="AA:BB:CC:DD:EE:FF")
    a = ap.parse_args()
    try:
        asyncio.run(main(f"ws://{a.host}:{a.port}/ws", a.device_id))
    except KeyboardInterrupt:
        pass
