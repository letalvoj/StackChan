# Chalk artwork and device sizing

`artwork/robot-face.svg` is a repository-local copy of the approved, named artist SVG. The generator no longer depends on a file inside the artist's Codex session.

Tune these constants in `gen_sprites.py`:

- `SCALE = 1.75`: about 9% larger than the previous 1.6 layout. The 200 × 140 art canvas has empty margins; its scaled canvas is larger than the screen, but the actual drawing remains inside the 320 × 240 panel. Judge ink bounds, not the source canvas bounds.
- `EYE_WIDTH = 1.4`: widens only the open eye shell around its local origin. At the chosen display scale, eyes are approximately 56 × 55 pixels. Height, irregular contour, placement, and movable pupils remain intact.

- `EYE_LIFT = 4`: lifts eyes, brows, and dizzy balance marks by 7 display pixels.
- `MOUTH_DROP = 3`: lowers mouths by about 5 display pixels; cheeks stay in place.

`gen_sprites.py` applies these settings and regenerates the sprite pixels, centers, eye radius, and anchors together. Never hand-edit `chalk_sprites.h` or `chalk_sprites.cpp`.

```bash
python3 tools/facegen/gen_sprites.py
cd tools/facelab
./build.sh
./grid.sh rounder chalk
./compare.sh chalk
```

The comparison renders the artist's vector paths with the same explicit size and eye-width adjustments. The bottom of each pair is the real C++ skin through LVGL. Pupils are intentionally centered by the runtime instead of pinned up-left as in the original artwork; the dizzy right eye also has its runtime rotation phase. RGB565 quantization, pixel placement, and image scaling can differ slightly from the SVG renderer.

Extra `build/frames/check-*.bmp` frames cover all four extreme gaze corners, twelve dizzy rotation steps, both heart wobble poses, and sweat motion. They do not clutter the main contact sheet.
