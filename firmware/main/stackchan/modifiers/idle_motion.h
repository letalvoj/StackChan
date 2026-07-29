/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../idle/idle_director.h"
#include <hal/hal.h>
#include <cstdint>

namespace stackchan {

/**
 * @brief Idle head motion.
 *
 * A thin adapter now: the behaviour lives in stackchan/idle/, where each mode is its own
 * file and the director owns the policy. This used to be one function with a four-way
 * if-else over a random roll, which was fine until we wanted a mode with internal timing
 * -- the dance needs beats, and that does not fit a "pick a target, move, done" shape.
 *
 * The constructor still takes an interval range so existing call sites are unaffected;
 * it now sets the pause *between behaviours* rather than between individual moves.
 */
class IdleMotionModifier : public Modifier {
public:
    IdleMotionModifier(uint32_t interval_min = 4000, uint32_t interval_max = 8000)
    {
        _director.setGap(interval_min, interval_max);
        _director.reset(GetHAL().millis() + 1000);   // settle before the first move
    }

    void pause()
    {
        _paused = true;
    }

    void resume()
    {
        if (_paused) {
            _paused = false;
            // Drop any half-finished behaviour: it was choreographed against a timeline
            // that stopped while we were paused, and resuming mid-dance looks broken.
            _director.reset(GetHAL().millis() + 500);
        }
    }

    void _update(Modifiable& stackchan) override
    {
        if (_paused || !stackchan.hasAvatar()) {
            return;
        }
        // Someone else (a tool call, an animation) is driving the head; stay out of it.
        if (stackchan.motion().isModifyLocked()) {
            return;
        }
        _director.update(stackchan, GetHAL().millis());
    }

private:
    idle::Director _director;
    bool _paused = false;
};

}  // namespace stackchan
