/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../idle_behavior.h"

namespace stackchan::idle {

/**
 * @brief The default: small, slow, local wandering. (weight 85)
 *
 * A few short steps from wherever the head already is, then done. Deliberately never
 * travels far in one move -- large sweeps are what made the old idle motion feel restless
 * and, more to the point, loud. The servos are the noisiest thing on the desk and the
 * step size is what drives how long they whine for.
 *
 * Steps are bounded relative to the CURRENT position rather than sampled absolutely, so
 * the head tends to linger in a region for a while and then gradually migrate, which
 * reads as attention rather than as a random number generator.
 */
class SlowRandomWalk : public Behavior {
public:
    const char* name() const override
    {
        return "walk";
    }
    int weight() const override
    {
        return 85;
    }

    void begin(const Context& ctx) override
    {
        _steps_left = Random::getInstance().getInt(2, 4);
        _next_at    = ctx.now_ms;   // first step immediately
    }

    bool update(const Context& ctx) override
    {
        if (ctx.now_ms < _next_at) {
            return true;
        }
        // Never queue a move on top of a move; it stacks commands and the head jerks.
        if (ctx.motion().isMoving()) {
            _next_at = ctx.now_ms + 200;
            return true;
        }
        if (_steps_left-- <= 0) {
            return false;
        }

        const auto here = ctx.angles();
        const int yaw   = here.x + Random::getInstance().getInt(-kMaxYawStep, kMaxYawStep);
        const int pitch = here.y + Random::getInstance().getInt(-kMaxPitchStep, kMaxPitchStep);

        ctx.moveTo(yaw, pitch, Random::getInstance().getInt(speed::kDrift, speed::kGentle));

        // Long, uneven pauses. Evenly spaced motion looks mechanical; the robot should
        // seem to be watching something, not running a timer.
        _next_at = ctx.now_ms + Random::getInstance().getInt(1800, 4500);
        return true;
    }

private:
    // "max 40 / 20 degrees from the current location" -- the brief, and the reason this
    // behaviour is quiet.
    static constexpr int kMaxYawStep   = deg(40);
    static constexpr int kMaxPitchStep = deg(20);

    int _steps_left     = 0;
    uint32_t _next_at   = 0;
};

}  // namespace stackchan::idle
