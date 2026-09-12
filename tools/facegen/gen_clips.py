#!/usr/bin/env python3
"""
Bake the artist's expression pack into LVGL clips.

The pack is 29 motion families, 174 drawn frames, in a 200x100 local space with its own
anchors for a 320x240 panel. LVGL is built here without vector graphics, so every frame is
rasterised once to an alpha-only sprite and tinted at draw time -- the same approach as
gen_sprites.py, extended from single poses to timed sequences.

Three things this does that the single-pose baker did not:

  * Splits a frame into colour layers. A talking mouth is a chalk shape with a black
    cutout inside it, and A8 carries no colour, so each colour becomes its own sprite and
    the skin stacks them. Empty layers on a given frame bake to nothing and are skipped.

  * Splits a frame into sides. Eyes, brows and cheeks are drawn as a pair in one file with
    `-left` / `-right` child groups. Baked whole they are mostly transparent and cannot be
    anchored per side, so each side is isolated and placed against its own anchor.

  * Deduplicates. Roughly half the side-drawings repeat -- the two eyes share a path, and
    the plain open shell is the rest frame of most families -- so identical pixel data is
    emitted once and referenced many times.

Timing comes from the artist's timing-and-anchors.json, never from a number typed here.

    ./gen_clips.py            # writes chalk_clips.{h,cpp} into the chalk skin
"""

import collections
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

SVG_NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG_NS)

PACK = pathlib.Path(__file__).resolve().parent / "artwork/pack-2026-09-12"
OUT = pathlib.Path(__file__).resolve().parents[2] / "firmware/main/stackchan/avatar/skins/chalk"

# The pack's own local viewport. Frames are drawn around an origin inside this box.
LOCAL_W, LOCAL_H = 200, 100
ORIGIN_X, ORIGIN_Y = 100, 45      # where the local origin sits inside the viewport

# The artist's palette. Anything drawn in one of these becomes its own alpha layer.
COLORS = {
    "#f4f1e9": ("chalk", "0xF4F1E9"),
    "#000":    ("ink",   "0x000000"),
    "#e67c89": ("pink",  "0xE67C89"),
    "#8cccd4": ("blue",  "0x8CCCD4"),
}

# Gaze is not baked: every gaze frame is the same pupil translated, and the skin already
# moves a pupil continuously, which drawn frames cannot do. The sheet is calibration.
SKIP_COMPONENTS = {"gaze"}


def need(tool):
    if shutil.which(tool) is None:
        sys.exit(f"missing required tool: {tool}")


def c_ident(text):
    return re.sub(r"[^0-9a-zA-Z]+", "_", text).strip("_")


def effective_color(elem, inherited_fill, inherited_stroke):
    """What colour a path actually draws in, honouring the group defaults it sits under."""
    fill = elem.get("fill", inherited_fill)
    stroke = elem.get("stroke", inherited_stroke)
    if fill and fill != "none":
        return fill
    if stroke and stroke != "none":
        return stroke
    return None


def colour_layers(root):
    """Every colour this frame draws in, in a stable order."""
    found = []
    for elem, colour in walk_paths(root):
        if colour and colour not in found:
            found.append(colour)
    # Chalk first, ink last: the cutout has to be stacked over the shape it cuts.
    order = {"#f4f1e9": 0, "#e67c89": 1, "#8cccd4": 2, "#000": 3}
    return sorted(found, key=lambda c: order.get(c, 9))


def walk_paths(node, inherited_fill="none", inherited_stroke="#f4f1e9"):
    """Yield (path element, its effective colour) for the whole subtree."""
    fill = node.get("fill", inherited_fill)
    stroke = node.get("stroke", inherited_stroke)
    for child in node:
        tag = child.tag.split("}")[-1]
        if tag in ("path", "circle", "ellipse", "rect"):
            yield child, effective_color(child, fill, stroke)
        else:
            yield from walk_paths(child, fill, stroke)


def isolate(svg_text, side, colour):
    """A copy of the frame showing only one side and only one colour."""
    root = ET.fromstring(svg_text)

    # Hide the other side, if the frame is a pair.
    if side:
        for elem in root.iter():
            eid = elem.get("id", "")
            if eid.endswith(("-left", "-right")) and eid != side:
                elem.set("display", "none")

    for elem, elem_colour in walk_paths(root):
        if elem_colour != colour:
            elem.set("display", "none")

    return ET.tostring(root, encoding="unicode")


def render(svg_text, workdir, name, scale):
    path = workdir / f"{name}.svg"
    png = workdir / f"{name}.png"
    path.write_text(svg_text)
    subprocess.run(["rsvg-convert", "-w", str(round(LOCAL_W * scale)),
                    "-h", str(round(LOCAL_H * scale)), "-o", str(png), str(path)], check=True)
    return png


def trim_box(png):
    out = subprocess.run(["magick", str(png), "-format", "%@", "info:"],
                         check=True, capture_output=True, text=True).stdout.strip()
    m = re.match(r"(\d+)x(\d+)([+-]\d+)([+-]\d+)$", out)
    if not m:
        return None
    w, h, x, y = (int(v) for v in m.groups())
    return (w, h, x, y) if w and h else None


def alpha_bytes(png, box):
    w, h, x, y = box
    raw = subprocess.run(["magick", str(png), "-crop", f"{w}x{h}+{x}+{y}", "+repage",
                          "-alpha", "extract", "-depth", "8", "gray:-"],
                         check=True, capture_output=True).stdout
    if len(raw) != w * h:
        sys.exit(f"alpha size mismatch for {png}: {len(raw)} != {w * h}")
    return raw


def c_array(data, indent="    "):
    lines = []
    for i in range(0, len(data), 16):
        lines.append(indent + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    return "\n".join(lines)


def main():
    need("rsvg-convert")
    need("magick")

    spec = json.loads((PACK / "timing-and-anchors.json").read_text())
    scale = spec["scale"]
    panel_w, panel_h = spec["canvas"]
    anchors = spec["anchors"]

    # Anchors are given in panel pixels; everything the skin places is measured from the
    # panel centre, so convert once here rather than in the C++.
    def anchor_offset(component, side):
        key = {"eyes": "eyes", "brows": "brows", "cheeks": "cheeks",
               "mouth": "mouth", "accents": "accent-right"}[component]
        ax, ay = anchors[key]
        return ax - panel_w / 2.0, ay - panel_h / 2.0

    side_offsets = {}               # (component, side) -> local translate
    sprites = {}                    # alpha hash -> (symbol, w, h, bytes)
    clips = []                      # (component, family, side, [layers], ms)
    tmp = pathlib.Path(tempfile.mkdtemp())

    for component, families in spec["sets"].items():
        if component in SKIP_COMPONENTS:
            continue
        for family in families:
            fname = family["name"]
            ids = family["ids"]
            ms = family["ms"]

            # Which sides this family has, decided from its first frame.
            first = (PACK / "frames" / component / f"{ids[0]}.svg").read_text()
            sides = sorted(set(re.findall(r'id="([^"]*-(?:left|right))"', first)))
            side_keys = [s.rsplit("-", 1)[1] for s in sides] or [""]

            for side_key in side_keys:
                if side_key and (component, side_key) not in side_offsets:
                    m = re.search(r'transform="translate\(\s*(-?[\d.]+)[\s,]+(-?[\d.]+)\s*\)[^"]*"\s+id="[^"]*-'
                                  + side_key + '"', first)
                    if m:
                        side_offsets[(component, side_key)] = (float(m.group(1)), float(m.group(2)))

                per_colour = collections.OrderedDict()

                for index, frame_id in enumerate(ids):
                    svg = (PACK / "frames" / component / f"{frame_id}.svg").read_text()
                    root = ET.fromstring(svg)
                    side_id = f"{frame_id}-{side_key}" if side_key else None

                    for colour in colour_layers(root):
                        variant = isolate(svg, side_id, colour)
                        tag = c_ident(f"{frame_id}_{side_key}_{COLORS[colour][0]}")
                        png = render(variant, tmp, tag, scale)
                        box = trim_box(png)
                        entry = per_colour.setdefault(colour, [None] * len(ids))
                        if box is None:
                            continue        # nothing of this colour on this frame
                        w, h, x, y = box
                        data = alpha_bytes(png, box)
                        digest = hashlib.sha1(data).hexdigest()
                        if digest not in sprites:
                            sprites[digest] = (f"px_{len(sprites):03d}", w, h, data)
                        ox, oy = anchor_offset(component, side_key)
                        cx = round(x + w / 2.0 - ORIGIN_X * scale + ox)
                        cy = round(y + h / 2.0 - ORIGIN_Y * scale + oy)
                        entry[index] = (digest, cx, cy)

                clips.append((component, fname, side_key, per_colour, ms))

    # The pupil: one sprite, moved continuously by the skin.
    gaze_svg = (PACK / "frames/gaze/gaze-look-left-00.svg").read_text()
    pupil_only = re.sub(r'<use[^>]*/>', "", gaze_svg)
    pupil_only = pupil_only.replace("</defs>", "</defs>"
                                    '<use xlink:href="#pupil-dot" transform="translate(0 0)"/>')
    png = render(pupil_only, tmp, "pupil", scale)
    box = trim_box(png)
    pupil = None
    if box:
        w, h, x, y = box
        data = alpha_bytes(png, box)
        digest = hashlib.sha1(data).hexdigest()
        if digest not in sprites:
            sprites[digest] = (f"px_{len(sprites):03d}", w, h, data)
        pupil = (digest, round(x + w / 2.0 - ORIGIN_X * scale),
                 round(y + h / 2.0 - ORIGIN_Y * scale))

    total = sum(len(v[3]) for v in sprites.values())
    print(f"  {len(clips)} clips, {len(sprites)} unique sprites, {total/1024:.1f} KB")

    anchor_offsets = {c: anchor_offset(c, "") for c in
                      ("eyes", "brows", "cheeks", "mouth", "accents")}
    emit(sprites, clips, pupil, spec, scale, side_offsets, anchor_offsets)


def emit(sprites, clips, pupil, spec, scale, SIDE_OFFSETS, ANCHOR_OFFSETS):
    PUPILS = {("eyes", f["name"]): f["pupil_visibility"]
              for f in spec["sets"]["eyes"] if "pupil_visibility" in f}
    banner = ("/*\n"
              " * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD\n"
              " *\n"
              " * SPDX-License-Identifier: MIT\n"
              " *\n"
              " * GENERATED FILE -- do not edit by hand.\n"
              " * Produced by tools/facegen/gen_clips.py from the artist's expression pack.\n"
              " * Regenerate with:  python3 tools/facegen/gen_clips.py\n"
              " */\n")

    h = [banner, "#pragma once", "#include <lvgl.h>", "", "namespace stackchan::avatar::chalk {", "",
         "/// One drawn frame of one colour layer. A null image means this layer draws",
         "/// nothing on this frame, which is common: a mouth has no dark inside until it opens.",
         "struct ClipFrame {",
         "    const lv_image_dsc_t* dsc;",
         "    int16_t cx;",
         "    int16_t cy;",
         "};",
         "",
         "/// One colour of a clip, across all its frames.",
         "struct ClipLayer {",
         "    const ClipFrame* frames;",
         "    uint32_t color;",
         "};",
         "",
         "/// A drawn motion family: layers to stack, and how long each frame is held.",
         "struct Clip {",
         "    const ClipLayer* layers;",
         "    const uint16_t* holdMs;",
         "    /// Per frame, which pupils may be drawn: bit 0 left, bit 1 right. Null for",
         "    /// anything that is not an eye. The artist gates these because an arc or a",
         "    /// narrowed dome has no white to put a pupil in, and the curious frames drop",
         "    /// one side on purpose.",
         "    const uint8_t* pupils;",
         "    uint8_t layerCount;",
         "    uint8_t frameCount;",
         "};",
         "",
         f"static constexpr float kPackScale = {scale}f;",
         "",
         "/// Whether this eye may draw its pupil on this frame.",
         "inline bool pupilVisible(const Clip* clip, uint8_t frame, bool leftEye)",
         "{",
         "    if (!clip || !clip->pupils || frame >= clip->frameCount) {",
         "        return clip && !clip->pupils;   // non-eye clips do not gate anything",
         "    }",
         "    const uint8_t mask = clip->pupils[frame];",
         "    return leftEye ? (mask & 1u) != 0u : (mask & 2u) != 0u;",
         "}",
         ""]

    c = [banner, '#include "chalk_clips.h"', "", "namespace stackchan::avatar::chalk {", ""]

    for digest, (sym, w, hh, data) in sprites.items():
        c += [f"static const uint8_t {sym}_map[] = {{", c_array(data), "};",
              f"static const lv_image_dsc_t {sym} = {{",
              "    .header = {",
              "        .magic = LV_IMAGE_HEADER_MAGIC,",
              "        .cf = LV_COLOR_FORMAT_A8,",
              "        .flags = 0,",
              f"        .w = {w},",
              f"        .h = {hh},",
              f"        .stride = {w},",
              "        .reserved_2 = 0,",
              "    },",
              f"    .data_size = sizeof({sym}_map),",
              f"    .data = {sym}_map,",
              "    .reserved = NULL,",
              "    .reserved_2 = NULL,",
              "};",
              ""]

    names = []
    for component, family, side, per_colour, ms in clips:
        base = c_ident(f"clip_{component}_{family}" + (f"_{side}" if side else ""))
        names.append(base)

        layer_syms = []
        for colour, frames in per_colour.items():
            cname, chex = COLORS[colour]
            lsym = f"{base}_{cname}"
            rows = []
            for f in frames:
                if f is None:
                    rows.append("    {NULL, 0, 0},")
                else:
                    digest, cx, cy = f
                    rows.append(f"    {{&{sprites[digest][0]}, {cx}, {cy}}},")
            c += [f"static const ClipFrame {lsym}_frames[] = {{", *rows, "};", ""]
            layer_syms.append((lsym, chex))

        pupil_sym = "NULL"
        vis = PUPILS.get((component, family))
        if vis:
            pupil_sym = f"{base}_pupils"
            bits = {"both": 3, "left": 1, "right": 2, "none": 0}
            c += [f"static const uint8_t {pupil_sym}[] = "
                  f"{{{', '.join(str(bits.get(v, 3)) for v in vis)}}};"]

        if component == "mouth":
            # "How open is this mouth" is a measurable property of the drawing: the number
            # of lit pixels across its layers. Ordering the frames by that gives an
            # openness ladder the skin can walk with an amplitude, without anyone having to
            # read the artist's pose names and guess at their ranking.
            areas = []
            for idx in range(len(ms)):
                total = 0
                for frames in per_colour.values():
                    f = frames[idx]
                    if f:
                        total += sum(1 for b in sprites[f[0]][3] if b > 8)
                areas.append((total, idx))
            ladder = [idx for _, idx in sorted(areas)]
            c += [f"static const uint8_t {base}_openness[] = "
                  f"{{{', '.join(str(i) for i in ladder)}}};"]
            h.append(f"extern const uint8_t {base}_openness[];")
            h.append(f"static constexpr uint8_t {base}_opennessCount = {len(ladder)};")
            c[-1] = c[-1].replace("static const uint8_t", "const uint8_t")

        c += [f"static const ClipLayer {base}_layers[] = {{"]
        for lsym, chex in layer_syms:
            c.append(f"    {{{lsym}_frames, {chex}}},")
        c += ["};",
              f"static const uint16_t {base}_ms[] = {{{', '.join(str(int(v)) for v in ms)}}};",
              f"const Clip {base} = {{{base}_layers, {base}_ms, {pupil_sym}, "
              f"{len(layer_syms)}, {len(ms)}}};",
              ""]
        h.append(f"extern const Clip {base};")

    if pupil:
        digest, cx, cy = pupil
        c += [f"const ClipFrame pupil = {{&{sprites[digest][0]}, {cx}, {cy}}};", ""]
        h += ["", "/// The pupil, placed by the skin rather than drawn per frame: every gaze frame",
              "/// in the pack is this same dot translated, and a continuous offset covers all of",
              "/// them plus everything in between.", "extern const ClipFrame pupil;"]

    # Where each side sits inside its pair, read from the artist's own transforms rather
    # than retyped. Decorators anchor against these, so an overlay follows the face.
    h += ["", "/// Anchor points, in pixels from the panel centre."]
    # Eyes carry their offset as a group transform; cheeks and brows bake it into the path
    # data instead, so those anchors are read back from where the art actually landed.
    def baked_centre(component, family, side):
        for comp, fam, sd, per_colour, _ in clips:
            if comp == component and fam == family and sd == side:
                for frames in per_colour.values():
                    for f in frames:
                        if f:
                            return f[1], f[2]
        return 0, 0

    for name, (comp, key) in (("eye_left", ("eyes", "left")), ("eye_right", ("eyes", "right"))):
        lx, ly = SIDE_OFFSETS.get((comp, key), (0.0, 0.0))
        ax, ay = ANCHOR_OFFSETS[comp]
        h.append(f"static constexpr lv_point_t kAnchor_{name} = "
                 f"{{{round(lx * scale + ax)}, {round(ly * scale + ay)}}};")
    for name, (comp, fam, key) in (("cheek_left", ("cheeks", "smile-lift", "left")),
                                   ("cheek_right", ("cheeks", "smile-lift", "right")),
                                   ("brow_left", ("brows", "curiosity", "left")),
                                   ("brow_right", ("brows", "curiosity", "right"))):
        cx, cy = baked_centre(comp, fam, key)
        h.append(f"static constexpr lv_point_t kAnchor_{name} = {{{cx}, {cy}}};")
    ax, ay = ANCHOR_OFFSETS["mouth"]
    h.append(f"static constexpr lv_point_t kAnchor_mouth = {{{round(ax)}, {round(ay)}}};")
    h.append(f"static constexpr int kEyeRadius = {round(18 * scale)};")

    # The gaze limits the artist measured, per eye family. A narrowed eye has less white.
    h += ["", "/// How far the pupil may travel inside each eye family, in local units."]
    for family in spec["sets"]["eyes"]:
        if "gaze_limit" in family:
            gx, gy = family["gaze_limit"]
            h.append(f"static constexpr lv_point_t kGazeLimit_{c_ident(family['name'])} = "
                     f"{{{round(gx * scale)}, {round(gy * scale)}}};")

    # A registry, so anything reviewing the pack iterates what was actually baked rather
    # than a hand-kept list that can fall behind the artwork.
    h += ["", "/// Every baked family, for review tooling.",
          "struct NamedClip {", "    const char* name;", "    const Clip* clip;", "};",
          "extern const NamedClip kAllClips[];",
          f"static constexpr int kAllClipCount = {len(names)};"]
    c += ["const NamedClip kAllClips[] = {"]
    for n in names:
        c.append(f'    {{"{n[5:]}", &{n}}},')
    c += ["};", ""]

    h += ["", "}  // namespace stackchan::avatar::chalk", ""]
    c += ["}  // namespace stackchan::avatar::chalk", ""]

    (OUT / "chalk_clips.h").write_text("\n".join(h))
    (OUT / "chalk_clips.cpp").write_text("\n".join(c))
    print(f"  wrote {OUT/'chalk_clips.h'}")
    print(f"  wrote {OUT/'chalk_clips.cpp'}")


if __name__ == "__main__":
    main()
