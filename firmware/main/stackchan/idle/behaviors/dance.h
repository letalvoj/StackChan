/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../idle_behavior.h"

namespace stackchan::idle {

/**
 * @brief A short procedurally-generated dance. (weight 5)
 *
 * The point of this behaviour is that it is NOT a random walk. Randomness picks the
 * *parameters* once, at begin(); after that the motion is a deterministic function of the
 * beat index. That distinction is the whole difference between "the robot is dancing" and
 * "the robot is twitching": rhythm is repetition the eye can predict, and a fresh random
 * target every step destroys exactly that.
 *
 * What is randomised per performance: tempo, how many bars, amplitude, vertical bias, and
 * which of the step patterns is danced. What is not: the pattern itself, which is a closed
 * form in the beat index.
 *
 * Runs for a couple of seconds and stops. Rare (5%) and brief on purpose -- a desk robot
 * that breaks into dance every minute stops being charming very quickly.
 */
class Dance : public Behavior {
public:
    const char* name() const override
    {
        return "dance";
    }
    int weight() const override
    {
        return 5;
    }

    void begin(const Context& ctx) override
    {
        auto& rng = Random::getInstance();

        // 100-150 BPM, on the eighth note. Slower than this and it reads as a stretch
        // rather than a dance; faster and the servos cannot reach the target in time.
        const int bpm = rng.getInt(100, 150);
        _beat_ms      = static_cast<uint32_t>(30000 / bpm);        // half a beat
        _beats_total  = rng.getInt(3, 5) * kBeatsPerBar;           // 3-5 bars
        _beat         = 0;
        _next_beat_at = ctx.now_ms;

        _pattern   = static_cast<Pattern>(rng.getInt(0, kPatternCount - 1));
        _amplitude = deg(rng.getInt(18, 34));
        // Centre the dance on a comfortable forward-ish pose rather than wherever the head
        // happened to be left, so a dance that starts from a corner still looks composed.
        _pitch_base = deg(rng.getInt(26, 38));
        _bob        = deg(rng.getInt(5, 11));
    }

    bool update(const Context& ctx) override
    {
        if (ctx.now_ms < _next_beat_at) {
            return true;
        }
        if (_beat >= _beats_total) {
            // Settle back to a neutral, gentle pose so the dance has an ending rather than
            // just stopping wherever the last beat landed.
            if (!ctx.motion().isMoving()) {
                ctx.moveTo(0, deg(32), speed::kGentle);
                return false;
            }
            return true;
        }

        int yaw   = 0;
        int pitch = _pitch_base;
        step(_beat, yaw, pitch);

        ctx.moveTo(yaw, pitch, speed::kDance);

        _next_beat_at = ctx.now_ms + _beat_ms;
        ++_beat;
        return true;
    }

private:
    enum Pattern : int {
        kSway = 0,     // side to side, nodding on the downbeat
        kBounce,       // stays central, bobs hard, small alternating tilt
        kFigureEight,  // yaw and pitch in 2:1 -- traces a lying-down figure 8
        kPatternCount
    };

    static constexpr int kBeatsPerBar = 4;

    /// Closed form in the beat index. Deterministic on purpose -- see the class comment.
    void step(int beat, int& yaw, int& pitch) const
    {
        const int inBar = beat % kBeatsPerBar;
        switch (_pattern) {
            case kSway:
                // Left, centre, right, centre -- and dip on every downbeat.
                yaw   = (inBar == 0) ? -_amplitude : (inBar == 2) ? _amplitude : 0;
                pitch = _pitch_base - ((inBar % 2 == 0) ? _bob : 0);
                break;

            case kBounce:
                // Vertical accent carries the rhythm; yaw only flicks to keep it alive.
                yaw   = (beat % 2 == 0) ? -_amplitude / 3 : _amplitude / 3;
                pitch = _pitch_base + ((beat % 2 == 0) ? _bob : -_bob);
                break;

            case kFigureEight:
            default: {
                // Yaw completes one cycle per bar, pitch two -- the 2:1 ratio is what
                // makes it a figure eight rather than an ellipse.
                const float t = static_cast<float>(inBar) / kBeatsPerBar;
                yaw   = static_cast<int>(_amplitude * std::sin(2.0f * 3.14159265f * t));
                pitch = _pitch_base + static_cast<int>(_bob * std::sin(4.0f * 3.14159265f * t));
                break;
            }
        }
    }

    Pattern _pattern       = kSway;
    uint32_t _beat_ms      = 250;
    int _beats_total       = 12;
    int _beat              = 0;
    uint32_t _next_beat_at = 0;
    int _amplitude         = deg(25);
    int _pitch_base        = deg(32);
    int _bob               = deg(8);
};

}  // namespace stackchan::idle
