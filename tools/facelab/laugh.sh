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
duration = t

def artist_step(ms):
    """Which of the artist's frames is on screen at this millisecond of the laugh."""
    ms %= duration
    return max(i for i, s in enumerate(starts) if s <= ms)

ours = sorted(frames.glob("seq-laugh-*.bmp"))

def ink(img, crop):
    """Fraction of lit pixels in a region -- the numeric check, not an eyeball one."""
    v = subprocess.run(["magick", str(img), "-crop", crop, "+repage", "-colorspace", "Gray",
                        "-threshold", "30%", "-format", "%[fx:mean]", "info:"],
                       capture_output=True, text=True, check=True).stdout
    return float(v)

REGIONS = {"eyes": "220x60+50+70", "mouth": "120x80+100+130"}

pairs, rows = [], []
for i, frame in enumerate(ours):
    ms = i * TICK
    k = artist_step(ms)
    ref = tmp / f"ref_{k:02d}.png"
    name = steps[k]["mouth"].replace("mouth-laugh-", "")
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

# One still per artist step, at the middle of its hold, as a readable sheet.
beats = []
for k, s in enumerate(starts):
    i = min(len(pairs) - 1, (s + steps[k]["hold_ms"] // 2) // TICK)
    beats.append(pairs[i])
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
