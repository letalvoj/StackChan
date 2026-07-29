# facelab — see the avatar without flashing

Renders the **real** avatar skin natively and tiles every emotion into one labelled
contact sheet. It compiles the same `eyes.cpp` / `mouth.cpp` the firmware runs, against the
same vendored LVGL — a preview that does not share the code cannot tell you whether the
face you designed is the face the robot will show.

```bash
./build.sh                    # one-off ~1 min for LVGL, then seconds
./grid.sh reference default   # out/grid_reference.png  — the shipping face
./grid.sh mine cute           # out/grid_mine.png       — the new one
```

Each run covers the six firmware emotions plus blink, half-blink and three mouth openings,
because those are driven by modifiers at runtime and a static emotion grid would miss half
of what the face actually does.

**Adding a skin:** implement it under `firmware/main/stackchan/avatar/skins/<name>/`, then
add a branch in `render_faces.cpp`. Both skins satisfy the same `Avatar` interface, which
is the property the harness exists to check.

Two gotchas worth knowing, both cost time to find:

* Image assets (`*.c`) **must be compiled as C**. `clang++` treats `.c` as C++, where a
  file-scope `const` has internal linkage, so the symbol silently vanishes at link time.
* macOS ships bash 3.2, which has no `wait -n`; parallel compilation uses `xargs -P`.
