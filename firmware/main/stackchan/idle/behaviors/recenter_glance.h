/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../idle_behavior.h"

namespace stackchan::idle {

/**
 * @brief Occasional larger move back toward the middle. (weight 10)
 *
 * SlowRandomWalk only ever takes small steps, so left alone it will eventually drift into
 * a corner and stay there admiring the wall. This is the restoring force: every so often,
 * look somewhere sampled around the neutral pose.
 *
 * The target is GAUSSIAN about (0, 30) degrees rather than uniform over the range. Uniform
 * sampling would make "stare at the far edge" exactly as likely as "look ahead", which is
 * both unnatural and noisy; a normal distribution puts most glances near centre and makes
 * the wide ones rare enough to be interesting when they happen.
 */
class RecenterGlance : public Behavior {
public:
    const char* name() const override
    {
        return "recenter";
    }
    int weight() const override
    {
        return 10;
    }

    void begin(const Context& ctx) override
    {
        _issued  = false;
        _next_at = ctx.now_ms;
    }

    bool update(const Context& ctx) override
    {
        if (ctx.now_ms < _next_at) {
            return true;
        }
        if (ctx.motion().isMoving()) {
            _next_at = ctx.now_ms + 200;
            return true;
        }
        if (_issued) {
            return false;
        }
        _issued = true;

        const float yaw   = gaussian(kYawMean, kYawSigma, kYawMin, kYawMax);
        const float pitch = gaussian(kPitchMean, kPitchSigma, kPitchMin, kPitchMax);

        // Slower than a walk step despite covering more ground: this is the one move that
        // can traverse the whole envelope, and rushing it is exactly the noise complaint.
        ctx.moveTo(deg(static_cast<int>(yaw)), deg(static_cast<int>(pitch)), speed::kPurposed);

        // Hold the new pose briefly so it reads as "looked at something", not as a
        // waypoint on the way somewhere else.
        _next_at = ctx.now_ms + restDuration(1600, 3000);
        return true;
    }

private:
    // Degrees, not decidegrees -- sampled in human units, converted once at use.
    static constexpr float kYawMean    = 0.0f;
    static constexpr float kYawSigma   = 22.0f;   // ~2.7 sigma reaches the +/-60 edge
    static constexpr float kYawMin     = -60.0f;
    static constexpr float kYawMax     = 60.0f;

    static constexpr float kPitchMean  = 30.0f;
    static constexpr float kPitchSigma = 11.0f;
    static constexpr float kPitchMin   = 10.0f;
    static constexpr float kPitchMax   = 60.0f;

    bool _issued      = false;
    uint32_t _next_at = 0;
};

}  // namespace stackchan::idle
