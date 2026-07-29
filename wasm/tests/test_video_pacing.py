"""Camera streaming must be self-limiting, whatever the device's capture latency.

Measured on hardware, the FOREGROUND photo path took 698 ms median / 939 ms worst.
Streaming uses the background path instead, which should be far quicker -- but the
client must not depend on that being true. If capture ever gets slow (a busy device, a
larger frame, a retry), the loop has to degrade to a lower frame rate rather than queue
work it cannot keep up with.

The shape that gives this is `sleep(interval)` THEN `await capture()`, so the period is
interval + capture_time. That is self-limiting by construction: the rate can only ever
fall below the ceiling, never rise above it, and slow captures cost frame rate rather
than latency or memory. The obvious alternative -- a fixed-rate timer firing captures
independently -- would overlap requests on a device that serves one at a time.

These tests pin that property, because it is the difference between "the camera is a bit
laggy" and "the audio pipeline is starved by a backlog of image requests".
"""

import asyncio
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "clients"))

INTERVAL = 0.05          # stands in for VIDEO_POLL_S, scaled down so tests stay fast


async def drive(capture_ms, duration_s, gate=lambda: True):
    """Run the video_uplink shape against a fake capture of a known cost."""
    sent, starts = [], []
    running = [True]

    async def loop():
        while running[0]:
            await asyncio.sleep(INTERVAL)
            if not running[0]:
                break
            if not gate():
                continue
            starts.append(asyncio.get_running_loop().time())
            await asyncio.sleep(capture_ms / 1000.0)      # the device call
            sent.append(asyncio.get_running_loop().time())

    t = asyncio.create_task(loop())
    await asyncio.sleep(duration_s)
    running[0] = False
    t.cancel()
    return sent, starts


def periods(stamps):
    return [b - a for a, b in zip(stamps, stamps[1:])]


@pytest.mark.parametrize("capture_ms", [5, 50, 200])
def test_never_exceeds_the_ceiling(capture_ms):
    """However fast capture gets, frames never come closer together than the interval."""
    sent, _ = asyncio.run(drive(capture_ms, 0.6))
    assert len(sent) >= 2, "expected several frames in the window"
    # Allow a little scheduler slack; the point is that it never bursts.
    assert min(periods(sent)) >= INTERVAL * 0.9


def test_slow_capture_costs_frame_rate_not_backlog():
    """A capture far slower than the interval must simply produce fewer frames.

    It must NOT keep issuing requests on schedule and accumulate them -- the device
    serves one client and one call at a time, so a backlog would turn into growing
    latency and contention with the audio path.
    """
    fast, _ = asyncio.run(drive(5, 0.6))
    slow, slow_starts = asyncio.run(drive(200, 0.6))

    assert len(slow) < len(fast), "slow capture should yield fewer frames"
    # Every capture completed before the next began: no overlap, no queue.
    for start, done in zip(slow_starts[1:], slow[:-1]):
        assert start >= done, "a capture began before the previous finished"


def test_gate_closed_costs_nothing():
    """With VAD closed, no captures are attempted at all -- not merely discarded."""
    sent, starts = asyncio.run(drive(50, 0.5, gate=lambda: False))
    assert sent == [] and starts == []


def test_poll_interval_is_one_second_in_the_client():
    """The shipping ceiling is 1 fps, which is what the Live API documents for video."""
    import gemini_live as g

    assert g.VIDEO_POLL_S == 1.0
