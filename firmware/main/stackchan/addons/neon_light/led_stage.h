#pragma once
#include <atomic>
#include <cstdint>

namespace stackchan::addon {

/* -------------------------------------------------------------------------- */
/*                                  Geometry                                   */
/* -------------------------------------------------------------------------- */

/// The head carries two strips of six: indices 0-5 are the left side, 6-11 the right.
inline constexpr int kLedCount    = 12;
inline constexpr int kLedsPerSide = 6;

/// The two strips are NOT wired the same way round, which is the sort of thing only the
/// hardware can tell you: measured on the device, a back-to-front rub lit the left side
/// correctly and the right side in reverse. So LED 0 and LED 6 are opposite ends of the
/// head, and each strip carries its own winding flag.
///
/// "Left" and "right" are the robot's own, matching LeftNeonLight / RightNeonLight -- the
/// right strip (indices 6-11) is the one on your left as you face the device.
///
/// These two booleans are the only place the winding is encoded; led_axis() is the only
/// reader, so flipping one here flips every effect that has a direction.
inline constexpr bool kLeftStripRunsBackToFront  = true;
inline constexpr bool kRightStripRunsBackToFront = false;

/// Where an LED sits along the head's back<->front axis: -1 at the back, +1 at the front.
constexpr float led_axis(int index)
{
    const float t = static_cast<float>(index % kLedsPerSide) / (kLedsPerSide - 1);  // 0..1
    const float u = t * 2.0f - 1.0f;
    const bool back_to_front =
        (index < kLedsPerSide) ? kLeftStripRunsBackToFront : kRightStripRunsBackToFront;
    return back_to_front ? u : -u;
}

/// The three head-touch pads sit on the same axis, evenly spaced. Pad 0 is the low bit
/// pair of the Si12T result (see si12t_parse_touch_result_to) and the negative end of
/// get_position(), which the gesture recogniser calls "backward".
inline constexpr float kTouchPadAxis[3] = {-1.0f, 0.0f, 1.0f};

/* -------------------------------------------------------------------------- */
/*                                   Status                                    */
/* -------------------------------------------------------------------------- */

enum class LedStatus {
    Idle,       ///< Nothing to say. The layer goes fully transparent.
    Waiting,    ///< Idle, but no host is connected.
    Listening,  ///< Microphone open.
    Speaking,   ///< TTS playing.
};

/* -------------------------------------------------------------------------- */
/*                                  LedStage                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief The one owner of the twelve head LEDs.
 *
 * Before this existed, three unrelated pieces of code wrote the hardware directly and
 * the last one to run won: AvatarController stamped LED 0 green/blue for listening and
 * speaking, the display tick stamped it red for "no host", and `self.robot.set_led_color`
 * painted all twelve through NeonLight. A host that set a colour lost it the moment the
 * robot started listening, and never got it back, because there was nowhere the wish was
 * still written down -- only the pixels, and those had been overwritten.
 *
 * So: three layers, composited every frame, never destructive.
 *
 *   base   <- whatever the host last asked for (MCP, BLE/JSON animation, setup app)
 *   status <- what the robot is doing, as a translucent wash that fades in and out
 *   notify <- one-shot sweeps for things that just happened
 *   touch  <- a localised glow that follows a finger on the head, brightest on top
 *
 * Status and touch are *overlays with alpha*, so the host's colour survives underneath
 * and reappears intact when the conversation ends or the hand lifts. Layers are a fixed
 * set of three rather than a plug-in system, because three is what the hardware has
 * anything to say about.
 *
 * Threading: everything but submitTouch() runs on the UI thread, which is also where
 * update() pushes I2C. submitTouch() is called from the head-touch task on the other
 * core and only stores one atomic word.
 */
class LedStage {
public:
    /// Idempotent; update() calls it if nobody else did.
    void init();

    /// Composite the three layers and push a frame if it differs from what the hardware
    /// already shows. Call once per UI frame -- StackChan::update() does.
    void update();

    /* ------------------------------ Base layer ------------------------------ */

    /// Set one physical LED's resting colour. This is what NeonLight drives.
    void setBase(int index, uint8_t r, uint8_t g, uint8_t b);

    /* ----------------------------- Status layer ----------------------------- */

    void setStatus(LedStatus status);
    LedStatus status() const
    {
        return _status;
    }

    /* ----------------------------- Notify layer ----------------------------- */

    /// Run a one-shot sweep in this colour: a bright point that rushes front-to-back,
    /// blooming outward as it goes, then washes out. About 1.3 s end to end.
    ///
    /// For events rather than states -- something happened, it is over, the lights go
    /// back to whatever they were showing. A second call restarts it rather than
    /// queueing, because two of the same event in a second are one gesture, not two.
    void flashNotify(uint8_t r, uint8_t g, uint8_t b);

    /* ----------------------------- Touch layer ------------------------------ */

    /// Raw per-pad contact strength, 0-3 each, straight off the Si12T. Safe to call from
    /// the head-touch task: it packs the sample into one atomic and returns.
    void submitTouch(uint8_t pad0, uint8_t pad1, uint8_t pad2);

private:
    struct Rgb {
        uint8_t r = 0, g = 0, b = 0;
    };

    void update_status(float dt);
    void update_notify(float dt);
    void update_touch(float dt);
    void composite_and_push();

    bool _is_inited     = false;
    uint32_t _last_tick = 0;

    Rgb _base[kLedCount];

    LedStatus _status    = LedStatus::Idle;
    float _status_alpha  = 0.0f;  ///< Smoothed toward the status's target alpha.
    float _status_phase  = 0.0f;  ///< Breath phase, radians.
    Rgb _status_color    = {};
    Rgb _status_color_to = {};

    /// Seconds into the current sweep, or < 0 when there is none running.
    float _notify_t     = -1.0f;
    Rgb _notify_color   = {};
    /// Set by flashNotify(), which may be called from anywhere; the sweep itself starts
    /// on the next update() so no animation state is touched off-thread.
    std::atomic<uint32_t> _notify_request{0};

    /// Packed submitTouch() sample: pad0 | pad1<<2 | pad2<<4. Written by the touch task,
    /// read by the UI thread; one word, so there is no half-updated sample to see.
    std::atomic<uint32_t> _touch_sample{0};
    float _touch_glow[kLedCount] = {};

    /// Last frame actually written, in the RGB565 the hardware stores. Comparing in 565
    /// rather than 888 is what keeps a slow breath from pushing frames the LEDs cannot
    /// tell apart.
    uint16_t _pushed[kLedCount] = {};
    bool _never_pushed          = true;
};

LedStage& GetLedStage();

}  // namespace stackchan::addon
