/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "idle_behavior.h"
#include "behaviors/slow_random_walk.h"
#include "behaviors/recenter_glance.h"
#include "behaviors/dance.h"
#include <memory>
#include <vector>

namespace stackchan::idle {

/**
 * @brief Picks an idle behaviour, runs it to completion, waits, picks again.
 *
 * The director owns the *policy* (which behaviour, how often); each Behavior owns its own
 * choreography and timing. Keeping those apart is what makes a new mode a single new file
 * plus one line here, rather than another branch in a growing if-else.
 *
 * Adding one:
 *     behaviors/my_thing.h  ->  subclass Behavior, give it a weight
 *     add(std::make_unique<MyThing>())  in the constructor below
 */
class Director {
public:
    Director()
    {
        // Weights are relative and declared on each behaviour, so the split lives next to
        // the code it describes instead of in a table that drifts out of date.
        //   walk 85 / recenter 10 / dance 5
        add(std::make_unique<SlowRandomWalk>());
        add(std::make_unique<RecenterGlance>());
        add(std::make_unique<Dance>());
    }

    void reset(uint32_t now_ms)
    {
        _active   = nullptr;
        _idle_until = now_ms;
    }

    void update(Modifiable& stackchan, uint32_t now_ms)
    {
        const Context ctx{stackchan, now_ms};

        if (_active != nullptr) {
            if (!_active->update(ctx)) {
                _active     = nullptr;
                // Rest between behaviours. Without this the robot is in perpetual motion,
                // which is both louder and less lifelike than moving in bursts.
                _idle_until = now_ms + restDuration(_gap_min_ms, _gap_max_ms);
            }
            return;
        }

        if (now_ms < _idle_until) {
            return;
        }
        // Do not start a behaviour on top of a move that is still finishing -- including
        // one somebody else commanded.
        if (stackchan.motion().isMoving()) {
            return;
        }

        _active = pick();
        if (_active != nullptr) {
            _active->begin(ctx);
        }
    }

    void setGap(uint32_t min_ms, uint32_t max_ms)
    {
        _gap_min_ms = min_ms;
        _gap_max_ms = max_ms;
    }

private:
    void add(std::unique_ptr<Behavior> b)
    {
        _total_weight += b->weight();
        _behaviors.push_back(std::move(b));
    }

    Behavior* pick() const
    {
        if (_behaviors.empty() || _total_weight <= 0) {
            return nullptr;
        }
        int roll = Random::getInstance().getInt(0, _total_weight - 1);
        for (const auto& b : _behaviors) {
            roll -= b->weight();
            if (roll < 0) {
                return b.get();
            }
        }
        return _behaviors.back().get();
    }

    std::vector<std::unique_ptr<Behavior>> _behaviors;
    int _total_weight     = 0;
    Behavior* _active     = nullptr;   // non-owning; points into _behaviors
    uint32_t _idle_until  = 0;
    uint32_t _gap_min_ms  = 5000;
    uint32_t _gap_max_ms  = 11000;
};

}  // namespace stackchan::idle
