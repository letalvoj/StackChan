"""The 24 kHz -> 16 kHz downsampler must actually filter, not merely interpolate.

This guards the second audio bug, which sounded nothing like the first one (see
test_stream_framing.py for that one). The original resampler was linear interpolation
with no anti-alias filter, so every frequency above the output's 8 kHz Nyquist folded
back into the voice band at full strength. Speech from a 24 kHz TTS has real energy up
there in every sibilant, so each "s" and "sh" dumped an inharmonic buzz into the middle
of the voice -- a continuous rasp riding on the speech rather than clicks between it.

The measurement is direct: put a pure tone in, read the level out at the frequency it
would fold to. A regression here is silent to every other test in the suite, because
the audio still has the right length, the right frame count and no gaps -- it just
sounds wrong.
"""

import math
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "clients"))

import gemini_live as G
from gemini_live import FULL_SCALE, Downsampler  # noqa: E402

SRC_RATE, DST_RATE = 24000, 16000
NYQUIST = DST_RATE / 2


@pytest.fixture(autouse=True)
def no_agc():
    """Auto-gain is module-level and defaults on, since only main() ever flips it.

    Every test above this fixture measures the FILTER -- passband flatness, alias
    rejection, the limiter -- and auto-gain moving the signal underneath a
    measurement is a second variable these tests are not supposed to have. It
    happened to still pass with AGC live (the rejection margins are ~50 dB), which
    is worse than failing: a real regression could hide behind gain drift the same
    way. AGC gets its own tests below, with this fixture bypassed explicitly.
    """
    old = G.AGC_ENABLED
    G.AGC_ENABLED = False
    yield
    G.AGC_ENABLED = old


def tone(freq, n, rate=SRC_RATE, amp=12000):
    return struct.pack(f"<{n}h", *[
        int(amp * math.sin(2 * math.pi * freq * i / rate)) for i in range(n)])


def power_db(pcm, freq, rate=DST_RATE):
    """Goertzel: energy at one frequency, in dB. No numpy in this project."""
    x = memoryview(pcm).cast("h")
    w = 2 * math.pi * freq / rate
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + c * s1 - s2
        s2, s1 = s1, s0
    p = (s1 * s1 + s2 * s2 - c * s1 * s2) / (len(x) ** 2)
    return -99.0 if p <= 1e-12 else 10 * math.log10(p)


def fold(freq):
    """Where `freq` lands after decimating to DST_RATE without a filter."""
    return abs(((freq + NYQUIST) % DST_RATE) - NYQUIST)


def through(freq, seconds=1.0, amp=12000):
    d = Downsampler(SRC_RATE, DST_RATE)
    out = d.feed(tone(freq, int(SRC_RATE * seconds), amp=amp))
    return d, out[2000:]          # skip the filter's start-up transient


@pytest.mark.parametrize("freq", [500, 1000, 3000, 6000])
def test_passband_is_flat(freq):
    """Everything a voice actually lives in must come through unattenuated."""
    _, out = through(freq)
    assert power_db(out, freq) > 73.0


@pytest.mark.parametrize("freq", [9000, 10000, 11000, 11500])
def test_above_nyquist_is_rejected(freq):
    """The bug. Linear interpolation passed these at ~71 dB, i.e. not at all filtered."""
    _, out = through(freq)
    landed = power_db(out, fold(freq))
    assert landed < 25.0, (
        f"{freq} Hz folded to {fold(freq)} Hz at {landed:.1f} dB; the anti-alias "
        f"filter is not doing its job -- this is the sibilant buzz"
    )


def test_ratio_is_exact_every_block():
    """1440 source samples in, 960 out, block after block.

    enqueue_audio slices at exactly one device frame of source audio and hands the
    result straight to the Opus encoder, which pads anything short. If the ratio ever
    drifted, that padding would put silence back in the middle of the stream -- the
    first audio bug, reintroduced through the fix for the second.
    """
    d = Downsampler(SRC_RATE, DST_RATE)
    for _ in range(8):
        assert len(d.feed(tone(440, 1440))) // 2 == 960


def test_limiter_never_wraps():
    """A full-scale input must round off, not wrap around into the opposite rail."""
    d = Downsampler(SRC_RATE, DST_RATE)
    out = d.feed(tone(500, 4800, amp=int(FULL_SCALE)))
    samples = list(memoryview(out).cast("h"))
    assert max(samples) <= FULL_SCALE and min(samples) >= -FULL_SCALE
    # Wrapping shows up as a sign flip at the peak, which no low-passed sine can do.
    assert d.clipped > 0, "expected the limiter to engage on a full-scale input"


def test_reset_clears_history_but_keeps_levels():
    """Barge-in resets the filter; the headroom report is about the speaker, not the turn."""
    d = Downsampler(SRC_RATE, DST_RATE)
    d.feed(tone(440, 1440, amp=int(FULL_SCALE)))
    peak_before = d.peak
    d.reset()
    assert d.peak == peak_before
    assert d.n_in == 0 and not d.buf
    peak, _, _ = d.take_levels()
    assert peak > 0.9 and d.peak == 0


# --------------------------------------------------------------------------- AGC
#
# no_agc above is bypassed in every test below -- these ARE the auto-gain tests.

def with_agc(fn):
    """Run fn with AGC forced on, regardless of the no_agc fixture's default."""
    old = G.AGC_ENABLED
    G.AGC_ENABLED = True
    try:
        return fn()
    finally:
        G.AGC_ENABLED = old


def test_agc_boosts_quiet_speech():
    """A quiet voice should climb toward the target ceiling over a few blocks.

    10% of full scale is a plausible quiet TTS level; left unboosted it would play
    back noticeably softer than before the aliasing fix removed the extra
    brightness that used to read as loudness -- see Downsampler's docstring.
    """
    def run():
        d = Downsampler(SRC_RATE, DST_RATE)
        for _ in range(30):
            d.feed(tone(300, 1440, amp=int(0.10 * FULL_SCALE)))
        return d.agc_gain
    gain = with_agc(run)
    assert gain > 2.0, f"expected a quiet voice to be boosted well above unity, got {gain:.2f}x"


def test_agc_does_not_boost_loud_speech():
    """Already-loud speech must not be pushed louder -- that is what clips it."""
    def run():
        d = Downsampler(SRC_RATE, DST_RATE)
        for _ in range(10):
            d.feed(tone(300, 1440, amp=int(0.90 * FULL_SCALE)))
        return d.agc_gain
    gain = with_agc(run)
    assert gain <= 1.05, f"expected loud speech to stay near unity gain, got {gain:.2f}x"


def test_agc_gain_is_bounded():
    """Silence must not be amplified into a hiss, and gain must never invert phase."""
    def run():
        d = Downsampler(SRC_RATE, DST_RATE)
        for _ in range(40):
            d.feed(tone(300, 1440, amp=1))       # near silence
        return d.agc_gain
    gain = with_agc(run)
    assert 0 < gain <= G.AGC_MAX_GAIN + 1e-9


def test_agc_disabled_holds_unity_gain():
    """GEMINI_API_AGC=0 must genuinely disable boosting, not just relabel it."""
    d = Downsampler(SRC_RATE, DST_RATE)          # no_agc fixture: AGC_ENABLED False
    for _ in range(30):
        d.feed(tone(300, 1440, amp=int(0.10 * FULL_SCALE)))
    assert d.agc_gain == 1.0
