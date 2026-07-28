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
        if sample_rate <= 0:
            raise CodecError(f"sample_rate must be positive, got {sample_rate}")
        if frame_ms <= 0:
            raise CodecError(f"frame_duration must be positive, got {frame_ms}")
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

    # libopus accepts only these; anything else fails deep inside the C library with
    # an opaque "invalid argument", which used to escape as a raw OpusError.
    SUPPORTED_RATES = (8000, 12000, 16000, 24000, 48000)
    SUPPORTED_FRAME_MS = (10, 20, 40, 60)      # 2.5/5 ms exist but are unusable here

    def __init__(self, sample_rate=DEFAULT_SAMPLE_RATE, frame_ms=DEFAULT_FRAME_MS):
        super().__init__(sample_rate, frame_ms)
        if sample_rate not in self.SUPPORTED_RATES:
            raise CodecError(
                f"Opus does not support {sample_rate} Hz; "
                f"use one of {', '.join(map(str, self.SUPPORTED_RATES))}"
            )
        if frame_ms not in self.SUPPORTED_FRAME_MS:
            raise CodecError(
                f"Opus frame_duration {frame_ms} ms is not on the grid; "
                f"use one of {', '.join(map(str, self.SUPPORTED_FRAME_MS))}"
            )
        try:
            import opuslib
        except ImportError as exc:      # pragma: no cover - environment dependent
            raise CodecError(
                "device negotiated Opus but opuslib is not installed "
                "(`brew install opus && pip install opuslib`)"
            ) from exc

        # VOIP tuning matches what the firmware encoder targets; APPLICATION_AUDIO
        # would add latency this path cannot afford.
        try:
            self._encoder = opuslib.Encoder(sample_rate, 1, "voip")
            self._decoder = opuslib.Decoder(sample_rate, 1)
        except Exception as exc:        # noqa: BLE001 - opuslib raises its own type
            raise CodecError(f"could not initialise Opus at {sample_rate} Hz: {exc}") from exc

    def decode(self, frame: bytes) -> bytes:
        if not frame:
            raise CodecError("empty Opus frame")
        try:
            pcm = self._decoder.decode(frame, self.samples_per_frame)
        except Exception as exc:        # noqa: BLE001 - opuslib raises its own type
            raise CodecError(f"undecodable Opus frame ({len(frame)} bytes): {exc}") from exc

        # Opus decodes a shorter packet happily and returns fewer samples. Letting that
        # through would forward, say, 640 bytes where the pipeline expects 1920 --
        # audio that is subtly wrong rather than obviously broken. Say so instead.
        if len(pcm) != self.bytes_per_frame:
            raise CodecError(
                f"frame decoded to {len(pcm)} bytes, expected {self.bytes_per_frame} "
                f"({self.frame_ms} ms @ {self.sample_rate} Hz) -- the peer is probably "
                f"using a different frame_duration than it advertised"
            )
        return pcm

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
    if fmt is None:
        fmt = "pcm"
    if not isinstance(fmt, str):
        raise CodecError(f"audio format must be a string, got {type(fmt).__name__}: {fmt!r}")
    try:
        cls = _REGISTRY[fmt.lower()]
    except KeyError:
        raise CodecError(
            f"unsupported audio format {fmt!r}; known: {', '.join(sorted(_REGISTRY))}"
        ) from None
    return cls(sample_rate, frame_ms)


def _positive_int(value, default, label):
    """Coerce a client-supplied number, tolerating quoted digits but nothing worse."""
    if value is None or value == "":
        return default
    try:
        return int(value)
    except (TypeError, ValueError):
        raise CodecError(f"{label} must be a number, got {value!r}") from None


def codec_from_hello(hello: dict) -> AudioCodec:
    """Pick a codec from a client's `hello`.

    Everything here is attacker- or bug-controlled, and the caller guards this with
    `except CodecError`, so any other exception type escaping would take down the
    connection handler instead of closing the socket cleanly.
    """
    params = hello.get("audio_params")
    if params is None or params == {}:
        params = {}
    elif not isinstance(params, dict):
        raise CodecError(f"audio_params must be an object, got {type(params).__name__}")

    return for_format(
        params.get("format"),
        _positive_int(params.get("sample_rate"), DEFAULT_SAMPLE_RATE, "sample_rate"),
        _positive_int(params.get("frame_duration"), DEFAULT_FRAME_MS, "frame_duration"),
    )


def pcm_to_samples(pcm: bytes) -> list:
    """PCM16 bytes -> signed ints. Used by the echo backend's level metering."""
    return list(struct.unpack(f"<{len(pcm) // SAMPLE_WIDTH}h", pcm[:len(pcm) // SAMPLE_WIDTH * SAMPLE_WIDTH]))
