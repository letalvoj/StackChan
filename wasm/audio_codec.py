"""Audio codec adaptation between the server and whatever the client advertises.

Two kinds of client connect to this server and they do NOT speak the same audio
format:

  * the WASM harness sends raw PCM16, because the browser has no Opus encoder in
    the path (see ARCHITECTURE.md §4.3);
  * the ESP32 firmware sends Opus, because that is what audio_service.cc does on
    real hardware and we do not want the device diverging from itself.

Rather than forcing one of them to change -- which would mean either crippling the
firmware or reimplementing the browser pipeline -- the server adapts. The client
declares its format in the `hello` handshake and gets a codec that turns its
frames into PCM16 on the way in and back into its own format on the way out.
Backends downstream therefore only ever deal in PCM16 and need no format logic.

`for_format()` is the entry point; everything downstream is PCM16 mono at
`sample_rate`.
"""

from __future__ import annotations

import struct

SAMPLE_WIDTH = 2          # PCM16
DEFAULT_SAMPLE_RATE = 16000
DEFAULT_FRAME_MS = 60


class CodecError(RuntimeError):
    """Raised when a client asks for a format we cannot service."""


class AudioCodec:
    """PCM16 on the inside, the client's format on the wire."""

    name = "abstract"

    def __init__(self, sample_rate=DEFAULT_SAMPLE_RATE, frame_ms=DEFAULT_FRAME_MS):
        self.sample_rate = sample_rate
        self.frame_ms = frame_ms

    @property
    def samples_per_frame(self) -> int:
        return int(self.sample_rate * self.frame_ms / 1000)

    @property
    def bytes_per_frame(self) -> int:
        return self.samples_per_frame * SAMPLE_WIDTH

    def decode(self, frame: bytes) -> bytes:
        """One wire frame -> PCM16 bytes."""
        raise NotImplementedError

    def encode(self, pcm: bytes) -> list:
        """PCM16 bytes -> a list of wire frames, split on the frame size.

        A trailing partial frame is zero-padded rather than dropped: Opus rejects
        short frames outright, and silently discarding the tail clips the end of
        every utterance.
        """
        raise NotImplementedError


class PcmCodec(AudioCodec):
    """Pass-through. What the WASM harness uses."""

    name = "pcm"

    def decode(self, frame: bytes) -> bytes:
        return frame

    def encode(self, pcm: bytes) -> list:
        step = self.bytes_per_frame
        out = []
        for off in range(0, len(pcm), step):
            chunk = pcm[off:off + step]
            if len(chunk) < step:
                chunk = chunk + b"\x00" * (step - len(chunk))
            out.append(chunk)
        return out


class OpusCodec(AudioCodec):
    """What the ESP32 firmware uses. Requires opuslib (and libopus)."""

    name = "opus"

    def __init__(self, sample_rate=DEFAULT_SAMPLE_RATE, frame_ms=DEFAULT_FRAME_MS):
        super().__init__(sample_rate, frame_ms)
        try:
            import opuslib
        except ImportError as exc:      # pragma: no cover - environment dependent
            raise CodecError(
                "device negotiated Opus but opuslib is not installed "
                "(`brew install opus && pip install opuslib`)"
            ) from exc

        # VOIP tuning matches what the firmware encoder targets; APPLICATION_AUDIO
        # would add latency this path cannot afford.
        self._encoder = opuslib.Encoder(sample_rate, 1, "voip")
        self._decoder = opuslib.Decoder(sample_rate, 1)

    def decode(self, frame: bytes) -> bytes:
        return self._decoder.decode(frame, self.samples_per_frame)

    def encode(self, pcm: bytes) -> list:
        step = self.bytes_per_frame
        out = []
        for off in range(0, len(pcm), step):
            chunk = pcm[off:off + step]
            if len(chunk) < step:
                chunk = chunk + b"\x00" * (step - len(chunk))
            out.append(self._encoder.encode(chunk, self.samples_per_frame))
        return out


_REGISTRY = {"pcm": PcmCodec, "opus": OpusCodec}


def for_format(fmt, sample_rate=DEFAULT_SAMPLE_RATE, frame_ms=DEFAULT_FRAME_MS) -> AudioCodec:
    """Build the codec a client asked for.

    An unset format means PCM: the WASM harness has always been implicitly raw,
    and defaulting the other way would turn a missing field into garbage audio
    rather than an obvious error.
    """
    key = (fmt or "pcm").lower()
    try:
        return _REGISTRY[key](sample_rate, frame_ms)
    except KeyError:
        raise CodecError(
            f"unsupported audio format {fmt!r}; known: {', '.join(sorted(_REGISTRY))}"
        ) from None


def codec_from_hello(hello: dict) -> AudioCodec:
    """Pick a codec from a client's `hello`, tolerating a missing audio_params."""
    params = hello.get("audio_params") or {}
    return for_format(
        params.get("format"),
        int(params.get("sample_rate") or DEFAULT_SAMPLE_RATE),
        int(params.get("frame_duration") or DEFAULT_FRAME_MS),
    )


def pcm_to_samples(pcm: bytes) -> list:
    """PCM16 bytes -> signed ints. Used by the echo backend's level metering."""
    return list(struct.unpack(f"<{len(pcm) // SAMPLE_WIDTH}h", pcm[:len(pcm) // SAMPLE_WIDTH * SAMPLE_WIDTH]))
