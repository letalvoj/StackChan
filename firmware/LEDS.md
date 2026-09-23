# LEDS.md — the twelve head lights, and why they are composited

Companion to `AVATAR.md`. The face is geometry animated in normalised units; the lights
are layers composited per frame. Same instinct, different medium.

---

## 1. The problem this replaced

Three unrelated pieces of code used to write the LED hardware directly, and the last one
to run won:

| Writer | What it did |
|---|---|
| `AvatarController::SetStatus()` | stamped **LED 0** green for listening, blue for speaking, black for idle |
| `StackChanAvatarDisplay::UpdateStatusBar()` | stamped **LED 0** red once a second when no host was connected |
| `self.robot.set_led_color` (MCP), BLE/JSON animation, the setup app | painted **all twelve** through `NeonLight` |

Two consequences, both of which you could see on the desk:

- **The status indicator lived on one LED out of twelve.** Eleven lights sat black while
  the robot listened.
- **A host colour was destroyed, not covered.** Ask the robot for warm amber, then say
  something to it: listening stamped LED 0 green, standby stamped it black, and the amber
  was gone — not hidden, *gone*, because the only place the wish had ever been recorded
  was the pixels themselves, and those had been overwritten. There was nothing left to
  restore.

Fighting writers is the symptom. The missing idea is that *a colour someone asked for is
state, and state that only exists as output cannot survive anything else writing output.*

---

## 2. What is there now

One owner, `LedStage` (`addons/neon_light/led_stage.{h,cpp}`), three fixed layers,
composited every frame and pushed once:

```
  touch    localised glow following a hand on the head        alpha, highest
  notify   one-shot sweeps for things that just happened        alpha
  status   what the robot is doing: listening/speaking/no-host  alpha
  base     whatever the host last asked for                    opaque, lowest
```

Four layers, not three as this file first described. Notify was added for an NFC badge
reader, which has since been removed. The layer stays: it is the general "something just
happened" channel, and it slotted in exactly where the "ideas this supports cheaply" list
at the bottom said it would.

Each layer above the base carries its own alpha, so nothing is destructive. The host's
amber sits in `base` untouched for as long as the host wants it; listening lays a green
wash over it and *takes the wash away again* when the conversation ends. Pet the robot
mid-sentence and a pink pool rides over both and fades out, leaving them exactly as they
were.

Three layers, not a plug-in layer framework — three is what the hardware has anything to
say about, and a registry of one-instance layers is just a slower way to spell three
members.

### Who writes what

| Layer | Written by | API |
|---|---|---|
| base | `LeftNeonLight` / `RightNeonLight`, which is what MCP, the BLE/JSON animation and the setup app already talk to | `NeonLight::setColor(...)` — unchanged |
| status | `AvatarController::SetStatus()`, `UpdateStatusBar()` | `GetLedStage().setStatus(LedStatus::…)` |
| notify | anything with an event to announce — no caller today | `GetLedStage().flashNotify(r, g, b)` |
| touch | `LedStage` itself, subscribed to `Hal::onHeadTouchField` | `submitTouch()`, called for you |

`NeonLight` kept its whole public surface. It just writes the stage's base layer now
instead of the hardware, so every existing caller was a no-op change.

---

## 3. Geometry

Twelve LEDs: **0–5 left side, 6–11 right side**, both strips wired the same way round.
`led_axis(i)` maps each to a position on the head's back↔front axis, −1 to +1.

The head-touch sensor (Si12T, `hal_head_touch.cpp`) is three pads on that *same* axis —
which is exactly why a touch can be drawn on the lights at all. Pad 0 is the low bit pair
of the sensor byte and the negative end of `get_position()`; the gesture recogniser calls
movement toward pad 2 "forward".

**The one thing the source cannot tell you** is whether LED index 0 is the front of the
head or the back. That is a fact about how the strips were soldered. It is isolated to a
single line:

```cpp
inline constexpr bool kLedStripRunsBackToFront = true;   // led_stage.h
```

If stroking the head makes the glow chase the wrong way, flip it. Nothing else in the
firmware encodes winding direction, so that is the whole fix.

---

## 4. The touch glow

Every pad throws a gaussian pool of light onto the axis (σ = 0.55). At full contact on
one side's pad the six LEDs of a strip come up at

```
pad 0   1.00  0.77  0.35  0.09  0.01  0.00
pad 1   0.19  0.55  0.94  0.94  0.55  0.19
pad 2   0.00  0.01  0.09  0.35  0.77  1.00
```

— a pool of three or four, with the far end genuinely dark. Wide enough that a fingertip
reads as one soft glow rather than three separate lamps; narrow enough that it is
obviously *local*, which is the whole point.

An LED takes the **strongest** pool reaching it rather than the sum, so two pads held at
once do not blow out the LED between them.

Then the asymmetry that makes it feel like light rather than a readout:

```
attack  26 /s   ≈ 40 ms    contact is instant
decay    3.2/s  ≈ 1 s      the afterglow trails the finger
```

That asymmetry *is* the effect. Symmetric smoothing gives you a lamp that tracks a
finger; fast-in/slow-out gives you something that looks warmed by it.

The colour is a warm pink — deliberately the same "you are being petted" the face says
with hearts (`face::Reaction::HeadPet`), so the two channels agree.

### Raw field vs. gesture

`GestureRecognizer` reduces the three pads to Press / Release / SwipeForward /
SwipeBackward. That is the right input for *behaviour* — `HeadPetModifier` wants to know
it was stroked, not where — and far too coarse for rendering.

So the poll now publishes both:

- `onHeadPetGesture` — unchanged, drives the face and the servos
- `onHeadTouchField` — per-pad strength and centroid, published only while in contact
  plus one final empty sample on release (an idle head has nothing to say, and this runs
  twenty times a second)

---

## 5. Threading, and the two things it costs

`onHeadTouchField` fires on the **head-touch task, pinned to core 1**. Everything else —
`setBase`, `setStatus`, `update()`, the I2C push — is the UI thread. `submitTouch()` is
the only crossing, and it packs the sample into a single `std::atomic<uint32_t>`, so
there is no half-written sample to observe and no lock on a sensor path. (`DEBUGGING.md`
§ "Never `vTaskDelay` on the touch path" is the same lesson from the other direction.)

`GetLedStage().init()` is called from `Hal::init()` **immediately before**
`head_touch_init()`, and that order is deliberate: `head_touch_init()` spawns the task
that emits the signal, and subscribing to a signal already being emitted from another
core is a data race on the subscriber list.

---

## 6. Cost

Two things keep an always-animating strip cheap:

**One transaction per frame.** `Hal::writeRgbFrame()` writes all twelve LEDs into the
expander's LED RAM in a single I2C write and latches them, via `setLedData()`. The old
path was twelve `setRgbColor()` calls, i.e. twelve transactions, plus a read-modify-write
to refresh — affordable for a one-shot colour change, not for 50 Hz.

**Frames the hardware cannot distinguish are not sent.** The expander stores RGB565, so
the dirty check compares *565*, not 888. A slow breath spends most of its time producing
byte-identical frames, and those never reach the bus.

### A cadence caveat worth knowing

The stage ticks from `StackChan::update()`, and the main loop in `hal.cpp` deliberately
throttles itself while xiaozhi is **not** idle:

```cpp
if (!hal_bridge::is_xiaozhi_idle()) {
    vTaskDelay(pdMS_TO_TICKS(100));      // give the audio path the CPU
}
```

So the lights run at ~50 Hz while idle and ~8 Hz mid-conversation — exactly when the
listening/speaking wash is on screen. The animations are all `dt`-driven (and `dt` is
clamped at 200 ms), so nothing speeds up or lurches; it is just coarser. Diffused through
the strip it is not visible, and it is not worth destabilising the audio path to fix.
Noted here so the next person does not go hunting for a bug.

---

## 7. Where to look

```
addons/neon_light/led_stage.h        ← geometry, LedStatus, the layer model
addons/neon_light/led_stage.cpp      ← all tuning constants are at the top
addons/neon_light/neon_light.cpp     ← the base-layer writer, five lines of it
hal/hal_head_touch.cpp               ← gesture + field, one poll
hal/hal_io_expander.cpp              ← writeRgbFrame(), the single-transaction push
stackchan/avatar_controller.cc       ← SetStatus(): the only status-layer writer worth changing
```

Every tuning number — colours, breath periods, alphas, the glow's width and its attack
and decay — is a named `constexpr` in the first sixty lines of `led_stage.cpp`. Retheming
the lights is editing that block; nothing else needs to know.

**Ideas this shape supports cheaply**, if you want more:

- Speaking brightness driven by the mouth's `weight` instead of a fixed pulse — the face
  already computes it, and the lights would breathe with the actual speech
- A status wash that scans front→back instead of breathing (`led_axis()` is already there)
- Left/right asymmetry keyed to servo yaw, so the head "leans" in light as well as motion
- More `flashNotify()` callers — a reminder firing, an OTA starting. The layer is there;
  it takes a colour

**What it does not want:** any new caller writing `GetHAL().setRgbColor()` directly. That
is the thing that was wrong, and it will be wrong again. If something needs the lights,
it needs a layer.
