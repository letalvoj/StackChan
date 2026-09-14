#!/usr/bin/env bash
# Put our laugh beside the artist's, millisecond for millisecond.
#
#   ./laugh.sh          -> out/laugh/
#
# Unlike astra.sh, these two *should* line up frame for frame. The Astra panels run on the
# modifiers' random timers, so only the character of the motion can match. A laugh is a
# scripted performance that starts the instant the emotion is set, so every capture has an
# exact counterpart in laugh-stop-motion.gif -- and a drawing that is wrong, late, or paired
# with the wrong eyes shows up as a mismatch at a known time.
#
# The counterpart is not always the GIF's frame at that time. A laughing face holds the laugh,
# so ours rests on the widest open mouth where the artist rests on a closed grin, and bursts
# through only the open steps. Outside the burst each capture is compared against that
# resting drawing instead; inside it, against the GIF on the GIF's own clock.
#
# Needs a render first: ./sheets.sh chalk (or anything that runs render_faces).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ART="$HERE/../facegen/artwork/laugh-2026-09-13"
TMP="$HERE/build/laugh"
FRAMES="$HERE/build/frames"
OUT="$HERE/out/laugh"

mkdir -p "$TMP" "$OUT"
rm -f "$TMP"/*.png "$OUT"/*.png "$OUT"/*.gif

[ -f "$FRAMES/seq-laugh-00.bmp" ] || { echo "no laugh render -- run ./sheets.sh chalk first" >&2; exit 1; }

magick "$ART/laugh-stop-motion.gif" -coalesce "$TMP/ref_%02d.png"

python3 - "$ART" "$TMP" "$FRAMES" "$OUT" <<'PY'
import json, pathlib, subprocess, sys

art, tmp, frames, out = (pathlib.Path(p) for p in sys.argv[1:5])
TICK = 33                                      # render_faces captures the laugh every update

steps = json.loads((art / "laugh-timing.json").read_text())["sequence"]
starts, t = [], 0
for step in steps:
    starts.append(t)
    t += step["hold_ms"]

def ink(img, crop):
    """Fraction of lit pixels in a region -- the numeric check, not an eyeball one."""
    v = subprocess.run(["magick", str(img), "-crop", crop, "+repage", "-colorspace", "Gray",
                        "-threshold", "30%", "-format", "%[fx:mean]", "info:"],
                       capture_output=True, text=True, check=True).stdout
    return float(v)

REGIONS = {"eyes": "220x60+50+70", "mouth": "120x80+100+130"}

# The firmware does not replay the whole GIF. A laughing face holds the laugh, so it keeps
# only the steps whose mouth is open (it shows its tongue) and drops the grin and settle the
# artist uses to recover. Its timeline, from the moment the emotion is set:
#   rest on the widest open mouth  ->  the open steps, on the artist's timing  ->  rest again
# The rest lasts the artist's own lead-in, so during the burst our clock and the GIF's agree.
open_steps = [i for i, s in enumerate(steps)
              if "#e67c89" in (art / "frames" / f"{s['mouth']}.svg").read_text()]
burst_from = starts[open_steps[0]]
burst_to = starts[open_steps[-1]] + steps[open_steps[-1]]["hold_ms"]
# The resting face is whichever open step draws the most mouth, measured on the GIF itself.
rest = max(open_steps, key=lambda i: ink(tmp / f"ref_{i:02d}.png", REGIONS["mouth"]))

# The harness sets the emotion and then advances in whole update ticks, capturing after each,
# so capture i is (i + 1) ticks after the set. The burst begins on the first tick at or after
# the lead-in -- not exactly at it -- and from then on runs on the artist's clock.
burst_tick = max(TICK, -(-burst_from // TICK) * TICK)

def expected_step(since_set):
    """Which of the artist's frames our face should match this long after the set, and the
    GIF time that corresponds to, or None while resting."""
    at = burst_from + since_set - burst_tick
    if since_set >= burst_tick and at < burst_to:
        return max(i for i, s in enumerate(starts) if s <= at), at
    return rest, None

ours = sorted(frames.glob("seq-laugh-*.bmp"))

pairs, rows = [], []
for i, frame in enumerate(ours):
    ms = (i + 1) * TICK                        # since the emotion was set
    k, at = expected_step(ms)
    ref = tmp / f"ref_{k:02d}.png"
    name = steps[k]["mouth"].replace("mouth-laugh-", "")
    if at is None:
        name = f"rest ({name})"
    pair = tmp / f"pair_{i:03d}.png"
    subprocess.run(["magick",
                    "(", str(ref), "-bordercolor", "#c08040", "-border", "2", ")",
                    "(", str(frame), "-bordercolor", "#4080c0", "-border", "2", ")",
                    "+append",
                    "-background", "#181818", "-fill", "#c0c0c0", "-pointsize", "14",
                    f"label:{ms:4d} ms   artist (left)  /  ours (right)   {name}",
                    "-gravity", "center", "-append", "+repage", str(pair)], check=True)
    pairs.append(str(pair))
    diffs = {r: abs(ink(frame, c) - ink(ref, c)) for r, c in REGIONS.items()}
    rows.append((ms, name, diffs))

# The animation, at the capture interval.
subprocess.run(["magick", "-delay", f"{TICK}x1000", *pairs, "-loop", "0", "-layers", "Optimize",
                str(out / "side-by-side.gif")], check=True)

# A readable sheet: the rest, then every burst step at the middle of its hold on our clock,
# then the rest it comes back to.
def capture_at(since_set):
    return max(0, min(len(pairs) - 1, since_set // TICK - 1))

beats = [pairs[capture_at(burst_tick // 2)]]
for k in open_steps:
    middle = burst_tick + (starts[k] - burst_from) + steps[k]["hold_ms"] // 2
    beats.append(pairs[capture_at(middle)])
beats.append(pairs[capture_at(burst_tick + (burst_to - burst_from) + 400)])
subprocess.run(["montage", *beats, "-tile", "3x", "-geometry", "+6+6", "-background", "#181818",
                str(out / "beats.png")], check=True)

print(f"  {'ms':>5}  {'artist step':<12} {'eyes Δink':>9} {'mouth Δink':>10}")
worst = {r: 0.0 for r in REGIONS}
for ms, name, d in rows:
    for r in REGIONS:
        worst[r] = max(worst[r], d[r])
    if ms % (TICK * 3) == 0:
        print(f"  {ms:5d}  {name:<12} {d['eyes']:9.4f} {d['mouth']:10.4f}")
print(f"  worst over all {len(rows)} captures: eyes {worst['eyes']:.4f}, mouth {worst['mouth']:.4f}")
print("  side-by-side.gif / beats.png")
PY

echo
echo "$OUT"
