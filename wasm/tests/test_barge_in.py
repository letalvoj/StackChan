"""A face tap while the robot is speaking must stop it dead.

The firmware has always sent this: tapping during `speaking` runs AbortSpeaking, which
emits {"type":"abort"} (protocol.cc). The client simply had no branch for it, so the
frame arrived and was dropped -- which is why a response had to be waited out in full.

What makes this worth a test rather than a one-line handler is that clearing the queue
is NOT sufficient. Gemini has already generated the rest of the turn and keeps
delivering it for seconds afterwards, so without a latch the robot goes quiet for a
moment and then carries on talking. That failure looks like a flaky interrupt rather
than a missing one, and it is invisible unless something checks that audio arriving
*after* the tap is discarded too.

The device half needs a physical finger, so these drive Device.pump directly with a
fake socket and assert on what it does.
"""

import asyncio
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "clients"))

from audio_codec import for_format  # noqa: E402
from gemini_live import GEMINI_RECEIVE_RATE, Device, Downsampler  # noqa: E402


class FakeWS:
    """Yields the given frames to pump(), records anything sent back."""

    def __init__(self, incoming):
        self._incoming = list(incoming)
        self.sent = []

    def __aiter__(self):
        async def gen():
            for m in self._incoming:
                yield m
        return gen()

    async def send(self, msg):
        self.sent.append(msg)

    async def close(self):
        pass


def make_device(incoming):
    d = Device("ws://test")
    d.ws = FakeWS(incoming)
    d.codec = for_format("pcm", 16000, 60)
    # connect() normally builds this once the codec is negotiated.
    d._down = Downsampler(GEMINI_RECEIVE_RATE, d.codec.sample_rate)
    return d


def abort_frame():
    return json.dumps({"type": "abort"})


def test_abort_drains_queued_audio_and_closes_the_turn():
    d = make_device([abort_frame()])
    d.speaking = True
    for _ in range(40):                       # ~2.4 s of pre-rolled speech
        d.tx.put_nowait(b"frame")

    asyncio.run(d.pump())

    assert d.barged_in, "the latch is what stops the rest of the turn playing"
    # Everything unsent is gone, and the end-of-turn marker is the only thing left --
    # pace() owns the tts:stop message, so barge_in must not send one itself.
    assert [d.tx.get_nowait() for _ in range(d.tx.qsize())] == [None]
    assert not any("tts" in s for s in d.ws.sent), \
        "barge_in must not write tts frames directly; pace() owns that message"
    assert d.aborts.get_nowait() == "user"


def test_audio_after_the_tap_is_discarded():
    """The half that clearing the queue does not cover."""
    async def go():
        d = make_device([abort_frame()])
        d.speaking = True
        d.tx.put_nowait(b"frame")
        await d.pump()

        # Gemini keeps delivering the abandoned turn. None of it may reach the device.
        await d.enqueue_audio(b"\x00\x01" * 5000)
        assert d.tx.qsize() == 1 and d.tx.get_nowait() is None

        # The server reporting the turn over is the only safe moment to unlatch; that
        # happens in downlink(). After it, audio flows again.
        d.barged_in = False
        await d.enqueue_audio(b"\x00\x01" * 5000)
        assert d.tx.qsize() > 0, "playback must resume once the abandoned turn has ended"

    asyncio.run(go())


def test_abort_while_already_idle_is_a_no_op():
    """A tap that starts a turn rather than interrupting one must not latch.

    Same frame, opposite meaning depending on state -- and latching here would mute
    the reply the tap was asking for.
    """
    d = make_device([abort_frame()])
    d.speaking = False

    asyncio.run(d.pump())

    assert not d.barged_in
    assert d.tx.qsize() == 0
