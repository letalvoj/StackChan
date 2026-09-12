# Bringing the face to life — porting the expression pack

> **Status: ported, 12 September.** Phases 1 and 2 are done. All 29 families are baked,
> the skin plays them, and every family is exported as a strip and a GIF. What remains is
> phase 3, the director. The plan below is kept as the record of the reasoning.

Proposal for `tools/facegen/artwork/pack-2026-09-12/`. 29 motion families, 174 drawn
frames, plus `timing-and-anchors.json`, which is the part that makes this cheap.

The headline: **the pack was drawn against the face we already ship, and our architecture
already has the right seams.** This is not a re-port. It is swapping static drawings for
clips and adding a small player.

---

## 1. It drops onto the current geometry

The pack declares its own layout — anchors in a 320 × 240 panel, local coordinates scaled
by 1.5. Those land within a few pixels of what the device is wearing right now:

| Anchor | Pack, at its scale 1.5 | Shipping `chalk_sprites.h` | Delta |
|---|---|---|---|
| Left eye | −64.5, −20 | −63, −14 | 2, 6 |
| Right eye | +64.5, −21 | +65, −16 | 1, 5 |
| Mouth | 0, +48 | +2, +51 | 2, 3 |
| Cheeks | 0, +20 | ±99, +33 | see note |
| Brows | 0, −60 | (not anchored today) | new |

So the enlargement the graphics department did survives untouched. We adopt the pack's
anchors as the declared layout and the face barely moves. Cheeks are the one row to
re-measure, because the pack anchors the *pair* at one point where we anchor each side.

**One open art decision.** The pack's open eye is 36 local units wide, so 54 px at its own
scale. Our shipping eye is 56 px wide because `EYE_WIDTH = 1.4` widened it. Applying that
same widening to the pack gives 76 px, which is wider than anything the artist drew or
checked. My recommendation: **bake the pack as drawn and drop `EYE_WIDTH`**, because every
one of the 36 eye frames was composed against the real proportion, and stretching them
non-uniformly will distort the lid curves the blink depends on. If the department wants
them wider, that is a request back to them for a redrawn sheet, not a scale factor.

---

## 2. It fits in flash

| | Sprites | A8 bytes |
|---|---:|---:|
| Every frame baked naively | 234 | 358 KB |
| Deduplicated on geometry + placement | 121 | 233 KB |
| Free in the app partition today | | 1.35 MB |

Measured, not estimated. Roughly half the side-drawings are duplicates — the left and right
eye share one path, and the plain open shell repeats 23 times as the rest frame of many
families. Deduplicating on the pair of (path data, placement transform) keeps the bake
pixel-honest while cutting a third of the bytes.

233 KB against 1.35 MB of headroom. This is not a constraint worth designing around.

---

## 3. What changes in code, and what does not

**Unchanged.** `Avatar`, `Feature`, `FaceAnchors`, `OverlayArt`, `SpriteDecorator`, the
`anchors()` / `overlayArt()` seam, the facelab harness, `face_states.h`. All of it already
does the right thing. The pack needs no new interface at the avatar level.

**The one new idea: a clip.** A sprite becomes a sequence with its own timing.

```cpp
enum class Loop { Once, Hold, Cycle };

struct Clip {
    const SpriteSlot* const* frames;
    const uint16_t*          ms;      ///< per-frame hold, straight from the artist's JSON
    uint8_t                  count;
    Loop                     loop;
};

/// One track's playhead. Each part owns one and runs on its own clock, which is the
/// pack's central instruction: parts that all start together read as a machine.
class ClipPlayer {
public:
    void play(const Clip& clip, uint32_t now);
    void hold(const Clip& clip, uint8_t frame);   ///< park on a pose, for speech
    bool advance(uint32_t now);                   ///< true when the frame changed
    const SpriteSlot* frame() const;
    bool finished() const;
};
```

Then each part owns a player instead of a slot: eye shell, pupil, brows, cheeks, mouth,
accent. `ChalkEyes::refresh()` stops being a switch over emotion-and-weight and becomes
"which clip is this track playing, and which frame is it on".

**Deletions.** `SpriteDecorator::Motion` and its `kWobbleFrames` / `kDripOffsets` /
`kSpinStep` hand-rolled animation all go: the pack draws those motions. The dizzy spin in
particular is six drawn spirals instead of one rotated bitmap, which will look sharper,
because rotating an A8 sprite blurs a 3.6 px chalk stroke. `ChalkMouth`'s `kOvalCurve` and
its scaled oval also go, replaced by drawn speech poses.

---

## 4. Emotion becomes a palette, not a pose

This is the substantive behaviour change. Today an emotion picks one drawing per part.
With the pack, an emotion selects a *family per track*, and each track then runs on its own:

| Emotion | Eye shell | Mouth at rest | Mouth speaking | Brows | Cheeks |
|---|---|---|---|---|---|
| Neutral | blink | quiet-smile | speech-neutral | — | rest |
| Happy | smile-squeeze | quiet-smile | speech-delighted | soft-delight | smile-lift |
| Angry | grumpy-squint | speech-grumpy 00 | speech-grumpy | stubborn | puffed-hmph |
| Sad | heavy-sad | speech-sad 00 | speech-sad | concern | rest |
| Doubt | curious-widen | thinking | speech-neutral | curiosity | rest |
| Sleepy | sleepy-drift | yawn | — | — | — |

Happy and Doubt gain brows they do not have today. Angry gains a cheek puff. Sleepy gains a
real yawn instead of a static small circle.

**Speaking.** Mouth weight stops scaling an oval and instead selects a pose inside the
emotion's speech family: closed, small, middle, large. The bands in `face_states.h` already
carve weight up correctly, so that header stays as-is and only the mapping target changes.
The guide asks for an attack of 60–100 ms and a release of 100–160 ms and explicitly warns
against picking a new random mouth every frame, so the player smooths the amplitude
envelope and holds a pose rather than chasing each audio sample.

**Two rows in the JSON replace hand-written logic outright.** `pupil_visibility` gives, per
frame, whether to draw both pupils, one, or none — which is exactly the `has_pupil`
reasoning currently written by hand in `eyes.cpp`, including the asymmetric one-eye case
that makes `curious-widen` read as curiosity. And `gaze_limit` gives the pupil travel per
family: ±3 for the narrow sad and grumpy shells, ±8 by ±6 for open ones. Both become data.

---

## 5. The pupil track stays continuous

Do **not** bake the 24 gaze frames. Inspecting them shows every one is the same
`pupil-dot` translated to an absolute local position; `gaze-look-left-03` is simply both
pupils at −7, +1 from their eye centres. Our existing continuous `setGaze` already produces
that and produces every position in between, which drawn frames cannot.

What we take from the gaze sheet instead is calibration: the extremes confirm the travel
limits, and `head-counterlook` confirms the counter-swing direction that
`IdleExpressionModifier` already implements. So that work stands, and we save 24 sprites.

---

## 6. Idle gets a director

Right now idle moves pupils and occasionally nudges the face. The pack's suggested
performances are recipes for something better: short scripted sequences across tracks with
deliberate offsets. A curiosity beat is "raise one brow, widen an eye 80–150 ms later,
glance at the stimulus, pull the mouth slightly to one side, release in reverse order."

That wants a small sequencer above the players — a list of (track, clip, delay) steps. It is
maybe 80 lines, and it is what converts a face that animates into a face that seems to
intend something. I would build it in phase 3, once the tracks are proven.

---

## 7. Risks I would check early

- **Redraw cost.** Six tracks changing independently, some at 40 ms holds. Every frame swap
  dirties a rectangle. The eye shells are the largest. I want an FPS measurement on hardware
  before committing to the full track count, and a cheap fallback of coalescing swaps into
  one LVGL pass per tick.
- **Bottom margin.** In the artist's own `composed-expressions.png`, the sad talking mouth
  touches the bottom edge of its tile. With the mouth anchor at +48 and a yawn that is the
  tallest drawing in the pack, this needs a real bounds check across all 42 mouth frames,
  the same way the rounder-face pass checked ink bounds.
- **Frame-swap flicker.** Changing `src` and position in separate LVGL operations can show
  a half-updated sprite. Set both before the next `lv_timer_handler`.
- **Speech pose churn.** The guide is emphatic that amplitude alone must not drive pose
  selection frame by frame. If the envelope smoothing is wrong the mouth will look like it
  is chewing. Worth a dedicated facelab filmstrip.

---

## 8. Phasing

1. **Bake and prove.** Extend `gen_sprites.py` with a pack mode that reads
   `timing-and-anchors.json`, dedups, and emits `chalk_clips.{h,cpp}`. Facelab grows a
   filmstrip export so every family can be reviewed as a strip. Nothing on the device
   changes yet. *Ships: proof sheets for all 29 families.*
2. **Clips replace statics.** `ClipPlayer`, the per-track wiring, the emotion palette table,
   and speech poses. Decorators switch to drawn accent clips and lose their hand-rolled
   motion. *Ships: the face above, animated, on hardware.*
3. **The director.** Idle attention, curiosity, joy/headpet, grumpy and sad talking, the
   sleepy-to-yawn sequence, per the guide's recipes. *Ships: a face with intent.*
4. **Polish.** Per-eye asymmetry for winks, the rare double blink, head-tilt on petting,
   whole-face breathing as a parent transform rather than per-part offsets.

Phase 1 and 2 are the bulk of the value. Phase 3 is where it stops looking like a toy.

---

## 9. What I would ask the department

- Confirm the eye width question in §1: bake as drawn, or redraw a wider sheet?
- The cheek pair anchors as one point; we anchor each side. Are the left and right cheek
  offsets inside the pair fixed, or may we place them independently?
- `speech-neutral` is a phoneme palette, not a sequence. With amplitude only, is
  00/05 → 01 → 04 → 02 a sensible closed-to-open ordering, or do they want a different one?
