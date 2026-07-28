"""Unit tests for the server-side audio adaptation.

These cover the gap that made audio "pass the tests but sound wrong": the ESP32
speaks Opus, the WASM harness speaks raw PCM, and the server has to serve both
without the backends knowing which is which.

    cd wasm && ./.venv/bin/python -m pytest tests -q
"""

import math
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from audio_codec import (  # noqa: E402
    CodecError,
    OpusCodec,
    PcmCodec,
    codec_from_hello,
    for_format,
    pcm_to_samples,
)

SR = 16000
FRAME_MS = 60


def tone(ms, freq=440.0, amplitude=0.3, sample_rate=SR):
    n = int(sample_rate * ms / 1000)
    return b"".join(
        struct.pack("<h", int(amplitude * 32767 * math.sin(2 * math.pi * freq * i / sample_rate)))
        for i in range(n)
    )


def rms(pcm):
    s = pcm_to_samples(pcm)
    if not s:
        return 0.0
    return math.sqrt(sum(v * v for v in s) / len(s))


# ── format selection ─────────────────────────────────────────────────────────

def test_missing_format_defaults_to_pcm():
    # A missing field must not silently become Opus: that would turn raw PCM into
    # garbage audio instead of an obvious error.
    assert isinstance(for_format(None), PcmCodec)
    assert isinstance(codec_from_hello({}), PcmCodec)


def test_format_is_case_insensitive():
    assert isinstance(for_format("PCM"), PcmCodec)


def test_unknown_format_is_rejected_loudly():
    with pytest.raises(CodecError) as exc:
        for_format("mp3")
    assert "mp3" in str(exc.value)


def test_hello_drives_codec_and_params():
    hello = {
        "type": "hello",
        "audio_params": {"format": "opus", "sample_rate": 16000, "frame_duration": 60},
    }
    codec = codec_from_hello(hello)
    assert isinstance(codec, OpusCodec)
    assert codec.sample_rate == 16000
    assert codec.frame_ms == 60


def test_hello_with_null_params_falls_back():
    # serve.py has seen clients send audio_params: null.
    codec = codec_from_hello({"audio_params": None})
    assert isinstance(codec, PcmCodec)
    assert codec.sample_rate == SR


# ── frame geometry ───────────────────────────────────────────────────────────

@pytest.mark.parametrize("cls", [PcmCodec, OpusCodec])
def test_frame_geometry(cls):
    codec = cls(SR, FRAME_MS)
    assert codec.samples_per_frame == 960        # 60 ms @ 16 kHz
    assert codec.bytes_per_frame == 1920


def test_encode_splits_on_frame_boundaries():
    codec = PcmCodec(SR, FRAME_MS)
    frames = codec.encode(tone(300))             # exactly 5 frames
    assert len(frames) == 5
    assert all(len(f) == codec.bytes_per_frame for f in frames)


def test_partial_tail_is_padded_not_dropped():
    # Dropping the tail clips the end of every utterance, and Opus rejects short
    # frames outright -- so the tail must be padded up to a whole frame.
    codec = PcmCodec(SR, FRAME_MS)
    frames = codec.encode(tone(70))              # one full frame + 10 ms
    assert len(frames) == 2
    assert all(len(f) == codec.bytes_per_frame for f in frames)
    assert frames[1].endswith(b"\x00" * 100)


def test_encoding_empty_audio_yields_nothing():
    assert PcmCodec(SR, FRAME_MS).encode(b"") == []


# ── round trips ──────────────────────────────────────────────────────────────

def test_pcm_round_trip_is_lossless():
    codec = PcmCodec(SR, FRAME_MS)
    pcm = tone(300)
    assert b"".join(codec.decode(f) for f in codec.encode(pcm)) == pcm


def test_opus_round_trip_preserves_length_and_signal():
    codec = OpusCodec(SR, FRAME_MS)
    pcm = tone(300)
    frames = codec.encode(pcm)

    # Opus is lossy, so assert on structure and energy rather than bytes.
    assert len(frames) == 5
    assert all(0 < len(f) < codec.bytes_per_frame for f in frames), "should compress"

    out = b"".join(codec.decode(f) for f in frames)
    assert len(out) == len(pcm)

    # The first frame is codec warm-up and legitimately quiet; compare the rest.
    assert rms(out[codec.bytes_per_frame:]) == pytest.approx(
        rms(pcm[codec.bytes_per_frame:]), rel=0.35
    )


def test_opus_silence_stays_silent():
    codec = OpusCodec(SR, FRAME_MS)
    silence = b"\x00" * (codec.bytes_per_frame * 3)
    out = b"".join(codec.decode(f) for f in codec.encode(silence))
    assert len(out) == len(silence)
    assert rms(out) < 50


def test_opus_frames_are_actually_smaller_than_pcm():
    # The whole reason the firmware uses Opus. If this ever fails, the encoder is
    # not doing anything and the link is carrying raw audio at Opus's frame rate.
    codec = OpusCodec(SR, FRAME_MS)
    pcm = tone(600)
    encoded = sum(len(f) for f in codec.encode(pcm))
    assert encoded < len(pcm) / 4


def test_decoding_a_corrupt_opus_frame_raises_rather_than_returning_noise():
    codec = OpusCodec(SR, FRAME_MS)
    with pytest.raises(Exception):
        codec.decode(b"\xff\xff\xff\xff")


# ── the cross-client property that actually matters ──────────────────────────

def test_both_clients_present_identical_pcm_to_the_backend():
    """A backend must not be able to tell an ESP32 from the WASM harness."""
    pcm = tone(300)

    wasm = codec_from_hello({"audio_params": {"format": "pcm"}})
    esp32 = codec_from_hello({"audio_params": {"format": "opus"}})

    from_wasm = b"".join(wasm.decode(f) for f in wasm.encode(pcm))
    from_esp32 = b"".join(esp32.decode(f) for f in esp32.encode(pcm))

    assert len(from_wasm) == len(from_esp32) == len(pcm)
    assert rms(from_esp32[esp32.bytes_per_frame:]) == pytest.approx(
        rms(from_wasm[wasm.bytes_per_frame:]), rel=0.35
    )
