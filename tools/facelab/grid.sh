#!/usr/bin/env bash
# Render every face variant and tile them into one labelled contact sheet.
#
#   ./grid.sh <label>      -> facelab/out/grid_<label>.png
#
# Labels go ABOVE each tile so the grid is readable at a glance and diffable against an
# earlier run: the whole point is comparing "what it looks like now" with "what it looked
# like before" and with the design target, side by side.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LABEL="${1:-$(date +%Y%m%d_%H%M%S)}"
SKIN="${2:-default}"
TMP="$HERE/build/frames"
OUT="$HERE/out"

mkdir -p "$TMP" "$OUT"
rm -f "$TMP"/*.bmp "$TMP"/*.png

"$HERE/build/render_faces" "$TMP" "$SKIN" > "$TMP/order.txt"

TILES=()
while read -r name; do
    src="$TMP/$name.bmp"
    [ -f "$src" ] || continue
    # Label above the frame; -background/-splice keeps the face pixels untouched so the
    # grid shows exactly what was rendered, not a resampled approximation.
    magick "$src" \
        -bordercolor '#303030' -border 1 \
        -background '#101010' -fill '#e0e0e0' -pointsize 15 \
        label:"$name" +swap -gravity center -append \
        "$TMP/$name.png"
    TILES+=("$TMP/$name.png")
done < "$TMP/order.txt"

montage "${TILES[@]}" \
    -tile 4x -geometry +6+6 -background '#181818' \
    "$OUT/grid_${LABEL}.png"

echo "$OUT/grid_${LABEL}.png"
