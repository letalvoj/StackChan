"""Streaming a continuous signal in ragged chunks must not inject silence.

This covers the bug that made Gemini's speech crackle, which survived two earlier
"fixes" aimed at pacing and threading because neither touched the actual cause.

codec.encode() zero-pads a trailing partial frame. That is correct for a complete
utterance -- Opus rejects short frames, and dropping the tail would clip the end of
every sentence. It is catastrophic when called per streamed chunk: a live model emits
arbitrary-sized chunks, so almost every one ends mid-frame and gets padded with silence
*in the middle of continuous speech*. Measured on a 3 s tone delivered in 26 ragged
chunks, the old path injected 601 ms of silence -- a fifth of the stream.

No amount of buffering or pacing downstream can repair this: the damage is already in
the samples by then. The fix is to carry a residual across chunks and only ever hand
whole frames to the encoder.
"""

import math
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "clients"))

from audio_codec import for_format  # noqa: E402
from gemini_live import Downsampler  # noqa: E402

SRC_RATE = 24000          # what Gemini Live emits
DEV_RATE = 16000          # what the device negotiated
FRAME_MS = 60


def resampler():
    """A fresh resampler, adapted to the (pcm, src, dst) shape these tests pass around.

    Fresh per stream, and never shared: the shipping resampler is an FIR filter with
    memory (Downsampler), so reusing one across two independent signals would leak the
    tail of the first into the head of the second. That statefulness is the point --
    it is what removes the discontinuity at every chunk boundary -- but it makes a
    module-level singleton wrong for tests.
    """
    down = Downsampler(SRC_RATE, DEV_RATE)
    return lambda pcm, src=SRC_RATE, dst=DEV_RATE: down.feed(pcm)


def tone(seconds=3.0, freq=440.0, rate=SRC_RATE):
    n = int(rate * seconds)
    return b"".join(
        struct.pack("<h", int(0.4 * math.sin(2 * math.pi * freq * i / rate) * 32767))
        for i in range(n)
    )


def ragged_chunks(pcm, sizes=(1024, 2048, 3200, 4096, 5000)):
    """Deliberately never aligned to the frame size, like a real model stream."""
    out, off, i = [], 0, 0
    while off < len(pcm):
        take = sizes[i % len(sizes)] * 2
        out.append(pcm[off:off + take])
        off += take
        i += 1
    return out


def silence_samples(pcm, minrun=40):
    """Samples inside runs of >= minrun consecutive zeros, EXCLUDING a trailing run.

    A trailing run is the end-of-turn flush padding the last partial frame, which is
    correct and necessary -- Opus will not accept a short frame. What must never happen
    is silence in the INTERIOR of the stream, which is the bug under test. Counting the
    tail would fail the frame durations that happen not to divide the signal evenly, and
    would be measuring the wrong thing.
    """
    s = memoryview(pcm).cast("h")
    total = cur = 0
    for v in s:
        if v == 0:
            cur += 1
        else:
            if cur >= minrun:
                total += cur
            cur = 0
    return total          # trailing run deliberately not added


def stream_with_residual(chunks, codec, resample):
    """The shipping path: accumulate at source rate, emit only whole frames."""
    src_step = int(SRC_RATE * codec.frame_ms / 1000) * 2
    residual, decoded = b"", b""
    for c in chunks:
        residual += c
        n = len(residual) // src_step
        if n:
            whole, residual = residual[:n * src_step], residual[n * src_step:]
            for f in codec.encode(resample(whole, SRC_RATE, DEV_RATE)):
                decoded += codec.decode(f)
    if residual:                       # flush: padding is correct only here
        for f in codec.encode(resample(residual, SRC_RATE, DEV_RATE)):
            decoded += codec.decode(f)
    return decoded


def test_per_chunk_encoding_injects_silence():
    """The bug itself. If this ever stops failing, the padding behaviour changed."""
    resample = resampler()

    codec = for_format("pcm", DEV_RATE, FRAME_MS)     # lossless, so the count is exact
    chunks = ragged_chunks(tone())

    naive = b""
    for c in chunks:
        for f in codec.encode(resample(c, SRC_RATE, DEV_RATE)):
            naive += codec.decode(f)

    injected = silence_samples(naive)
    assert injected > DEV_RATE * 0.1, (
        "expected the naive per-chunk path to inject substantial silence; "
        "if it no longer does, audio_codec.encode stopped padding and this "
        "test is guarding nothing"
    )


def test_residual_streaming_injects_no_silence():
    """The fix. A continuous tone must stay continuous however it is chunked."""
    codec = for_format("pcm", DEV_RATE, FRAME_MS)
    decoded = stream_with_residual(ragged_chunks(tone()), codec, resampler())

    assert silence_samples(decoded) == 0


def test_residual_streaming_preserves_length():
    """No samples invented, none lost -- within one frame of the resampled original."""
    codec = for_format("pcm", DEV_RATE, FRAME_MS)
    src = tone(seconds=2.0)
    decoded = stream_with_residual(ragged_chunks(src), codec, resampler())

    expected = len(resampler()(src)) // 2
    got = len(decoded) // 2
    assert abs(got - expected) <= codec.samples_per_frame


@pytest.mark.parametrize("frame_ms", [20, 40, 60])
def test_holds_for_every_frame_duration(frame_ms):
    """The device picks frame_duration in its hello; the fix must not assume 60 ms."""
    codec = for_format("pcm", DEV_RATE, frame_ms)
    decoded = stream_with_residual(ragged_chunks(tone(seconds=1.5)), codec, resampler())
    assert silence_samples(decoded) == 0
