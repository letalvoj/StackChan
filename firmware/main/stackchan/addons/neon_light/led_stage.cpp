#include "led_stage.h"
#include <hal/hal.h>
#include <cmath>

using namespace stackchan::addon;

namespace {

/* -------------------------------------------------------------------------- */
/*                                   Tuning                                    */
/* -------------------------------------------------------------------------- */

/// The stage runs at the UI frame rate but only needs to think this often.
constexpr uint32_t kTickIntervalMs = 20;  // 50 Hz

/// How each status reads. Alpha is the peak of the breath; the trough is
/// alpha * kBreathFloor, so a status never fully uncovers the base mid-breath and then
/// covers it again -- that flicker is what makes indicator LEDs look broken.
struct StatusStyle {
    uint8_t r, g, b;
    float alpha;
    float period_sec;
};

constexpr float kBreathFloor = 0.72f;

constexpr StatusStyle kStatusIdle = {0, 0, 0, 0.0f, 1.0f};

/// Pale red, slow. An unattended robot on a shelf is resting, not faulted -- but at the
/// 24/255 this used to be it was indistinguishable from off, so the indicator said
/// nothing at all. Legible, and breathing, so it reads as alive rather than as an alarm.
constexpr StatusStyle kStatusWaiting = {96, 0, 0, 0.55f, 4.5f};

/// Green, unhurried: the robot is holding the floor for you.
constexpr StatusStyle kStatusListening = {0, 150, 60, 0.85f, 2.4f};

/// Blue, quicker -- roughly the cadence of speech.
constexpr StatusStyle kStatusSpeaking = {0, 70, 190, 0.85f, 1.0f};

/// Seconds for the status wash to fade in or out. A status change used to be a hard
/// cut, which on a 12-LED strip beside someone's face is a flash.
constexpr float kStatusFadeSec = 0.35f;

/// Warm pink: the same "you are being petted" the face says with hearts.
constexpr StatusStyle kTouchStyle = {220, 80, 140, 1.0f, 0.0f};

/// The notify sweep: a point rushes the length of the head, blooming wider as it goes,
/// then the bloom washes out. Reads as "scanned ... accepted" in one gesture.
constexpr float kNotifySweepSec  = 0.55f;
constexpr float kNotifyFadeSec   = 0.75f;
constexpr float kNotifySigmaFrom = 0.35f;  ///< A point, at the start.
constexpr float kNotifySigmaTo   = 1.95f;  ///< Nearly the whole strip, by the end.

/// Width of one pad's glow in axis units. 0.55 puts the neighbouring pad's LEDs at about
/// a sixth brightness -- enough that a finger reads as a soft pool rather than three
/// separate lamps, not so much that the whole head lights up for one fingertip.
constexpr float kTouchSigma = 0.55f;

/// Fast in, slow out. The asymmetry is the whole effect: contact is instant, the
/// afterglow trails the finger and fades over about a second.
constexpr float kTouchAttackPerSec = 26.0f;
constexpr float kTouchDecayPerSec  = 3.2f;

float approach(float current, float target, float ratePerSec, float dt)
{
    // Frame-rate independent exponential ease; equivalent to lerp() at a fixed dt but
    // does not change speed if a frame is late.
    const float k = 1.0f - std::exp(-ratePerSec * dt);
    return current + (target - current) * k;
}

uint8_t blend_channel(uint8_t bg, uint8_t fg, float alpha)
{
    const float v = static_cast<float>(bg) + (static_cast<float>(fg) - static_cast<float>(bg)) * alpha;
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return static_cast<uint8_t>(v + 0.5f);
}

uint16_t to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

const StatusStyle& style_of(LedStatus status)
{
    switch (status) {
        case LedStatus::Waiting:
            return kStatusWaiting;
        case LedStatus::Listening:
            return kStatusListening;
        case LedStatus::Speaking:
            return kStatusSpeaking;
        case LedStatus::Idle:
        default:
            return kStatusIdle;
    }
}

}  // namespace

/* -------------------------------------------------------------------------- */

void LedStage::init()
{
    if (_is_inited) {
        return;
    }
    _is_inited = true;
    _last_tick = GetHAL().millis();

    // The head-touch task publishes here rather than the other way round, so nothing in
    // the sensor path has to know that LEDs exist. Fires on the touch task's core; the
    // handler stores one atomic and gets out.
    GetHAL().onHeadTouchField.connect([this](HeadTouchField field) {
        submitTouch(field.pad[0], field.pad[1], field.pad[2]);
    });
}

void LedStage::update()
{
    if (!_is_inited) {
        init();
    }

    const uint32_t now     = GetHAL().millis();
    const uint32_t elapsed = now - _last_tick;
    if (elapsed < kTickIntervalMs) {
        return;
    }
    _last_tick = now;

    // Clamped: a long stall (flash write, a blocking network call) must not teleport the
    // animations, it should just resume from where they were.
    const float dt = std::fmin(static_cast<float>(elapsed) / 1000.0f, 0.2f);

    update_status(dt);
    update_notify(dt);
    update_touch(dt);
    composite_and_push();
}

void LedStage::setBase(int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index < 0 || index >= kLedCount) {
        return;
    }
    _base[index] = {r, g, b};
}

void LedStage::setStatus(LedStatus status)
{
    if (status == _status) {
        return;
    }
    _status = status;

    // Going transparent keeps the outgoing colour, so the wash dims out as itself rather
    // than sliding to black on the way -- it is the alpha that carries "idle", not a
    // colour, and a green that turns grey before it leaves reads as a fault.
    if (status == LedStatus::Idle) {
        return;
    }

    const auto& style = style_of(status);
    _status_color_to  = {style.r, style.g, style.b};

    // Coming up from fully transparent there is nothing on screen to cross-fade from, so
    // take the new colour at once instead of fading up through the previous status's.
    if (_status_alpha <= 0.01f) {
        _status_color = _status_color_to;
        _status_phase = 0.0f;  // cos(0) == 1: start the breath at the top, not mid-sigh.
    }
}

void LedStage::flashNotify(uint8_t r, uint8_t g, uint8_t b)
{
    // Colour plus a "there is a request" bit, in one word: any task can call this
    // without touching the animation, which belongs to the UI thread.
    const uint32_t packed = 0x01000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
    _notify_request.store(packed, std::memory_order_relaxed);
}

void LedStage::update_notify(float dt)
{
    const uint32_t request = _notify_request.exchange(0, std::memory_order_relaxed);
    if (request != 0) {
        _notify_color = {static_cast<uint8_t>((request >> 16) & 0xFF), static_cast<uint8_t>((request >> 8) & 0xFF),
                         static_cast<uint8_t>(request & 0xFF)};
        _notify_t     = 0.0f;
        return;  // Render the first frame at t = 0 rather than already a frame in.
    }

    if (_notify_t < 0.0f) {
        return;
    }

    _notify_t += dt;
    if (_notify_t > kNotifySweepSec + kNotifyFadeSec) {
        _notify_t = -1.0f;
    }
}

void LedStage::submitTouch(uint8_t pad0, uint8_t pad1, uint8_t pad2)
{
    const uint32_t packed = static_cast<uint32_t>(pad0 & 0x03) | (static_cast<uint32_t>(pad1 & 0x03) << 2) |
                            (static_cast<uint32_t>(pad2 & 0x03) << 4);
    _touch_sample.store(packed, std::memory_order_relaxed);
}

/* -------------------------------------------------------------------------- */

void LedStage::update_status(float dt)
{
    const auto& style = style_of(_status);

    // Four time constants lands within a couple of percent of the target in
    // kStatusFadeSec, which is what "fades in over a third of a second" means in practice.
    _status_alpha = approach(_status_alpha, style.alpha, 4.0f / kStatusFadeSec, dt);
    if (_status_alpha < 0.002f) {
        _status_alpha = 0.0f;
    }

    // Cross-fade the colour too, so listening -> speaking is one wash turning from green
    // to blue rather than a green one being swapped for a blue one.
    const float color_step = std::fmin(1.0f, dt * 6.0f);
    _status_color.r        = blend_channel(_status_color.r, _status_color_to.r, color_step);
    _status_color.g        = blend_channel(_status_color.g, _status_color_to.g, color_step);
    _status_color.b        = blend_channel(_status_color.b, _status_color_to.b, color_step);

    if (style.period_sec > 0.0f) {
        _status_phase += dt * 2.0f * static_cast<float>(M_PI) / style.period_sec;
        if (_status_phase > 2.0f * static_cast<float>(M_PI)) {
            _status_phase -= 2.0f * static_cast<float>(M_PI);
        }
    }
}

void LedStage::update_touch(float dt)
{
    const uint32_t packed = _touch_sample.load(std::memory_order_relaxed);

    float pad[3];
    for (int p = 0; p < 3; p++) {
        pad[p] = static_cast<float>((packed >> (p * 2)) & 0x03) / 3.0f;
    }

    for (int i = 0; i < kLedCount; i++) {
        const float u = led_axis(i);

        // Each pad throws a gaussian pool of light onto the axis; an LED takes the
        // strongest one reaching it rather than the sum, so two pads held at once do not
        // blow out the LED between them.
        float target = 0.0f;
        for (int p = 0; p < 3; p++) {
            if (pad[p] <= 0.0f) {
                continue;
            }
            const float d          = u - kTouchPadAxis[p];
            const float falloff    = std::exp(-(d * d) / (2.0f * kTouchSigma * kTouchSigma));
            const float from_this  = pad[p] * falloff;
            target                 = std::fmax(target, from_this);
        }

        const float rate = (target > _touch_glow[i]) ? kTouchAttackPerSec : kTouchDecayPerSec;
        _touch_glow[i]   = approach(_touch_glow[i], target, rate, dt);
        if (_touch_glow[i] < 0.002f) {
            _touch_glow[i] = 0.0f;
        }
    }
}

void LedStage::composite_and_push()
{
    // Breath is a raised cosine in [kBreathFloor, 1].
    const float breath =
        kBreathFloor + (1.0f - kBreathFloor) * 0.5f * (1.0f + std::cos(_status_phase));
    const float status_alpha = _status_alpha * breath;

    // The sweep's centre runs the length of the axis while its width blooms from a point
    // to nearly the whole strip; the envelope holds through the sweep and washes out
    // after it. One expression, so there is no seam where a band becomes a flash.
    float notify_center = 0.0f;
    float notify_sigma  = 1.0f;
    float notify_env    = 0.0f;
    if (_notify_t >= 0.0f) {
        const float p = std::fmin(1.0f, _notify_t / kNotifySweepSec);
        notify_center = -1.0f + 2.0f * p;
        notify_sigma  = kNotifySigmaFrom + (kNotifySigmaTo - kNotifySigmaFrom) * p;
        notify_env    = (_notify_t <= kNotifySweepSec)
                            ? 1.0f
                            : std::fmax(0.0f, 1.0f - (_notify_t - kNotifySweepSec) / kNotifyFadeSec);
    }

    uint8_t frame[kLedCount * 2];
    uint16_t packed[kLedCount];
    bool changed = _never_pushed;

    for (int i = 0; i < kLedCount; i++) {
        uint8_t r = _base[i].r;
        uint8_t g = _base[i].g;
        uint8_t b = _base[i].b;

        if (status_alpha > 0.0f) {
            r = blend_channel(r, _status_color.r, status_alpha);
            g = blend_channel(g, _status_color.g, status_alpha);
            b = blend_channel(b, _status_color.b, status_alpha);
        }

        if (notify_env > 0.0f) {
            const float d = led_axis(i) - notify_center;
            const float a = notify_env * std::exp(-(d * d) / (2.0f * notify_sigma * notify_sigma));
            if (a > 0.0f) {
                r = blend_channel(r, _notify_color.r, a);
                g = blend_channel(g, _notify_color.g, a);
                b = blend_channel(b, _notify_color.b, a);
            }
        }

        if (_touch_glow[i] > 0.0f) {
            const float a = _touch_glow[i] * kTouchStyle.alpha;
            r             = blend_channel(r, kTouchStyle.r, a);
            g             = blend_channel(g, kTouchStyle.g, a);
            b             = blend_channel(b, kTouchStyle.b, a);
        }

        packed[i] = to_rgb565(r, g, b);
        if (packed[i] != _pushed[i]) {
            changed = true;
        }

        frame[i * 2]     = static_cast<uint8_t>(packed[i] & 0xFF);
        frame[i * 2 + 1] = static_cast<uint8_t>((packed[i] >> 8) & 0xFF);
    }

    if (!changed) {
        return;
    }

    for (int i = 0; i < kLedCount; i++) {
        _pushed[i] = packed[i];
    }
    _never_pushed = false;

    // One I2C transaction for the whole strip, not twelve. The old per-LED path is still
    // there for callers that genuinely want a single lamp, but nothing on this path does.
    GetHAL().writeRgbFrame(frame, sizeof(frame));
}

/* -------------------------------------------------------------------------- */

LedStage& stackchan::addon::GetLedStage()
{
    static LedStage stage;
    return stage;
}
