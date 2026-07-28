"""Hardware: the device serves exactly one client, and says so out loud.

Skipped unless a real StackChan is reachable over USB-NCM, so it stays out of the
way in CI and runs when the robot is on the desk.

    cd wasm && ./.venv/bin/python -m pytest tests -q

This covers a bug that took several flash cycles to find by hand. The firmware
adopts the client socket on every inbound frame, which means two connected hosts
would quietly trade ownership of the send path: request/response still looked
fine, because the requester is whoever most recently touched the handler, but
device-originated frames (audio, TTS state) have no requester and followed
whoever spoke last. A live Opus stream could be stolen mid-utterance by another
tool merely polling get_device_status.

The firmware now closes the previous socket when it adopts a new one. The
regression these tests lock down is the *observable* part of that: the loser must
be hung up on, not left connected and deaf. A silently deaf socket is the failure
mode that cost the debugging time, because every client library reports a close
and none of them report being ignored.
"""

import asyncio
import json
import uuid

import pytest
import websockets

DEVICE_URL = "ws://192.168.7.1:8081/ws"
SR, FRAME_MS = 16000, 60

HELLO = {
    "type": "hello",
    "transport": "websocket",
    "audio_params": {"format": "opus", "sample_rate": SR, "frame_duration": FRAME_MS},
}


async def _reachable() -> bool:
    try:
        async with websockets.connect(DEVICE_URL, open_timeout=3):
            return True
    except Exception:
        return False


def _needs_device():
    if not asyncio.run(_reachable()):
        pytest.skip("no StackChan on 192.168.7.1 (plug it in over USB)")


async def _connect():
    ws = await websockets.connect(DEVICE_URL, open_timeout=10, max_size=None)
    await ws.send(json.dumps({**HELLO, "session_id": str(uuid.uuid4())}))
    return ws


async def _recv_json(ws, timeout=3.0):
    """Next JSON frame, skipping binary audio."""
    async def pump():
        while True:
            m = await ws.recv()
            if not isinstance(m, bytes):
                return json.loads(m)

    return await asyncio.wait_for(pump(), timeout)


def test_device_greets_on_connect():
    """Device speaks first. This is the whole device->host direction in one assert."""
    _needs_device()

    async def run():
        ws = await _connect()
        try:
            hello = await _recv_json(ws)
            assert hello["type"] == "hello"
            # Proves it is the real firmware and not something echoing us back.
            assert hello["features"]["mcp"] is True
            assert hello["audio_params"]["sample_rate"] == SR
        finally:
            await ws.close()

    asyncio.run(run())


def test_second_client_evicts_the_first():
    """The loser must be CLOSED, not left open and deaf."""
    _needs_device()

    async def run():
        a = await _connect()
        await _recv_json(a)          # a's greeting

        b = await _connect()
        await _recv_json(b)          # b's greeting
        try:
            # A must now be hung up on. Reading is what surfaces it; a send can sit
            # in the local buffer and succeed even after the peer is gone.
            with pytest.raises((websockets.exceptions.ConnectionClosed, asyncio.TimeoutError)):
                await _recv_json(a, timeout=5.0)
            assert a.close_code is not None, "A was left connected but deaf"
        finally:
            await b.close()
            await a.close()

    asyncio.run(run())


def test_surviving_client_still_works_after_an_eviction():
    """Evicting must not damage the session that caused it.

    The subtle failure here is the close_fn for the evicted socket tearing down the
    *new* client's state on its way out -- the two sessions differ only by fd, and
    httpd recycles fd numbers aggressively.
    """
    _needs_device()

    async def run():
        a = await _connect()
        await _recv_json(a)
        b = await _connect()
        await _recv_json(b)
        try:
            await b.send(json.dumps({"type": "mcp", "payload": {
                "jsonrpc": "2.0", "id": 7, "method": "tools/list"}}))
            reply = await _recv_json(b, timeout=5.0)
            assert reply["type"] == "mcp"
            assert reply["payload"]["id"] == 7
            assert reply["payload"]["result"]["tools"], "no tools after eviction"
        finally:
            await b.close()
            await a.close()

    asyncio.run(run())
