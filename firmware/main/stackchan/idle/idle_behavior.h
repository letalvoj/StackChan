/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../utils/random.h"
#include <smooth_ui_toolkit.hpp>
#include <cmath>
#include <cstdint>

// Shared vocabulary for idle behaviours.
//
// An idle "behaviour" is a short-lived mode the robot enters while nobody is talking to
// it: a slow look around, a glance back to centre, a little dance. The director (see
// idle_director.h) picks one at random by weight, runs it to completion, waits, and
// picks again.
//
// To add a behaviour: subclass Behavior, implement begin()/update(), and register it in
// the director with a weight. Nothing else needs to change.

namespace stackchan::idle {

// ---------------------------------------------------------------------------- units
//
// Motion works in DECIDEGREES (yaw -800..800 is -80..80 degrees). Mixing the two up is
// a factor-of-ten bug that presents as "the robot barely moves" or "the robot slams
// into its end stops", so degrees never appear raw below -- always deg(x).
constexpr int deg(int degrees)
{
    return degrees * 10;
}

// Mechanical envelope. Behaviours clamp to this; nothing should ever command past it.
struct Envelope {
    static constexpr int kYawMin   = deg(-80);
    static constexpr int kYawMax   = deg(80);
    static constexpr int kPitchMin = deg(0);
    static constexpr int kPitchMax = deg(60);

    static int clampYaw(int v)
    {
        return uitk::clamp(v, kYawMin, kYawMax);
    }
    static int clampPitch(int v)
    {
        return uitk::clamp(v, kPitchMin, kPitchMax);
    }
};

// ---------------------------------------------------------------------- servo speed
//
// moveWithSpeed takes 0-1000. The servos are audible, and this robot lives on an office
// desk, so idle motion stays in the quiet end of the range. Anything above ~150 is
// clearly heard across a room; the dance is allowed to break this because it is rare,
// brief, and meant to be noticed.
// Servo noise scales with how fast the horn is driven, and the *long* moves are the
// worst offenders: they hold that speed for longer, so a fast long sweep is the one
// thing you actually hear across a room. Hence the ordering below is inverted from the
// obvious one -- the further the head travels, the gentler it is driven.
namespace speed {
constexpr int kDrift    = 34;    // small steps; barely audible
constexpr int kGentle   = 68;    // deliberate but soft
constexpr int kPurposed = 85;    // the long repositioning move -- slowest per degree
constexpr int kDance    = 320;   // rhythmic and audible (behaviour currently disabled)
}  // namespace speed

// ------------------------------------------------------------------------- sampling

/**
 * @brief Normally-distributed sample, truncated to [min, max].
 *
 * Box-Muller from the shared RNG. Truncation is by resampling rather than clamping:
 * clamping would pile probability mass onto the two end points, which shows up as a
 * robot that keeps staring at exactly its extreme angles instead of mostly near centre.
 */
inline float gaussian(float mean, float stddev, float min, float max)
{
    auto& rng = Random::getInstance();
    for (int attempt = 0; attempt < 8; ++attempt) {
        float u1 = rng.getFloat(1e-6f, 1.0f);
        float u2 = rng.getFloat(0.0f, 1.0f);
        float z  = std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * 3.14159265f * u2);
        float v  = mean + z * stddev;
        if (v >= min && v <= max) {
            return v;
        }
    }
    return uitk::clamp(mean, min, max);   // pathological RNG; centre is a safe answer
}

/**
 * @brief A pause that feels considered rather than scheduled.
 *
 * Uniform sampling over one range is what makes idle motion read as mechanical: every
 * pause lands in the same narrow band, so the robot fidgets on an audible metronome even
 * though the numbers are technically random.
 *
 * A mixture fixes it. Most pauses are ordinary; roughly one in four is a long settle
 * where the robot simply holds still and looks at something. The long tail is what sells
 * "resting" -- and on a desk, the stillness is the point.
 */
inline uint32_t restDuration(uint32_t base_min_ms, uint32_t base_max_ms)
{
    auto& rng = Random::getInstance();
    if (rng.getInt(0, 99) < 25) {
        // Long settle: two to three times the ordinary pause.
        return rng.getInt(static_cast<int>(base_max_ms * 2), static_cast<int>(base_max_ms * 3));
    }
    return rng.getInt(static_cast<int>(base_min_ms), static_cast<int>(base_max_ms));
}

// -------------------------------------------------------------------------- context

/// Everything a behaviour is allowed to touch, passed by reference each tick.
struct Context {
    Modifiable& stackchan;
    uint32_t now_ms;

    motion::Motion& motion() const
    {
        return stackchan.motion();
    }

    /// Current head position in decidegrees; .x is yaw, .y is pitch.
    uitk::Vector2i angles() const
    {
        return stackchan.motion().getCurrentAngles();
    }

    /// Issue a clamped move. Every behaviour goes through here, so the envelope is
    /// enforced in exactly one place.
    void moveTo(int yaw, int pitch, int spd) const
    {
        stackchan.motion().moveWithSpeed(Envelope::clampYaw(yaw), Envelope::clampPitch(pitch), spd);
    }
};

// ------------------------------------------------------------------------- behaviour

/**
 * @brief One idle mode.
 *
 * Lifecycle: begin() once, then update() every tick until it returns false. A behaviour
 * owns its own timing -- the director does not impose a step rate, because a dance needs
 * beats and a slow look-around needs long pauses, and forcing both through one cadence
 * is how you get a robot that twitches.
 */
class Behavior {
public:
    virtual ~Behavior() = default;

    /// Short name, for logging.
    virtual const char* name() const = 0;

    /// Relative likelihood of being chosen. Weights need not sum to anything.
    virtual int weight() const = 0;

    /// Called once when the behaviour is selected.
    virtual void begin(const Context& ctx) = 0;

    /// Called every tick. Return false when finished.
    virtual bool update(const Context& ctx) = 0;
};

}  // namespace stackchan::idle
