#!/usr/bin/env bash
# Compare our face against the artist's own performance, beat for beat.
#
#   ./astra.sh          -> out/astra/
#
# independent-motion.gif is six faces running side by side for 5.4 seconds. The renderer
# produces the same six panels at the same cadence, and this composes ours into the same
# 2x3 layout so the two can be put next to each other at matching moments.
#
# Comparing a still against a still is not enough here. The question is whether our face
# has the *nuance* the artist drew -- whether a grumpy eye keeps its pupil under the lid,
# whether a delighted mouth rests wide rather than small -- and that only shows up when the
# same instant is placed side by side.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PACK="$HERE/../facegen/artwork/pack-2026-09-12"
TMP="$HERE/build/astra"
FRAMES="$HERE/build/frames"
OUT="$HERE/out/astra"

mkdir -p "$TMP" "$OUT"
rm -f "$OUT"/*.png "$OUT"/*.gif

# The artist's frames, coalesced once. 90 frames at 60 ms; ours are 30 at 180 ms, so every
# third of theirs lines up with one of ours.
if [ ! -f "$TMP/f_000.png" ]; then
    magick "$PACK/independent-motion.gif" -coalesce "$TMP/f_%03d.png"
fi

python3 - "$HERE" "$TMP" "$FRAMES" "$OUT" <<'PY'
import pathlib, subprocess, sys

here, tmp, frames, out = (pathlib.Path(p) for p in sys.argv[1:5])

# Panel order matches the artist's grid, reading across then down.
PANELS = [("astra-idle", "idle / looking around"),
          ("astra-curious", "curious / what was that?"),
          ("astra-delighted", "delighted / talking"),
          ("astra-grumpy", "grumpy / talking"),
          ("astra-sad", "sad / talking"),
          ("astra-petted", "petted / melting")]

OURS = 45           # frames the renderer produced per panel
THEIRS_PER_OURS = 2 # 90 of theirs over 45 of ours

def label(src, caption, dst, size="320x240"):
    subprocess.run(["magick", str(src), "-resize", size + "!",
                    "-bordercolor", "#404040", "-border", "1",
                    "-background", "#101010", "-fill", "#c0c0c0", "-pointsize", "15",
                    # Caption under the tile, matching where the artist bakes theirs. Side
                    # by side, captions on opposite edges make the two grids hard to read
                    # against each other.
                    f"label:{caption}", "-gravity", "center", "-append",
                    "+repage", str(dst)], check=True)
    return str(dst)

# --- our own six-panel composite, per beat -----------------------------------------
mine = []
for i in range(OURS):
    cells = [label(frames / f"seq-{name}-{i:02d}.bmp", cap, tmp / f"cell-{name}-{i:02d}.png")
             for name, cap in PANELS]
    grid = tmp / f"ours-{i:02d}.png"
    subprocess.run(["montage", *cells, "-tile", "3x2", "-geometry", "+6+6",
                    "-background", "#181818", str(grid)], check=True)
    mine.append(str(grid))

subprocess.run(["magick", *["-delay", "12"] + mine, "-loop", "0", "-layers", "Optimize",
                str(out / "ours.gif")], check=True)

# --- the two performances running together, side by side ----------------------------
# A still says whether a pose is right. Only this says whether the *motion* is right --
# whether a blink lands where theirs lands, whether a jaw works at the same rate. Both
# sides are driven by their own clock and never sync up, which is the point: what should
# match is the character of the movement, not the frame numbers.
sxs = []
for i in range(OURS):
    theirs = tmp / f"f_{i * THEIRS_PER_OURS:03d}.png"
    pair   = tmp / f"sxs-{i:02d}.png"
    subprocess.run(["magick",
                    "(", str(theirs), "-resize", "x520",
                    "-bordercolor", "#c08040", "-border", "2",
                    "-background", "#181818", "-fill", "#e8b070", "-pointsize", "20",
                    "label:artist reference", "+swap", "-gravity", "center", "-append", ")",
                    "(", str(mine[i]), "-resize", "x520",
                    "-bordercolor", "#4080c0", "-border", "2",
                    "-background", "#181818", "-fill", "#70b0e8", "-pointsize", "20",
                    "label:ours", "+swap", "-gravity", "center", "-append", ")",
                    "-background", "#181818", "-gravity", "south", "+append",
                    # The label carries its own small canvas, and an animation honours page
                    # geometry where a still viewer ignores it -- without this the GIF
                    # crops itself down to the size of the caption.
                    "+repage", str(pair)], check=True)
    sxs.append(str(pair))

subprocess.run(["magick", *["-delay", "12"] + sxs, "-loop", "0", "-layers", "Optimize",
                str(out / "side-by-side.gif")], check=True)

# --- theirs beside ours, at matching moments ----------------------------------------
# Four evenly spaced beats: enough to read the performance without a wall of images.
for n, i in enumerate((5, 16, 27, 38)):
    theirs = tmp / f"f_{i * THEIRS_PER_OURS:03d}.png"
    pair = out / f"beat-{n}-at-{i * 120}ms.png"
    subprocess.run(["magick",
                    "(", str(theirs), "-resize", "1000x", "-bordercolor", "#c08040", "-border", "2",
                    "-background", "#181818", "-fill", "#e8b070", "-pointsize", "22",
                    "label:artist reference", "+swap", "-gravity", "center", "-append", ")",
                    "(", str(mine[i]), "-resize", "1000x", "-bordercolor", "#4080c0", "-border", "2",
                    "-background", "#181818", "-fill", "#70b0e8", "-pointsize", "22",
                    "label:ours", "+swap", "-gravity", "center", "-append", ")",
                    "-background", "#181818", "-append", "+repage", str(pair)], check=True)
    print(f"  beat-{n}-at-{i * 120}ms.png")

# One sheet with every beat, for the whole performance at a glance.
subprocess.run(["magick", *[str(out / p.name) for p in sorted(out.glob("beat-*.png"))],
                "-background", "#181818", "-append", str(out / "all-beats.png")], check=True)
print("  ours.gif / side-by-side.gif / all-beats.png")
PY

echo
echo "$OUT"
