#!/usr/bin/env bash
# Build the native face renderer.
#
# Compiles the REAL avatar skin against the REAL vendored LVGL, so what you see is what
# the device draws. Deliberately compiles only the skin and its dependencies -- pulling in
# all of stackchan/ would drag in the HAL, servos and FreeRTOS for no benefit, since a
# static face snapshot needs none of them.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FW="$ROOT/firmware"
LVGL="$FW/managed_components/lvgl__lvgl"
OUT="$HERE/build"

mkdir -p "$OUT"

# LVGL is big; build it once into a static lib and reuse.
if [ ! -f "$OUT/liblvgl.a" ]; then
  echo "building LVGL (one-off, ~1 min) …"
  mkdir -p "$OUT/lvgl"
  find "$LVGL/src" -name '*.c' > "$OUT/lvgl_srcs.txt"
  # xargs -P for parallelism: bash 3.2 ships on macOS and has no `wait -n`.
  xargs -P 8 -I{} sh -c '
      obj="$1/$(echo "${2#$3/src/}" | tr "/" "_" | sed "s/\.c$/.o/")"
      [ -f "$obj" ] || cc -c "$2" -o "$obj" -O2 -w \
          -DLV_CONF_INCLUDE_SIMPLE=1 -I"$4/wasm/shims" -I"$3" -I"$3/src"
  ' _ "$OUT/lvgl" {} "$LVGL" "$ROOT" < "$OUT/lvgl_srcs.txt"
  ar rcs "$OUT/liblvgl.a" "$OUT"/lvgl/*.o
fi

SMOOTH_SRCS=$(find "$FW/components/smooth_ui_toolkit/src" -name '*.cpp')
SKIN_SRCS=$(find "$FW/main/stackchan/avatar/skins" -name '*.cpp')
# Decorators are overlays (heart, blush, sweat, dizzy) drawn on top of the face --
# a third of what the robot displays, and easy to forget because no emotion selects them.
SKIN_SRCS="$SKIN_SRCS $(find "$FW/main/stackchan/avatar/decorators" -name '*.cpp')"
# The speech bubble's arrow is a compiled-in image asset. It MUST be compiled as C:
# clang++ treats .c as C++, where a file-scope `const` has internal linkage, so the
# symbol silently vanishes and the link fails with an undefined reference.
ASSET_OBJS=""
for a in $(find "$FW/main/stackchan/avatar/skins" "$FW/main/stackchan/avatar/decorators" -name '*.c'); do
  o="$OUT/$(basename "$a" .c).o"
  cc -c "$a" -o "$o" -O2 -w -DLV_CONF_INCLUDE_SIMPLE=1 \
     -I"$ROOT/wasm/shims" -I"$LVGL" -I"$LVGL/src"
  ASSET_OBJS="$ASSET_OBJS $o"
done

# shellcheck disable=SC2086
c++ -std=c++17 -O2 -w \
    -DLV_CONF_INCLUDE_SIMPLE=1 \
    -DFIRMWARE_VERSION='"facelab"' \
    -DBOARD_NAME='"facelab"' -DBOARD_TYPE='"facelab"' \
    -I"$ROOT/wasm/shims" -I"$LVGL" -I"$LVGL/src" \
    -I"$FW/components/smooth_ui_toolkit/src" \
    -I"$FW/components/smooth_ui_toolkit/src/lvgl" \
    -I"$FW/components/smooth_ui_toolkit/src/uitk" \
    -I"$FW/main" \
    -I"$FW/xiaozhi-esp32/main" \
    -I"$FW/xiaozhi-esp32/main/display" \
    -I"$FW/xiaozhi-esp32/main/display/lvgl_display" \
    -I"$FW/components/mooncake/src" \
    -I"$FW/components/mooncake_log/src" \
    "$HERE/render_faces.cpp" $SKIN_SRCS $ASSET_OBJS $SMOOTH_SRCS \
    "$OUT/liblvgl.a" \
    -o "$OUT/render_faces"

echo "built $OUT/render_faces"
