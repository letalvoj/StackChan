#!/usr/bin/env bash
# Build every offline review artefact from one render pass.
#
#   ./sheets.sh [skin]        -> out/sheets/<skin>/
#
# Four kinds of output, because they answer different questions:
#
#   matrix-*.png    combination grids. Does every emotion survive every mouth opening,
#                   every lid position, every corner of the pupil's travel?
#   strip-*.png     a performance laid out left to right, each frame labelled with the
#                   hold the runtime actually uses. Read the transitions.
#   anim-*.gif      the same performance at its real timing. A blink either reads as a
#                   blink or it does not, and no still will tell you which.
#   index.png       every strip stacked, to see what moved since last time.
#
# A still can only show that a pose is correct. Whether a mouth keeps its corners while the
# jaw opens, or a pupil stays inside the white at full gaze, is a question about the frames
# in between -- which is why the renderer emits timed sequences and not just tiles.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SKIN="${1:-chalk}"
TMP="$HERE/build/frames"
OUT="$HERE/out/sheets/$SKIN"

mkdir -p "$TMP" "$OUT"
rm -f "$TMP"/*.bmp "$TMP"/lbl-*.png "$TMP"/manifest.txt
rm -f "$OUT"/*.png "$OUT"/*.gif

"$HERE/build/render_faces" "$TMP" "$SKIN" >/dev/null

python3 - "$TMP" "$OUT" "$SKIN" <<'PY'
import collections, pathlib, subprocess, sys

tmp, out, skin = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]

# How many columns each matrix wants, so every row is one emotion and reads across.
COLUMNS = {"main": 6, "mouth": 4, "eyes": 3, "gaze": 3}

tiles = collections.defaultdict(list)      # group -> [(name, file)]
captions = {}                              # sequence -> caption
frames = collections.defaultdict(list)     # sequence -> [(file, hold_ms)]

for line in (tmp / "manifest.txt").read_text().splitlines():
    kind, *rest = line.split("|")
    if kind == "tile":
        group, name, file = rest
        tiles[group].append((name, file))
    elif kind == "seq":
        captions[rest[0]] = rest[1]
    elif kind == "frame":
        seq, file, ms = rest
        frames[seq].append((file, int(ms)))


def labelled(src, caption, dst):
    """Caption above the frame, so a sheet stays diffable against an earlier run."""
    subprocess.run(["magick", str(src),
                    "-bordercolor", "#303030", "-border", "1",
                    "-background", "#101010", "-fill", "#e0e0e0", "-pointsize", "15",
                    f"label:{caption}", "+swap", "-gravity", "center", "-append",
                    str(dst)], check=True)
    return str(dst)


for group, entries in tiles.items():
    cols = COLUMNS.get(group, 4)
    cells = [labelled(tmp / f, n, tmp / f"lbl-{group}-{n}.png") for n, f in entries]
    subprocess.run(["montage", *cells, "-tile", f"{cols}x", "-geometry", "+6+6",
                    "-background", "#181818", str(out / f"matrix-{group}.png")], check=True)
    print(f"  matrix-{group}.png   {len(cells)} tiles")

strips = []
for seq, entries in frames.items():
    cells = [labelled(tmp / f, f"{ms}ms", tmp / f"lbl-seq-{seq}-{i}.png")
             for i, (f, ms) in enumerate(entries)]

    # Filmstrip: one row, so the eye reads it as time passing left to right.
    body = tmp / f"strip-body-{seq}.png"
    subprocess.run(["montage", *cells, "-tile", f"{len(cells)}x1", "-geometry", "+4+4",
                    "-background", "#181818", str(body)], check=True)
    strip = out / f"strip-{seq}.png"
    subprocess.run(["magick", str(body),
                    "-background", "#181818", "-fill", "#e0e0e0", "-pointsize", "20",
                    f"label:{seq} — {captions.get(seq, '')}",
                    "+swap", "-gravity", "west", "-append", str(strip)], check=True)
    strips.append(str(strip))

    # GIF delays are centiseconds, so each frame's reported hold is converted rather than
    # guessed. The animation then runs at the speed the modifiers actually produce.
    args = []
    for f, ms in entries:
        args += ["-delay", str(max(2, round(ms / 10))), str(tmp / f)]
    subprocess.run(["magick", *args, "-loop", "0", "-layers", "Optimize",
                    str(out / f"anim-{seq}.gif")], check=True)
    print(f"  strip-{seq}.png / anim-{seq}.gif   {len(cells)} frames")

if strips:
    subprocess.run(["magick", *strips, "-background", "#181818", "-gravity", "west",
                    "-append", str(out / "index.png")], check=True)
PY

echo
echo "$OUT"
