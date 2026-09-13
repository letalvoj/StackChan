# Laugh — reference-aligned revision

Six hand-drawn mouth poses: grin, catch, ha, hee, haa, settle. The heavy tooth band has been removed in favor of a simple warm-white outline and a small pink tongue. Peak laugh adds a distinct > < eye squeeze named `eyes-laugh-squeeze-peak`, with independent left/right child IDs. Recovery uses the original smile eyes. Original library definitions remain unchanged.

`laugh-sheet.svg`: reusable definitions and a contact sheet. `frames/`: six standalone mouth SVGs. `laugh-face.svg`: assembled peak pose. `laugh-stop-motion.gif`: held-frame preview. `laugh-timing.json`: exact component IDs, hold durations, anchors and offsets.

Use discrete frame selection, not path morphing. The 2.7-second event contains two uneven bursts and a recovery; the GIF repeats for review. Return to the caller's expression afterward. Mouth anchor (160,161), eyes (160,100), cheeks (160,140), local scale 1.5. Hide pupils during these squeezed-eye drawings. Preserve enclosing presentation attributes when extracting definitions.

SVG Tiny 1.2, local use references, no filters or bitmap textures. SVG structure and renders checked offline; no firmware changes. The previous toothy extension is archived in the parent folder as `laugh-before-reference-revision.zip` and excluded from the current packs.
