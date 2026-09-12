#!/usr/bin/env python3
"""
Bake the chalk face artwork into LVGL A8 sprites.

The source of truth is the artist's robot-face.svg. Every shape there is a stroked or
filled path in a 200x140 face space; LVGL is built here without vector graphics
(CONFIG_LV_USE_VECTOR_GRAPHIC is not set), so paths cannot be drawn at runtime. Instead
each *placed instance* is rasterised once, in face coordinates with its own transform
already applied, and stored as an alpha-only image that the skin tints at draw time.

Baking the placement rather than the bare definition is deliberate: it keeps the port
pixel-honest, because the rotation and scale on each instance come from the SVG rather
than from a constant retyped here. What the skin supplies at runtime is only the *delta* --
gaze, breath, idle drift, mouth opening.

A8 plus recolour works because every shape is a single flat colour. The two-tone parts
(eye and its pupil, open mouth and its inner cutout) are split into separate sprites,
which is required anyway: the pupil has to move independently.

    ./gen_sprites.py            # writes chalk_sprites.{c,h} into the chalk skin

Requires rsvg-convert and ImageMagick, both already used elsewhere in this repo.
"""

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

# The art canvas includes generous empty margins. Enlarge the drawing, not the panel;
# review the actual ink bounds (including moving overlays) before increasing this.
SCALE = 1.75
EYE_WIDTH = 1.4  # Nearly circular open eyes; retain the uneven contour and height.
EYE_LIFT = 4     # Face units: lift eyes/brows together, away from the cheeks.
MOUTH_DROP = 3   # Face units: a little more breathing room below the eyes.
FACE_W, FACE_H = 200, 140
DEV_W, DEV_H = round(FACE_W * SCALE), round(FACE_H * SCALE)
FACE_CX, FACE_CY = DEV_W / 2.0, DEV_H / 2.0

CHALK = "0xF4F1E9"
BLACK = "0x000000"
PINK = "0xE67C89"
BLUE = "0x8CCCD4"

ART = pathlib.Path(__file__).resolve().parent / "artwork/robot-face.svg"
OUT = pathlib.Path(__file__).resolve().parents[2] / "firmware/main/stackchan/avatar/skins/chalk"

def raw(d, **attrs):
    """A path lifted verbatim out of the source SVG.

    Some of the artist's paths bundle a screen-left and a screen-right mark into one `d`
    (the brow pairs, the half-blink lids, the dizzy balance marks). Baked whole they are
    mostly transparent -- the dizzy one alone was 19 KB of empty space -- and worse, a
    single sprite cannot be positioned against a left and a right anchor. Splitting on the
    subpath boundary keeps the exact path data while making each half placeable.
    """
    return {"d": d, "attrs": attrs}


# (sprite name, id to <use> or raw() path, transform or None, colour)
#
# Transforms are copied verbatim from robot-face.svg. Where the same definition is placed
# twice, the first entry is screen-left and the second screen-right, matching the artist
# guide's convention.
SPRITES = [
    # --- eyes: the open egg, per emotion, without its pupil -----------------------
    ("eye_egg_l_neutral",   "shape-eye-open",       "translate(64 66) rotate(-5)",   CHALK),
    ("eye_egg_r_neutral",   "shape-eye-open",       "translate(137 65) rotate(5)",   CHALK),
    ("eye_egg_l_angry",     "shape-eye-open",       "translate(66 70) scale(.9)",    CHALK),
    ("eye_egg_r_angry",     "shape-eye-open",       "translate(135 69) scale(.9)",   CHALK),
    ("eye_egg_l_sad",       "shape-eye-open",       "translate(65 72) scale(.95 1)", CHALK),
    ("eye_egg_r_sad",       "shape-eye-open",       "translate(136 71) scale(.95 1)", CHALK),
    ("eye_egg_l_doubt",     "shape-eye-open",       "translate(64 68) scale(.95)",   CHALK),
    ("eye_half_r_doubt",    "shape-eye-half-open",  "translate(137 66) rotate(-8)",  CHALK),
    ("eye_arc_l_happy",     "shape-eye-happy-arc",  "translate(64 67) rotate(-7)",   CHALK),
    ("eye_arc_r_happy",     "shape-eye-happy-arc",  "translate(137 65) rotate(8)",   CHALK),
    ("eye_closed_l",        "shape-eye-closed",     "translate(64 68) rotate(3)",    CHALK),
    ("eye_closed_r",        "shape-eye-closed",     "translate(137 67) rotate(-5)",  CHALK),
    ("eye_closed_l_sleepy", "shape-eye-closed",     "translate(64 71) rotate(3)",    CHALK),
    ("eye_closed_r_sleepy", "shape-eye-closed",     "translate(137 70) rotate(-5)",  CHALK),
    ("eye_half_l",          "shape-eye-half-open",  "translate(64 63)",              CHALK),
    ("eye_half_r",          "shape-eye-half-open",  "translate(137 62)",             CHALK),
    ("halfblink_lid_l",     raw("M49 59Q62 58 80 60"),   None,                       CHALK),
    ("halfblink_lid_r",     raw("M122 58Q137 58 153 59"), None,                      CHALK),

    # The pupil is baked at the face centre so its recorded offset is measured from the
    # eye anchor rather than from a fixed eye slot -- the skin then moves it for gaze.
    ("pupil",               "shape-eye-glint",      "translate(100 70)",             BLACK),

    # --- brows -------------------------------------------------------------------
    ("brow_angry_l",        raw("M51 48Q61 57 76 57"),   None,                       CHALK),
    ("brow_angry_r",        raw("M125 56Q140 54 149 46"), None,                      CHALK),
    ("brow_sad_l",          raw("M52 52Q64 51 72 43"),   None,                       CHALK),
    ("brow_sad_r",          raw("M127 43Q134 51 148 50"), None,                      CHALK),
    ("brow_doubt_l",        raw("M49 39Q61 31 76 37"),   None,                       CHALK),
    ("brow_doubt_r",        raw("M123 44L150 41"),       None,                       CHALK),

    # --- cheeks ------------------------------------------------------------------
    ("cheek_l",             "shape-cheek-hatching", "translate(44 89) rotate(-5)",   CHALK),
    ("cheek_r",             "shape-cheek-hatching", "translate(157 88) rotate(7)",   CHALK),

    # --- mouths ------------------------------------------------------------------
    ("mouth_neutral",       "mouth-neutral-smile",  None,                            CHALK),
    ("mouth_happy",         "mouth-happy-smile",    None,                            CHALK),
    ("mouth_sad",           "mouth-sad-frown",      None,                            CHALK),
    ("mouth_angry",         "mouth-angry-pout",     None,                            CHALK),
    ("mouth_doubt",         "mouth-doubt-smirk",    None,                            CHALK),
    ("mouth_open_fill",     "shape-mouth-happy-open",   None,                        CHALK),
    ("mouth_open_cut",      "shape-mouth-inner-cutout", None,                        BLACK),
    # Baked at unit scale; the skin scales it for speech.
    ("mouth_oval",          "mouth-speaking-oval",  "translate(101 96)",             CHALK),

    # --- decorations and overlays -------------------------------------------------
    ("sleepy_z",            "decoration-sleepy-z",  None,                            CHALK),
    ("heart",               "shape-heart",          "translate(159 35) rotate(15)",  PINK),
    ("heart_sparkles",      "decoration-heart-sparkles", None,                       PINK),
    ("blush_l",             "shape-blush-hatching", "translate(44 89) rotate(-6)",   PINK),
    ("blush_r",             "shape-blush-hatching", "translate(157 88) rotate(6)",   PINK),
    ("angrymark",           "overlay-angrymark",    None,                            PINK),
    ("sweat",               "overlay-sweat",        None,                            BLUE),
    # Spun at runtime by the dizzy decorator, so it is baked upright-as-drawn and
    # rotated about its own centre.
    ("dizzy_spiral_l",      "shape-eye-dizzy-spiral", "translate(64 67) rotate(-12)", CHALK),
    ("dizzy_spiral_r",      "shape-eye-dizzy-spiral", "translate(137 70) rotate(168) scale(.95)", CHALK),
    ("dizzy_mouth",         raw("M87 99Q91 92 96 99T105 99T115 100", **{"stroke-width": "3.6"}), None, CHALK),
    ("dizzy_wobble_l",      raw("M31 56Q25 64 29 71", **{"stroke-width": "3.6"}),    None,        CHALK),
    ("dizzy_wobble_r",      raw("M168 70Q174 77 170 83", **{"stroke-width": "3.6"}), None,        CHALK),
]

# Anchor points in face space, lifted from the transforms above. The skin exposes these so
# decorators can position against the face they are actually drawn on (see AVATAR_TODO A1).
ANCHORS = {
    "eye_l":   (64, 66),
    "eye_r":   (137, 65),
    "cheek_l": (44, 89),
    "cheek_r": (157, 88),
    "mouth":   (101, 96),
}
# Half-width of the open eye in face units, for scaling anything that must cover it.
EYE_RADIUS_FACE = 13 * EYE_WIDTH


def need(tool):
    if shutil.which(tool) is None:
        sys.exit(f"missing required tool: {tool}")


def extract_defs(svg_text):
    m = re.search(r"<defs\b.*?</defs>", svg_text, re.S)
    if not m:
        sys.exit("no <defs> block in the source SVG")
    return m.group(0)


def tuned_defs(svg_text, layout=False):
    """Keep the artist's paths and IDs; widen the open-eye shell about its local origin."""
    ET.register_namespace("", "http://www.w3.org/2000/svg")
    ET.register_namespace("xlink", "http://www.w3.org/1999/xlink")
    root = ET.fromstring(svg_text)
    eye = next(e for e in root.iter() if e.get("id") == "shape-eye-open")
    eye.set("transform", f"scale({EYE_WIDTH} 1)")
    defs = next(e for e in root if e.tag.endswith("}g"))
    defs = next(e for e in defs if e.tag.endswith("}defs"))
    if layout:
        # Apply the same placement changes to the complete vector reference. Move placed
        # eyes, not their local shapes, so rotations/scales do not skew the vertical lift.
        href = "{http://www.w3.org/1999/xlink}href"
        eye_refs = {"#feature-eye-open", "#shape-eye-happy-arc", "#shape-eye-closed",
                    "#shape-eye-half-open", "#shape-eye-dizzy-spiral"}
        brow_ids = {"brows-angry", "brows-sad", "brows-doubt", "feature-halfblink-lids"}
        mouth_ids = {"mouth-neutral-smile", "mouth-happy-smile", "mouth-sad-frown",
                     "mouth-angry-pout", "mouth-doubt-smirk", "shape-mouth-happy-open",
                     "shape-mouth-inner-cutout"}
        for e in list(defs.iter()):
            dy = -EYE_LIFT if e.get(href) in eye_refs or e.get("id") in brow_ids else 0
            if e.get("id") in mouth_ids or e.get(href) == "#mouth-speaking-oval":
                dy = MOUTH_DROP
            if dy:
                e.set("transform", f"translate(0 {dy}) " + e.get("transform", ""))
            if e.get("id") == "feature-dizzy-mouth-and-wobble":
                mouth, left, right = re.findall(r"M[^M]+", e.attrib.pop("d"))
                e.tag = "{http://www.w3.org/2000/svg}g"
                for d, offset in ((mouth, MOUTH_DROP), (left + right, -EYE_LIFT)):
                    ET.SubElement(e, "{http://www.w3.org/2000/svg}path",
                                  {"d": d, "transform": f"translate(0 {offset})"})
    return ET.tostring(defs, encoding="unicode")


def vertical_offset(name):
    if name.startswith(("eye_", "brow_", "halfblink_", "dizzy_spiral", "dizzy_wobble")):
        return -EYE_LIFT
    if name.startswith("mouth_") or name == "dizzy_mouth":
        return MOUTH_DROP
    return 0


def render(defs, use_id, transform, workdir, name):
    """Rasterise one placed instance on the scaled, transparent face canvas."""
    xform = f' transform="{transform}"' if transform else ""
    if isinstance(use_id, dict):
        extra = "".join(f' {k}="{v}"' for k, v in use_id["attrs"].items())
        element = f'<path d="{use_id["d"]}"{extra}{xform} />'
    else:
        element = f'<use xlink:href="#{use_id}"{xform} />'
    doc = (
        '<svg xmlns="http://www.w3.org/2000/svg" '
        'xmlns:xlink="http://www.w3.org/1999/xlink" version="1.2" baseProfile="tiny" '
        f'width="{FACE_W}" height="{FACE_H}" viewBox="0 0 {FACE_W} {FACE_H}">'
        '<g fill="none" stroke="#f4f1e9" stroke-width="4.4" stroke-linecap="round" '
        'stroke-linejoin="round">'
        f"{defs}"
        f"{element}"
        "</g></svg>"
    )
    svg_path = workdir / f"{name}.svg"
    png_path = workdir / f"{name}.png"
    svg_path.write_text(doc)
    subprocess.run(
        ["rsvg-convert", "-w", str(DEV_W), "-h", str(DEV_H), "-o", str(png_path), str(svg_path)],
        check=True,
    )
    return png_path


def trim_box(png_path):
    """Bounding box of the non-transparent pixels, as (w, h, x, y)."""
    out = subprocess.run(
        ["magick", str(png_path), "-format", "%@", "info:"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    m = re.match(r"(\d+)x(\d+)([+-]\d+)([+-]\d+)$", out)
    if not m:
        return None
    w, h, x, y = (int(v) for v in m.groups())
    return (w, h, x, y) if w and h else None


def alpha_bytes(png_path, box):
    w, h, x, y = box
    raw = subprocess.run(
        ["magick", str(png_path), "-crop", f"{w}x{h}+{x}+{y}", "+repage",
         "-alpha", "extract", "-depth", "8", "gray:-"],
        check=True, capture_output=True,
    ).stdout
    if len(raw) != w * h:
        sys.exit(f"alpha extraction size mismatch for {png_path}: {len(raw)} != {w*h}")
    return raw


def c_array(data, indent="    "):
    lines = []
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i:i + 16])
        lines.append(indent + chunk + ",")
    return "\n".join(lines)


def main():
    need("rsvg-convert")
    need("magick")
    defs = tuned_defs(ART.read_text())
    OUT.mkdir(parents=True, exist_ok=True)

    baked = []
    with tempfile.TemporaryDirectory() as td:
        workdir = pathlib.Path(td)
        for name, use_id, transform, color in SPRITES:
            dy = vertical_offset(name)
            if dy:
                transform = f"translate(0 {dy}) " + (transform or "")
            png = render(defs, use_id, transform, workdir, name)
            box = trim_box(png)
            if box is None:
                sys.exit(f"sprite '{name}' rendered empty -- check id '{use_id}'")
            w, h, x, y = box
            data = alpha_bytes(png, box)
            # Centre of the sprite relative to the face centre, in device pixels.
            cx = round(x + w / 2.0 - FACE_CX)
            cy = round(y + h / 2.0 - FACE_CY)
            baked.append(dict(name=name, w=w, h=h, cx=cx, cy=cy, color=color, data=data))
            print(f"  {name:22s} {w:3d}x{h:<3d}  centre ({cx:+4d},{cy:+4d})  {len(data):6d} B")

    total = sum(len(b["data"]) for b in baked)
    print(f"\n  {len(baked)} sprites, {total} bytes of pixel data")

    banner = (
        "/*\n"
        " * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD\n"
        " *\n"
        " * SPDX-License-Identifier: MIT\n"
        " *\n"
        " * GENERATED FILE -- do not edit by hand.\n"
        " * Produced by tools/facegen/gen_sprites.py from the artist's robot-face.svg.\n"
        " * Regenerate with:  python3 tools/facegen/gen_sprites.py\n"
        " */\n"
    )

    # ---------------------------------------------------------------- header
    h_lines = [banner, "#pragma once", "#include <lvgl.h>", "", "namespace stackchan::avatar::chalk {", ""]
    h_lines += [
        "// Scaled 200x140 art; offsets are device pixels relative to the 320x240 panel centre.",
        f"static constexpr float kArtScale = {SCALE}f;",
        f"static constexpr int kFaceWidth  = {DEV_W};",
        f"static constexpr int kFaceHeight = {DEV_H};",
        "",
        "/// Where a sprite sits, relative to the panel centre, when nothing has moved it.",
        "struct SpriteSlot {",
        "    const lv_image_dsc_t* dsc;",
        "    int cx;",
        "    int cy;",
        "    uint32_t color;",
        "};",
        "",
    ]
    for b in baked:
        h_lines.append(f"extern const SpriteSlot {b['name']};")
    h_lines += ["", "// Anchor points the skin publishes so decorators can position against this face."]
    for key, (fx, fy) in ANCHORS.items():
        fy += vertical_offset(key + "_")
        dx = round(fx * SCALE - FACE_CX)
        dy = round(fy * SCALE - FACE_CY)
        h_lines.append(f"static constexpr lv_point_t kAnchor_{key} = {{{dx}, {dy}}};")
    h_lines.append(f"static constexpr int kEyeRadius = {round(EYE_RADIUS_FACE * SCALE)};")
    h_lines += ["", "}  // namespace stackchan::avatar::chalk", ""]
    (OUT / "chalk_sprites.h").write_text("\n".join(h_lines))

    # ---------------------------------------------------------------- source
    c_lines = [banner, '#include "chalk_sprites.h"', "", "namespace stackchan::avatar::chalk {", ""]
    for b in baked:
        n = b["name"]
        c_lines += [
            f"static const uint8_t {n}_map[] = {{",
            c_array(b["data"]),
            "};",
            f"static const lv_image_dsc_t {n}_dsc = {{",
            "    .header = {",
            "        .magic = LV_IMAGE_HEADER_MAGIC,",
            "        .cf = LV_COLOR_FORMAT_A8,",
            "        .flags = 0,",
            f"        .w = {b['w']},",
            f"        .h = {b['h']},",
            f"        .stride = {b['w']},",
            "        .reserved_2 = 0,",
            "    },",
            f"    .data_size = sizeof({n}_map),",
            f"    .data = {n}_map,",
            "    .reserved = NULL,",
            "    .reserved_2 = NULL,",
            "};",
            f"const SpriteSlot {n} = {{&{n}_dsc, {b['cx']}, {b['cy']}, {b['color']}}};",
            "",
        ]
    c_lines += ["}  // namespace stackchan::avatar::chalk", ""]
    (OUT / "chalk_sprites.cpp").write_text("\n".join(c_lines))

    print(f"  wrote {OUT/'chalk_sprites.h'}")
    print(f"  wrote {OUT/'chalk_sprites.cpp'}")


if __name__ == "__main__":
    main()
