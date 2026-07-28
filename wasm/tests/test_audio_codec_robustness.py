"""Hostile and malformed `hello` handshakes.

The server builds a codec straight from a value the client controls, and
serve.py guards that with `except CodecError`. Anything that escapes as a
different exception type kills the connection handler with a traceback instead
of a clean close, so the contract these tests pin down is narrow and important:

    for_format() and codec_from_hello() raise CodecError, or they succeed.

Every case here was a live crash before the accompanying fix.

    cd wasm && ./.venv/bin/python -m pytest tests -q
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from audio_codec import CodecError, OpusCodec, codec_from_hello, for_format  # noqa: E402


def hello(**audio_params):
    return {"type": "hello", "version": 1, "audio_params": audio_params}


# ── the contract ─────────────────────────────────────────────────────────────

MALFORMED = [
    pytest.param(hello(format=123), id="format-is-a-number"),
    pytest.param(hello(format=["opus"]), id="format-is-a-list"),
    pytest.param(hello(format={"a": 1}), id="format-is-an-object"),
    pytest.param(hello(format="opus", sample_rate="abc"), id="sample_rate-not-a-number"),
    pytest.param(hello(format="pcm", sample_rate="abc"), id="sample_rate-not-a-number-pcm"),
    pytest.param(hello(format="opus", frame_duration="soon"), id="frame_duration-not-a-number"),
    pytest.param(hello(format="opus", sample_rate=22050), id="opus-unsupported-rate-22050"),
    pytest.param(hello(format="opus", sample_rate=44100), id="opus-unsupported-rate-44100"),
    pytest.param(hello(format="opus", sample_rate=0), id="opus-zero-rate"),
    pytest.param(hello(format="opus", sample_rate=-16000), id="opus-negative-rate"),
    pytest.param(hello(format="opus", frame_duration=0), id="zero-frame-duration"),
    pytest.param(hello(format="opus", frame_duration=-60), id="negative-frame-duration"),
    pytest.param(hello(format="opus", frame_duration=17), id="opus-frame-duration-not-on-grid"),
    pytest.param({"audio_params": [1, 2, 3]}, id="audio_params-is-a-list"),
    pytest.param({"audio_params": "opus"}, id="audio_params-is-a-string"),
]


@pytest.mark.parametrize("bad", MALFORMED)
def test_malformed_hello_raises_codec_error_and_nothing_else(bad):
    """A client must never be able to crash the handler with a stray type."""
    with pytest.raises(CodecError):
        codec_from_hello(bad)


def test_for_format_rejects_non_strings():
    for value in (123, 4.5, [], {}, object()):
        with pytest.raises(CodecError):
            for_format(value)


# ── things that must keep working ────────────────────────────────────────────

@pytest.mark.parametrize("rate", [8000, 12000, 16000, 24000, 48000])
def test_every_opus_supported_rate_is_accepted(rate):
    assert codec_from_hello(hello(format="opus", sample_rate=rate)).sample_rate == rate


@pytest.mark.parametrize("ms", [10, 20, 40, 60])
def test_opus_frame_durations_on_the_grid_are_accepted(ms):
    assert codec_from_hello(hello(format="opus", frame_duration=ms)).frame_ms == ms


def test_numeric_strings_are_still_tolerated():
    # JSON from a hand-rolled client often quotes numbers; that is sloppy, not hostile.
    codec = codec_from_hello(hello(format="opus", sample_rate="16000", frame_duration="60"))
    assert codec.sample_rate == 16000 and codec.frame_ms == 60


def test_absent_and_null_fields_fall_back_to_pcm_defaults():
    for params in ({}, {"format": None}, {"format": None, "sample_rate": None}):
        codec = codec_from_hello({"audio_params": params})
        assert codec.name == "pcm"
        assert codec.sample_rate == 16000 and codec.frame_ms == 60

    assert codec_from_hello({}).name == "pcm"
    assert codec_from_hello({"audio_params": None}).name == "pcm"


# ── frame-size disagreement ──────────────────────────────────────────────────

def test_decoding_a_shorter_frame_than_negotiated_is_reported_not_silent():
    """A device that advertises 60 ms but sends 20 ms must not pass unnoticed.

    Opus happily decodes the shorter packet and returns fewer samples, so without
    a check the server forwards 640 bytes where the rest of the pipeline expects
    1920 -- audio that is subtly wrong rather than obviously broken.
    """
    sender = OpusCodec(16000, 20)
    receiver = OpusCodec(16000, 60)
    short_frame = sender.encode(b"\x00" * sender.bytes_per_frame)[0]

    with pytest.raises(CodecError) as exc:
        receiver.decode(short_frame)
    assert "frame" in str(exc.value).lower()


def test_matching_frame_sizes_decode_cleanly():
    codec = OpusCodec(16000, 60)
    frame = codec.encode(b"\x00" * codec.bytes_per_frame)[0]
    assert len(codec.decode(frame)) == codec.bytes_per_frame


def test_garbage_binary_is_a_codec_error_not_a_raw_opus_error():
    """serve.py drops undecodable frames; it must be able to catch them by type."""
    with pytest.raises(CodecError):
        OpusCodec(16000, 60).decode(b"\xff\xff\xff\xff\xff\xff")


def test_empty_frame_is_a_codec_error():
    with pytest.raises(CodecError):
        OpusCodec(16000, 60).decode(b"")
