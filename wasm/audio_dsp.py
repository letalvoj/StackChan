"""Centralized audio signal processing utilities for StackChan servers and simulators."""
import math
import struct
from typing import Optional

RMS_UI_SCALE = 1.0


def compute_rms_normalized(pcm_bytes: bytes, scale: float = RMS_UI_SCALE) -> float:
    """Compute normalized RMS from raw PCM16 little-endian bytes for oscilloscope visualization."""
    num_samples = len(pcm_bytes) // 2
    if num_samples == 0:
        return 0.0
    samples = struct.unpack(f"<{num_samples}h", pcm_bytes)
    rms_raw = math.sqrt(sum(s * s for s in samples) / num_samples)
    return min(1.0, (rms_raw / 32768.0) * scale)


def downsample_24k_to_16k(pcm_24k: bytes) -> bytes:
    """Downsample 24kHz PCM16 LE to 16kHz using 3:2 linear interpolation for hardware playback."""
    num_samples_24k = len(pcm_24k) // 2
    if num_samples_24k == 0:
        return b""
    samples_24k = struct.unpack(f"<{num_samples_24k}h", pcm_24k)
    output_len = num_samples_24k * 2 // 3
    samples_16k = []
    for i in range(output_len):
        src_pos = i * 3.0 / 2.0
        idx = int(src_pos)
        frac = src_pos - idx
        if idx + 1 < num_samples_24k:
            val = samples_24k[idx] * (1.0 - frac) + samples_24k[idx + 1] * frac
        else:
            val = float(samples_24k[min(idx, num_samples_24k - 1)])
        samples_16k.append(max(-32768, min(32767, int(val))))
    return struct.pack(f"<{len(samples_16k)}h", *samples_16k)
