# AVATAR.md — how the face works, and how to make a new one

Written before redesigning the face, so we both know what the canvas actually is.

---

## 1. The short answer

**C++ and LVGL. No JavaScript, no sprites, no bitmaps, no canvas drawing.**

The face is a handful of ordinary LVGL container widgets — rectangles with a background
colour and a corner radius — whose *geometry is animated at runtime*. An eye is a circle.
The eyelid is a black rectangle that slides down over it. The mouth is one rectangle whose
width, height and corner radius are interpolated.

That is the whole trick, and it is worth appreciating before replacing it: there is no
frame-by-frame animation anywhere. Every expression is a continuous function of a few
numbers, which is why the face can blend smoothly between states and costs almost no
memory. Nothing is pre-rendered.

Decorators (the blush, sweat drop, anger mark, hearts) are the exception — those are
compiled-in C image arrays, in `avatar/decorators/assets/`.

---

## 2. Layers

```
Stackchan::update()                      once per UI frame
   └── Avatar::update()                  avatar/avatar/avatar.h
         ├── KeyElements                 leftEye / rightEye / mouth / speechBubble
         └── Decorator pool              heart, sweat, angry, dizzy, shy (images)

Modifiers  ──drive──▶  Features          modifiers/*.h
```

| Path | What it is |
|---|---|
| `avatar/avatar/elements/` | The **interface**: `Element`, `Feature`, `Emotion` |
| `avatar/skins/default/` | The **implementation**: `eyes.cpp`, `mouth.cpp`, `speech_bubble.cpp` |
| `avatar/decorators/` | Overlay images with their own lifetime |
| `modifiers/` | The **animation**: blink, breath, speaking, head-pet, IMU, idle expression |
| `stackchan/idle/` | Idle *head motion* (servos) — separate from the face |

The split that matters: **`Feature` defines what a face part can do; a skin decides what it
looks like.** A new face is a new skin. Nothing above it has to change.

---

## 3. The Feature contract

Every face part exposes the same four normalised knobs (`elements/feature.h`,
`elements/element.h`). All of them are integers, all clamped:

| Property | Range | Meaning |
|---|---|---|
| `weight` | 0–100 | Intensity. Eyes: 0 = shut, 100 = wide. Mouth: 0 = closed, 100 = open |
| `size` | −100–100 | Relative scale, 0 = normal |
| `position` | −100–100 (x, y) | Offset within the part's allowed travel, **not** pixels |
| `rotation` | 0–3600 | Tenths of a degree |
| `visible` | bool | |
| `emotion` | enum | Skin maps this to a pose (see §5) |

Normalised units are the reason modifiers are skin-agnostic: `BlinkModifier` sets
`weight = 0` and has no idea whether that closes a circle, a slit, or an anime `>_<`.

### How the default skin realises them

**Eyes** (`skins/default/eyes.cpp`) — three nested containers:

```
_container   pivot/rotation, fixed 32×32 box
  _eye       radius = LV_RADIUS_CIRCLE, primary colour   ← the eye
  _eyelid    square, secondary (background) colour       ← slides down to blink
```

`setWeight()` maps 0–100 onto the eyelid's Y offset. Blinking is literally a black
rectangle sliding over a white circle. `setPosition()` maps −100..100 onto a ±16 px
travel around a base position of `(±70, −16)`.

**Mouth** (`skins/default/mouth.cpp`) — a single container. `setWeight()` interpolates
width, height **and corner radius** together, so a closed mouth is a thin bar and an open
one is a rounded blob. One number, three properties, no assets.

---

## 4. Modifiers: where the procedural animation lives

A `Modifier` is a small object with an `_update()` called every frame; it nudges Features
and can request its own destruction. This is the animation system.

| Modifier | Effect |
|---|---|
| `blink.h` | Eyelid down/up on a randomised interval |
| `breath.h` | Slow sinusoidal drift — the thing that makes it look alive |
| `speaking.h` | Mouth weight driven while TTS plays |
| `head_pet.h` | Reacts to the head-touch sensor |
| `imu.h` | Reacts to being picked up or shaken |
| `idle_expression.h` | Occasional expression changes when idle |
| `timed.h` | Wrapper: run another modifier for N ms, then destroy |

Added via `stackchan.addModifier(std::make_unique<XModifier>())`, in
`stackchan_display.cc` `SetupUI()`.

**Modifiers are additive and unordered**, which is worth knowing: two modifiers writing the
same Feature fight, last write per frame wins. `isModifyLocked()` is how a deliberate
animation temporarily claims exclusive control.

---

## 5. Emotions

`Emotion` (`elements/emotion.h`) is an enum the skin translates into a pose. In the default
skin, `setEmotion()` for the eyes is just "pick a weight and a rotation, mirrored for the
right eye" — e.g. angry tilts the inner corners down. The skin owns that mapping entirely,
so a new face can express the same emotion set completely differently.

---

## 6. So — what can we change?

Roughly in order of effort.

**Retheme (minutes).** `DefaultAvatar::primaryColor` / `secondaryColor`. The face is drawn
in exactly two colours.

**Reshape the existing parts (an afternoon).** Edit `eyes.cpp` / `mouth.cpp`. The constants
at the top of each file — `_eye_size`, `_eye_pos`, `_eye_size_limit`, `_mouth_min_size`,
`_mouth_max_size`, `_mouth_min_radius` — control almost everything about the proportions.
Bigger eyes, closer set, a wider mouth, a flatter rest pose: all constants.

**A new skin (the real answer).** Copy `skins/default/` to `skins/<name>/`, implement
`Feature` for each part, and construct it instead of `DefaultAvatar` in
`stackchan_display.cc` `SetupUI()`. Everything above — every modifier, every emotion, the
speech bubble, the servo choreography — keeps working untouched, because it all speaks in
normalised units.

**Ideas the current architecture supports well**, because it is vector-ish geometry rather
than sprites:

- Eyes as rounded rects instead of circles (a very different personality for ~5 lines)
- Eyelids that come from *both* directions, or angled lids for sleepy/suspicious
- Pupils as a separate child element that tracks servo yaw — the head turns *and* the eyes
  lead the turn, which is a huge liveliness win for little work
- A mouth built from two or three elements so it can curve, not just open
- Brows as a new `Feature` — the single biggest expressiveness gain per line of code

**What it does not support cheaply:** anything that wants per-pixel drawing, gradients, or
frame-by-frame art. LVGL can blit images (the decorators do), but an image-based face gives
up the smooth interpolation that makes this one feel alive, and costs flash per frame.

---

## 7. Where to look first

```
avatar/skins/default/eyes.cpp        ← 144 lines, the whole eye
avatar/skins/default/mouth.cpp       ←  73 lines, the whole mouth
avatar/avatar/elements/feature.h     ←  the contract a new skin must satisfy
modifiers/blink.h                    ←  a representative modifier
hal/board/stackchan_display.cc       ←  SetupUI(): where the avatar is built
```

The entire default face is under 400 lines. It is a small, well-factored thing — the
redesign is mostly a design conversation, not an engineering one.
