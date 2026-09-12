# facelab — see the avatar without flashing

Renders the **real** avatar skin natively and tiles every state into one labelled contact
sheet. It compiles the same `eyes.cpp` / `mouth.cpp` the firmware runs, against the same
vendored LVGL — a preview that does not share the code cannot tell you whether the face you
designed is the face the robot will show.

```bash
./build.sh                    # one-off ~1 min for LVGL, then seconds
./grid.sh mine chalk          # out/grid_mine.png     — the shipping face
./grid.sh ref default         # out/grid_ref.png      — the legacy face, to diff against
./compare.sh chalk            # out/compare_chalk.png — ours against the artist's SVG
```

`compare.sh` is the one that answers "did the port drift". It renders each `face-*` straight
out of the artist's repository-local `tools/facegen/artwork/robot-face.svg` with the size and eye-width settings the sprite baker uses and stacks it
directly above our frame. A misaligned pair is a bug you can see.

## What it covers

The six firmware emotions, the modifier states on top of them (blink, half-blink, three
mouth openings, happy-talk), the six decorator overlays, and — because the face spends most
of its life here and none of it used to be reviewed — idle gaze, idle drift, breath, the
sleepy speech bubble, and the panic dance pose.

## Two rules this tool exists to enforce

**Weights come from `stackchan/face_states.h`, never from a number typed here.** The harness
used to carry its own copies and they drifted: it drew blinks at weight 0 where the blink
modifier closes to 25, and speaking mouths at 30 and 100 where the modifier only emits
0–20 and 40–80. Nine of eighteen tiles were showing states the device could not reach.

**Every shot resets the face first.** Nothing used to, so one tile with an open mouth
silently opened the mouth on every tile after it. That is how a head-pet tile ended up
showing a mouth the device never opens while being petted.

## Gotchas, all of which cost time to find

* The clock is virtual and advances in lockstep with the LVGL tick. When the HAL stub read
  CPU time instead, decorator animation landed on a different frame every run and two grids
  of the same commit would not diff.
* Image assets in `*.c` **must be compiled as C**. `clang++` treats `.c` as C++, where a
  file-scope `const` has internal linkage, so the symbol silently vanishes at link time.
  The generated chalk sprites are `.cpp` and declare `extern` in their header, which is the
  other way to satisfy this.
* macOS ships bash 3.2, which has no `wait -n`; parallel compilation uses `xargs -P`.

## Adding a skin

Implement it under `firmware/main/stackchan/avatar/skins/<name>/`, override `anchors()` and
`overlayArt()` so decorators land on your face rather than someone else's, then add a branch
in `render_faces.cpp`. Every skin satisfies the same `Avatar` interface, which is the
property the harness exists to check as much as the drawing is.

The chalk skin's artwork is generated: `python3 tools/facegen/gen_sprites.py` bakes each
placed path from the artist's SVG into an alpha sprite. Do not hand-edit `chalk_sprites.*`.

Sizing and roundness are configured in `tools/facegen/gen_sprites.py`; see `tools/facegen/README.md`. The reference comparison applies both settings. Additional `check-*.bmp` frames cover moving decorators and extreme gaze.
