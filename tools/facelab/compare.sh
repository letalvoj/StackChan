#!/usr/bin/env bash
# Stack the artist's drawing above our render, tile by tile.
#
#   ./compare.sh            -> out/compare_chalk.png
#
# The grid on its own only shows what we drew; it cannot tell you whether we drew the right
# thing. This renders each face-* straight out of the artist's robot-face.svg at the same
# scale and geometry the sprite baker used, and puts it directly above the frame the real
# skin produced through LVGL. A port that has drifted shows up as a misaligned pair.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ART="${ART_SVG:-$HERE/../facegen/artwork/robot-face.svg}"
TMP="$HERE/build/compare"
OUT="$HERE/out"
SKIN="${1:-chalk}"

mkdir -p "$TMP" "$OUT"
rm -f "$TMP"/*.png

# The eighteen the artist drew. Our grid renders more than this -- idle gaze, breath, the
# speech bubble -- but those have no counterpart to compare against.
TILES="neutral happy angry sad doubt sleepy blink halfblink speak-sm speak-md speak-lg happytalk heart shy headpet angrymark sweat dizzy"

# Render the original vector paths with the same explicit tuning constants as the baker.
# This remains independent of the C++ sprite composition, anchors and LVGL rendering.
python3 - "$HERE" "$ART" "$TMP" $TILES <<'PYREF'
import pathlib, subprocess, sys
here, art, tmp = map(pathlib.Path, sys.argv[1:4])
sys.path.insert(0, str(here.parent / "facegen"))
import gen_sprites as gen
refs = gen.tuned_defs(art.read_text(), layout=True)
for name in sys.argv[4:]:
    png = gen.render(refs, "face-" + name, None, tmp, "ref_" + name)
    subprocess.run(["magick", str(png), "-background", "black", "-alpha", "remove",
                    "-gravity", "center", "-extent", "320x240",
                    str(tmp / ("ref_" + name + "_padded.png"))], check=True)
PYREF

PAIRS=()
for t in $TILES; do
    ours="$HERE/build/frames/$t.bmp"
    [ -f "$ours" ] || { echo "missing render for $t -- run ./grid.sh first" >&2; exit 1; }
    magick \
        \( "$TMP/ref_${t}_padded.png" -bordercolor '#404040' -border 1 \) \
        \( "$ours" -bordercolor '#404040' -border 1 \) \
        -background '#101010' -append \
        -background '#101010' -fill '#e0e0e0' -pointsize 16 \
        label:"$t" +swap -gravity center -append \
        "$TMP/pair_$t.png"
    PAIRS+=("$TMP/pair_$t.png")
done

montage "${PAIRS[@]}" -tile 6x -geometry +8+8 -background '#181818' "$OUT/compare_${SKIN}.png"
echo "$OUT/compare_${SKIN}.png"
echo "top of each pair: the artist's SVG with size/eye tuning.  bottom: what the skin renders through LVGL."
