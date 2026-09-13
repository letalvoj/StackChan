# Avatar / facelab — what was wrong and what was done

All items closed. Kept as the record of why the face stack looks the way it does now.

The new face is the artist's chalk drawing, ported from `robot-face.svg`. Proof that the
port matches is `tools/facelab/out/compare_chalk.png`: each tile shows the artist's SVG
directly above the frame the real skin renders through LVGL.

---

## A. Firmware — done

### A1. Decorator geometry was hardcoded to the default skin
All five decorators carried file-scope constants tuned for the original 16 px eyes: dizzy
at ±70/-16, which was a byte-for-byte copy of `default/eyes.cpp`'s eye slots, plus shy,
heart, angry and sweat. Any second skin got its overlays in the first skin's positions.

**Fixed** by `Avatar::anchors()` and `Avatar::overlayArt()`. A skin declares where its eyes,
cheeks and mouth are, and what artwork an overlay uses; the decorator contributes only the
animation. The default skin declares its original numbers, so it renders unchanged.

The five decorator classes collapsed into one `SpriteDecorator` plus five one-line
subclasses that pick a set of overlays and a motion, because once placement and artwork
came from the skin they differed in nothing else.

### A2. Shake hides the skin's eyes — kept, deliberately
The spirals stand in for the eyes rather than sitting over them. That substitution is the
effect and it stays. The defect was only that the spirals landed on the old eye slots;
they now anchor to whichever skin is worn. The chalk skin also supplies a wavy dizzy mouth
and balance marks, so the shake handler hides the real mouth when a skin has one rather
than rocking a second mouth underneath it.

### A3. Double blush during a shake
`skinDrawsBlush` on `FaceAnchors`. A skin that blushes on its own suppresses the shy
overlay. The chalk skin sets it false, because the reference deliberately draws the pink
blush over the chalk cheek marks on the shy and headpet faces.

### A4. Laughing
Left alone. `laughing` still maps to Happy and `crying` to Sad in `avatar_controller.cc`.
Adding a seventh emotion means new artwork, which is the artist's call, not a port's.

---

## B. facelab — the tiles that lied, all fixed

### B1. Mouth weight leaked across shots
Nothing reset feature state between tiles, so `happytalk`'s open mouth carried into all six
decorator tiles. Every shot now starts from a known face: emotion, visibility, position,
rotation, gaze and both weights are reset before the shot is applied.

### B2. Blink weight was wrong
The harness drew blink at 0 and halfblink at 32; the modifier closes to 25. Both now come
from `stackchan/face_states.h`, which the blink modifier itself uses. The blink also gained
a genuine half-closed step on the way down, so the artist's halfblink drawing is a state
the device actually passes through instead of a picture nothing produced.

### B3. Two of three speaking tiles were unreachable
30 sat in the dead zone between the closed band (0-20) and the open band (40-80); 100 was
above the cap. The three tiles are now the three openings the artwork defines, and the skin
interpolates between them for every weight in between. The speaking modifier's own bands
come from the same shared header.

### B4. The dizzy tile did not match the device
It composited spirals over visible eyes, a state that cannot occur, which is exactly why
the misplacement in A1 was invisible on the grid and only showed up on hardware. The tile
now reproduces the shake handler's real sequence.

### B5. Runs were not reproducible
The HAL stub returned CPU time from `clock()` while LVGL advanced on a fixed tick, so
decorator animation phase differed every run. One virtual clock now drives both.

---

## C. Coverage — added

`gaze-left`, `gaze-up`, `idle-drift`, `breath`, `sleepy-speech` and `dance-panic`. The idle
and breath states are most of what the face does in practice and none of it had ever been
reviewed; sleepy always carries a "Zzz..." bubble on the device, so the plain sleepy tile
was only half the state.

---

## D. Structure

`stackchan/face_states.h` holds the weights the runtime produces, with no dependencies, so
the harness and the modifiers share them instead of keeping copies that drift. The full
extraction described earlier — a fake `Modifiable` driving the real modifier objects —
was not needed once the constants were shared and state was reset per shot; it remains the
right move if the harness ever needs to test timing rather than appearance.

---

## E. Which skin is flashed — answered

`hal/board/stackchan_display.cc:265`, and it constructed `CuteAvatar`. Earlier greps missed
it because the file is `.cc` and the search covered `.cpp` and `.h`. That is why every app
appeared to build the default face while the device was plainly wearing another one.

It is now `ChalkAvatar`, and the comment there says it is the only place the face is chosen.
The apps build their own avatars for their own screens and were switched too. The cute skin
is deleted; `default` remains as the legacy reference to diff against.

---

## New: pupils

The artwork draws the pupil high and to the left, which reads as permanently looking away.
`Feature::setGaze` is a new axis, distinct from `setPosition`: position slides the whole eye
across the face, gaze moves only the pupil inside it. A zero gaze is dead centre.

`IdleExpressionModifier` drives it. It used to slide both eyes bodily around on a timer,
which reads as the face drifting rather than as the robot looking at something. It now
holds mostly still and moves the pupils instead:

* **Saccades.** Eyes jump, they do not drift. Most targets are near centre so the robot
  reads as facing whoever is in front of it, with occasional wider glances and the rare
  look up or down.
* **The head.** When the head turns, the eyes swing the other way and catch up, which is
  what real eyes do to hold a fixed point while the head moves. Large jumps are treated as
  commanded moves and ignored, so the pupils are not pegged to the edge of the eye.

---

## Not done, and why

`make wasm` fails to link on six HAL symbols — battery voltage, charge phase, wifi link
state, gateway link, sensor events. The WASM HAL is from 27 July; the status-bar work that
needs them landed 2 September and never got WASM counterparts. Nothing here touches them.
Fixing it means writing six stubs, which is a separate change.

## 9 September — rounder, fuller face refinement

The earlier chalk port was still uncommitted. Preserved it, including the idle gaze-write
guard, and tuned the bake to `SCALE = 1.75` and `EYE_WIDTH = 1.4`. Open eyes are now about
56 × 55 pixels instead of 37 × 51; art spacing is about 9% larger. The original SVG is
copied into `tools/facegen/artwork/robot-face.svg` so regeneration no longer requires the
artist's Codex directory. `compare.sh` reads that source and both tuning constants.

The native renderer now also exports twelve dizzy rotation steps, two heart wobble
frames, sweat motion, and all four full-gaze corners. Reviewed 44 frames: face art has
at least a 6 px screen margin. The existing sleepy speech bubble still reaches the bottom
edge as it did before; it is not part of the enlarged face art. Proofs are under
`tools/facelab/out/{before-after-rounder,motion-rounder,compare_chalk}.png` and
`rounder-bounds.json`.

Found and corrected another handoff discrepancy: the preview opened the headpet mouth,
but `HeadPetModifier` only selected Happy. It now applies the shared medium mouth weight
on petting and restores the previous weight unless another modifier has changed it.

Final spacing pass: `EYE_LIFT = 4` and `MOUTH_DROP = 3` open roughly 12 display pixels of extra eye-to-mouth spacing while leaving the cheeks in place. Eye and mouth anchors follow the same offsets as the sprites, and the vector comparison applies the same layout.

Final ESP32 build succeeded and the firmware was flashed on 9 September using esptool
on `/dev/cu.usbmodem101`; every written region passed hash verification. Flash used
`--after no_reset`, leaving the robot in the bootloader for the requested physical short
reset. Device screenshot verification awaits that reboot. The first default macOS `say`
voice stalled; retried the reboot announcement with the built-in Samantha voice.
Changes remain in the workspace, uncommitted.

## 12 September — the artwork source has forked

Verified by baking directly from the artist's `emotion-grid.svg` and diffing the result
against the sprites the device is running. **39 of 47 sprites are byte-identical.** The
shipping binary is the emotion-grid artwork; the two artist files differ only in the id on
their `<defs>` wrapper and their `face-*` definitions are the same.

The other 8 differ for one reason, and it is a real hazard rather than an art difference:

| Sprite | Cause |
|---|---|
| `pupil` | the artist file bakes `translate(2.1 7.4)` to centre it in the widened eye; we centre it at runtime in `eyes.cpp` |
| 6 mouths + `mouth_open_cut` | the artist file bakes `translate(0 3)`; `gen_sprites.py` also applies `MOUTH_DROP = 3` |

So the tuning now exists twice. The department regenerated the Codex files on 9 September
with the widening and offsets baked into the paths, while `tools/facegen/artwork/` holds the
untuned original that `gen_sprites.py` tunes programmatically. Each path alone produces the
right face. Combining them double-applies and drops the mouth about 5 px.

It already bites: pointing `compare.sh` at the artist's current file crashes, because
`tuned_defs` tries to split a `feature-dizzy-mouth-and-wobble` the department has already
split.

**Decision needed.** Either refresh `tools/facegen/artwork/robot-face.svg` from the artist's
new file and delete the widening and offset steps from `gen_sprites.py`, making the artist
file the single source of truth; or keep the programmatic tuning and note that the Codex
directory has diverged. The first is better — two tuning paths is exactly the drift the
compare sheet exists to catch.

Current proof against the shipping face: `tools/facelab/out/compare_emotion_grid.png`.

## 12 September — the face layer moved into the firmware

The preview was a second implementation of the face wearing a test-harness hat. It owned a
`Pose` type describing a whole face, an `applyPose` that reset and applied it, a `Deco` enum
with its own decorator factory, its own 320x240 constants, and a `PreviewChan` that
reimplemented `StackChan::update()`. Every one of those was a copy of something the runtime
already did, and copies drift: the preview's head-pet drew a different overlay set than the
modifier's, and its dizzy tile was missing the blush the device shows.

What was actually blocking sharing was the shape of the runtime, not the harness.

### Modifiable handed out hardware
`Modifiable::motion()` returned a concrete `Motion`, which owns two `Servo`s, which reach the
bus. Anything wanting to drive a modifier had to link the servo stack, so the preview
reimplemented the modifiers instead. There is now `motion::MotionControl`, a behaviour-only
interface — angles, speeds, locks — that `Motion` implements. `StackChan` returns the
concrete type through a covariant override, so the hardware callers are untouched. The
preview's stub head is twenty lines.

`StackChan` also owned its two neon lights *by value*, the same coupling one layer down.
They are attached by the board now, defaulting to a `NullNeonLight`.

### The shared layer
* `face_states.h` — the panel size, alongside the weights the runtime produces. Both skins
  and the preview had been hardcoding 320x240 separately.
* `face_state.h/.cpp` — `FaceState`, the whole-face value, and `applyTo`. Also `Reaction`,
  which names what a reaction *means* rather than which overlays it happens to use, so
  `ImuEventModifier`, `HeadPetModifier` and the preview all ask for the same thing.
* `face_scene.h/.cpp` — `createAvatar`, which owns skin choice and panel construction,
  replacing six copies of "make_unique then init"; `onFaceTapped`, because tap-to-talk is
  the device's only control and the full-screen-panel hazard deserves one owner; and
  `measureMargins`, so "does the art fit the screen" has an answer rather than an eyeball.
* `Avatar::resetFeatures()` and `Avatar::panel()` on the base, both of which several call
  sites were reconstructing by hand.

### Result
The preview now owns zero lines of face logic. What is left in it is a stub head, a stub
light, an LVGL host display and a BMP writer — the things that genuinely differ between a
desktop and a robot. It drives the real `BlinkModifier`, `SpeakingModifier`,
`BreathModifier` and `IdleExpressionModifier`.

### Still coupled, and the next slice
`StackChan` includes `json_helper.h`, whose motion path takes a concrete `Motion*` for servo
configuration, so linking the real `StackChan` still drags in servos. Moving the settings
JSON out of `StackChan` into something the board owns would let the preview drive the actual
runtime object rather than a stand-in.

## 12 September — the expression pack is in

All 29 families, 174 drawn frames, baked and playing.

**Bake.** `tools/facegen/gen_clips.py` reads the artist's `timing-and-anchors.json` and
produces `chalk_clips.{h,cpp}`: 39 clips over 162 unique sprites, 229 KB, against 1.35 MB
of free app partition. It splits each frame by colour, because a talking mouth is a chalk
shape with a black cutout and A8 carries none; splits pairs into sides, because eyes, brows
and cheeks are drawn together but anchored apart; and deduplicates, because about half the
side-drawings repeat. Timings, anchors, per-frame pupil gating and gaze limits all come
from the artist's metadata rather than being retyped.

**Runtime.** `ClipTrack` plays a family, stacking a sprite per colour layer.
`chalk_palette.cpp` maps each emotion to a family per track — eyes, resting mouth, speech
mouth, brows, cheeks — plus the frame that is that emotion's settled look, its gaze limit,
and an openness ladder through its speech poses.

The big behavioural change is talking. It used to be one oval scaled up, so every emotion
looked identical the moment the mouth opened. Each emotion now speaks through its own drawn
poses and keeps its corners: angry stays pressed and tense, sad stays low-cornered, happy
opens into a grin. `matrix-mouth.png` is that change in one image.

`Reaction` picks up the pack's accents, so shake and head-pet draw the new heart, spirals,
sweat and anger mark and stay in step with the preview.

**Export.** `sheets.sh` now emits, from one render pass: five combination matrices, fifteen
behaviour strips and GIFs driven by the real modifiers, and a strip plus GIF for every one
of the 39 baked families — iterated from a registry the generator emits, so a family the
artist adds cannot be silently missed. 114 artefacts, 5.8 MB.

**Dropped.** The old `chalk_sprites.*` bake and the sleepy "Z", which this pack does not
draw. Gaze is deliberately not baked: every gaze frame is the same pupil translated, and
the continuous offset covers those plus everything between them.

**Not done.** Phase 3, the director: the pack's suggested performances as scripted
sequences across tracks with deliberate offsets.

## 12 September — measured against Astra's own performance

`independent-motion.gif` is six faces running side by side for 5.4 s. `tools/facelab/astra.sh`
coalesces it, renders the same six panels at the same cadence, and puts the two grids next
to each other at matching moments. Eyeballing a still could not settle whether we had the
artist's nuance; measuring mouth activity across the whole performance could.

Five real defects came out of that loop, none of which was visible in a static sheet.

**The resting mouth never moved.** Standard deviation of mouth ink was 0.0000 on idle and
curious, against the artist's 0.0017 and 0.0039. We held frame 0 of the quiet family
forever, but the artist marks those families "occasional, not continuous". A resting mouth
now plays its family through once every few seconds, jittered, and settles back. Idle now
measures 0.0440/0.0013 against their 0.0443/0.0017.

**A talking mouth fell out of its own family.** The speaking modifier shuts the jaw between
syllables into 0..20, and anything under 25 was treated as "at rest", so a talking face
flickered back to the emotion's resting shape on every closed beat and lost the corners the
speech family exists to hold. Any weight above zero is now a speaking mouth.

**The widest pose was unreachable.** The openness ladder was indexed over 0..100 while the
jaw tops out at 80, so the widest drawing in every family never appeared during speech.

**The ladders themselves were guesses.** They had been written by reading the artist's pose
names, and that reading was wrong: it ranked the grumpy "tight teeth" pose mid-way when it
is near the widest. The generator now measures each frame's lit area and emits the ordering,
so nobody has to rank poses by name.

**The jaw was open exactly half the time.** A strict 50/50 alternation reads as flapping.
Closed beats are now 40% of the length of open ones, which is both closer to speech and to
the reference.

Resulting ratio of our mouth activity to the artist's, per panel: idle 0.99, curious 0.97,
delighted 1.05, grumpy 0.96, sad 1.14, petted 1.09.

Frame choices also corrected against the reference: grumpy keeps its pupils under the lid
rather than squeezing shut, curious rests symmetric with both brows arched rather than on
the asymmetric reaction frame, sad rests heavy but open, and delighted rests on its own wide
smile rather than the small neutral one.

## 12 September — a review pass against the artist's own sheets

Re-reviewing the whole uncommitted tree, with the pack's sheets open beside it, turned up
four more. Two of them were mine, introduced by the previous pass.

**The delighted face wore two eyebrows.** A squeezed happy eye is already an arc, and the
soft-delight brow drew a second arc directly above it. `composed-expressions.png` draws the
delighted face with one arc per eye and nothing over it, so happy now draws no brows. That
leaves `brows-soft-delight` baked but unreferenced, which is the correct outcome: the pack
draws it for a face whose eyes are open, and ours are not.

**Curiosity had been flattened into symmetry.** The previous pass moved both the eye and the
brow rest frames off their asymmetric poses, reasoning that asymmetry was a reaction rather
than a rest. That reasoning was wrong, and the artist's own sheet says so plainly: "curious /
what was that?" is drawn with a high arched brow over a normal left eye and a flat brow over
a widened right one. That is brow frame 02 and eye frame 02, which is where they are now.
The lesson is the one this file keeps re-learning -- check the reference, do not reason about
what the reference probably meant.

**A silent face was mouthing words to itself.** The resting-mouth fidget played `mouthRest`,
and happy, grumpy and sad all rest on a frame of their *speech* family. Every speech row in
the pack is labelled "audio-driven"; playing one while nothing is being said is using the
wrong drawing. There is now a separate `mouthIdle`, set only where the pack actually draws a
quiet mouth -- quiet-smile for neutral, thinking for curious, the yawn for sleepy -- and null
elsewhere, where the face simply holds still.

**One speech beat in twenty-one dropped out of its own family.** The closed band was 0..20
inclusive, and a skin reads weight zero as "not speaking", so a closed beat that rolled a
literal 0 snapped the mouth back to the emotion's resting shape for that beat. The band now
starts at 1. This is the same bug as the `kOpenAbove` threshold from the previous pass,
surviving at the boundary: the fix then was to treat any weight above zero as speech, which
left zero itself still reachable by the thing producing the weights.

Measured against the reference again afterwards, mouth activity per panel: idle 0.98,
curious 0.97, delighted 1.03, grumpy 1.02, sad 1.10, petted 1.02 -- every panel at or closer
to the reference than before. Curious is the one that moved most: its mouth varied a third as
much as the artist's, and now varies about four fifths as much.

## 13 September — a real laugh

"laughing" was a second spelling of "happy": `avatar_controller.cc` mapped both to
`Emotion::Happy`, so a client that asked the face to laugh got a wide smile. There is now an
`Emotion::Laugh`, baked from the artist's laugh extension (`artwork/laugh-2026-09-13`): six
mouth drawings, a new `> <` peak eye, and a 2.7 s performance of two uneven bursts and a
recovery.

**The laugh is led by its mouth.** Everything else in the pack is a family per track, each
on its own clock. The laugh is not: every mouth drawing is paired with its own eyes, cheeks
and a 1–3 px face lift, and a "ha" always wears the `> <`. So the generator attaches a
*companion* to each laugh mouth frame, from `laugh-timing.json`, and the skin makes the rest of
the face follow whichever mouth frame is showing. That one mechanism covers both uses:
silent, the face plays the burst; talking, the speech amplitude walks the six drawings (by
measured openness, settle → grin → catch → hee → ha → haa) and each loud syllable brings its
own squeeze with it. Setting the emotion starts the burst at once, and re-sending it with
every sentence does not cut the burst short.

**The laugh mouths sit 7 px higher than the pack's.** The extension draws against a mouth
anchor of (160, 161) where the pack uses (160, 168). Baking them against the pack's anchor
would have put every laugh a little low. The generator now takes an anchor per family, and
refuses to bake if the extension's eye or cheek anchors ever stop matching the pack's.

**A blink opened eyes that were drawn shut.** This predates the laugh. A blink swaps in the
round blink lids, and on an emotion whose eyes are already closed arcs -- happy's squeeze,
sleepy's droop -- that drew a half-open eye for a moment on every blink. The laugh's `> <`
made it impossible to miss. An eye frame on which the artist shows neither pupil is now
treated as drawn shut, and a blink leaves it alone.

**Clip timing drifted.** `ClipTrack` counted each hold from the tick that noticed the previous
one had ended, so every update's lateness was added to every hold after it. At a 33 ms
update a nine-beat laugh ran up to ~300 ms long. Holds now run from when they were due.

`laugh.sh` checks all of it against `laugh-stop-motion.gif`. Because the laugh starts the
instant the emotion is set, every 33 ms capture has an exact counterpart. The worst
difference over all 90 captures is 0.0026 mouth ink and 0.0010 eye ink, against 0.03–0.11
and 0.013–0.019 between any two neighbouring poses of the artist's own. So every capture
shows the right drawing, with the right eyes, at the right millisecond.

The Astra panels did not regress. Measuring the eye region for the first time showed the
delighted and petted eyes now hold still between blinks, as the artist's do.
