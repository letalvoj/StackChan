# Little feelings — independent facial performances

Six component sheets, 29 motion families, 174 drawn frames. Warm chalk on black; pink affection and blue sweat. The contours are intentionally uneven. Mouths have separate drawings for each emotional speech family, rather than uniformly scaling a single oval.

## Files

| Sheet | Frames | What it explores |
| --- | ---: | --- |
| mouth-sheet.svg | 42 | Quiet smile, neutral speech poses, delighted speech, grumpy speech, sad speech, thinking, yawn |
| eyes-sheet.svg | 36 | Blink, smile squeeze, curious widening, grumpy narrowing, sad heaviness, sleepy drift |
| gaze-sheet.svg | 24 | Left glance, up-right glance, tracking, counterlook during a head turn |
| brows-sheet.svg | 24 | Curiosity, stubbornness, concern, soft delight |
| cheeks-sheet.svg | 24 | Blush buildup, smile lift, puffed hmph, pet warmth |
| accents-sheet.svg | 24 | Heart pulse, dizzy rotation, falling sweat, anger twitch |

Each sheet has a PNG preview. `frames/<component>/` contains a standalone transparent SVG for every frame, with a common 200 × 100 viewport and `viewBox="-100 -45 200 100"`. `face-parts.svg` is the shared library with a visible assembled neutral face. `composed-expressions.svg` is a static six-face composition; `independent-motion.gif` is a short performance preview. The GIF has scripted speech poses, not lip-sync to an audio recording.

## Independent controls

| Track | Independent axes | Combining it with other tracks |
| --- | --- | --- |
| Mouth | Aperture, width, corner curvature, left/right pull, tension, speech pose | Choose an emotional family, then its speech pose. Talking preserves the family's corners. |
| Eye shell/lids | Openness, upper/lower lid shape, squint, widening, droop, asymmetry | A blink temporarily replaces the lids and returns to the emotional eye state. |
| Pupils | Horizontal/vertical gaze, per-eye offset, target hold, counter-motion during head turns | Move inside the current eye shell. Gate visibility and limit travel using the eye-frame metadata. |
| Brows | Inner lift, outer lift, slant, curvature, left/right asymmetry | Curiosity can raise one brow before the eyes follow. Concern lifts inner brow ends. |
| Cheeks | Height, puff, hatch spread, blush intensity, left/right asymmetry | Use one chalk cheek-geometry state; pink blush can overlay it. |
| Accents | Event intensity, scale, rotation, drift, lifetime | Hearts pulse briefly, sweat falls once, anger twitches twice. Spirals replace both shells and pupils. |
| Whole face | Small horizontal/vertical drift, roll, breathing | Compose as a parent transform, with much smaller travel than gaze. No extra sprite sheet is necessary. |
| Timing | Attack, hold, release, pauses, phase offsets | Independent clocks give the parts a sense of intent; avoid starting every track together. |

Eye, gaze, brow and cheek frames have `-left` and `-right` child IDs. These retain their position relative to the pair's origin, so either side can be referenced independently. For example, `eyes-curious-widen-03-left` and `eyes-sleepy-drift-02-right` can form a wink. Check the combined expression before using unusual mixes.

## Suggested performances

**Idle attention.** Hold gaze for roughly 0.6–1.6 seconds. A saccade takes 35–70 ms; hold the new target instead of endlessly sweeping. Let a brow follow after 80–150 ms if the target is interesting. Blink occasionally, with the rare second blink after a 120 ms gap. The mouth stays mostly at rest. Whole-face breathing travels about one pixel.

**Curiosity.** Raise one brow, widen an eye, glance toward the stimulus, then pull the mouth slightly to one side. Release in the opposite order. Keep the brow raised longer than the initial widening.

**Talking.** Pick the emotional mouth row first. Use phoneme information to select speech poses when available. With only audio amplitude, use closed/small/middle/large openings within that family and smooth the envelope; do not select a new random mouth every frame. Attack in about 60–100 ms and release in 100–160 ms. Return to the emotional resting mouth during pauses. Brows and gaze continue on their own clocks. F/V and EE are specific poses, not additional volume levels.

**Joy / headpet.** Lift the cheeks, squeeze the eyes with a slight delay, open a crooked grin, then introduce warmth and a heart. Tilt toward the touch by roughly 2–3 degrees. The cheeks should settle later than the mouth. Headpet keeps its open happy mouth.

**Grumpy talking.** Hold inward-slanting brows; preserve tight mouth corners while opening the jaw. An occasional cheek puff feels stubborn. Use the anger mark for a short reaction, not constant vibration.

**Sad talking.** Hold raised inner brows and lower-heavy eyes. The mouth opens with downward corners and closes reluctantly. Longer pauses and slow releases carry more emotion than adding small extra lip strokes.

**Sleepy / yawn.** Let the lids sink first, then open the yawn. Hold the peak, close the mouth, and reopen the eyes last. Keep the pupils hidden during closed and narrow-lid frames.

**Dizzy.** Place a spiral at each eye anchor and hide the normal eye shells and pupils. Offset their rotation phase; use a quiet mouth and low-amplitude head tilt so the eyes remain readable.

## Porting and timing

`timing-and-anchors.json` contains all frame IDs, suggested hold times, loop/event intent, speech-pose names, pupil visibility and conservative gaze limits. Most rows are drawn transition sequences. Speech rows are pose palettes: their left-to-right order is an audition sequence, not a universal spoken syllable.

The demonstration panel is 320 × 240. Local component coordinates are scaled by 1.5:

| Part | Anchor in panel pixels |
| --- | --- |
| Eye pair and gaze pair | 160, 100 |
| Brow pair | 160, 60 |
| Mouth | 160, 168 |
| Cheek pair | 160, 140 |
| Top-right accent | 274, 44; use a smaller independent accent scale |

The pair's local eye centers are approximately -43,0 and 43,-1. Draw shells first and pupils afterward. Use the metadata to omit the right pupil in asymmetric narrow-eye poses. Limit gaze to ±3 local units for sad/grumpy shells and up to ±8 horizontal / ±6 vertical for open shells. A faint grey/white shell shown behind the gaze sheet is context only and is not part of the gaze sprites.

Typical instance:

```xml
<use xlink:href="#mouth-speech-delighted-02"
     transform="translate(160 168) scale(1.5)"/>
```

Keep the enclosing presentation attributes when extracting definitions: `fill="none" stroke="#f4f1e9" stroke-width="4.2" stroke-linecap="round" stroke-linejoin="round"`. Local paths override these where needed. Mouths with dark cutouts have two colors; preserve both layers when baking sprites.

SVG files use the Tiny 1.2 profile, paths, groups and local use references. There are no masks, filters, bitmap textures, CSS, scripts or external resources. Frame topology intentionally varies, so arbitrary pairs are not ready for direct path-data morphing. Use frame selection with small positional/scale easing; an eventual morph pass should explicitly match control-point topology for the chosen transitions.

This is a graphics and motion-design package. It does not change or flash firmware. Physical screen review and renderer-specific animation integration remain a later step.
