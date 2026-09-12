/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../utils/random.h"
#include <smooth_ui_toolkit.hpp>
#include <hal/hal.h>
#include <cstdint>
#include <cstdlib>

namespace stackchan {

/**
 * @brief What the face does when nothing else is asking anything of it.
 *
 * This used to slide both eyes bodily around the face on a timer. That reads as the whole
 * face drifting rather than as the robot looking at something, because eyes that move
 * without a pupil moving inside them are not looking anywhere -- they are just being
 * repositioned. The face now mostly holds still and moves its pupils instead.
 *
 * Two things drive the pupils:
 *
 *  * Saccades. Eyes do not drift, they jump: a target is picked, held for a while, then
 *    abandoned for another. Most targets are near centre so the robot reads as facing the
 *    person in front of it, with the occasional wider glance to keep it alive.
 *
 *  * The head. When the head turns, the eyes swing the other way and then catch up, which
 *    is what real eyes do to hold a fixed point while the head moves. It costs one
 *    subtraction per update and it is most of what makes a turn look intentional rather
 *    than like the whole head being dragged.
 */
class IdleExpressionModifier : public Modifier {
public:
    IdleExpressionModifier(uint32_t interval_min = 900, uint32_t interval_max = 2600)
        : _interval_min(interval_min), _interval_max(interval_max)
    {
        _next_tick = GetHAL().millis() + 500;
    }

    void _update(Modifiable& stackchan) override
    {
        if (!stackchan.hasAvatar() || stackchan.avatar().isModifyLocked()) {
            return;
        }

        uint32_t now = GetHAL().millis();

        track_head(stackchan, now);

        if (now >= _next_tick) {
            perform_idle_glance(stackchan.avatar());
            _next_tick = now + Random::getInstance().getInt(_interval_min, _interval_max);
        }

        apply_gaze(stackchan.avatar());
    }

private:
    /* ------------------------------ Head tracking ----------------------------- */

    /**
     * @brief Swing the eyes against the head's movement, then let them catch up.
     *
     * Servo angles are in the same units the motion API uses throughout; only the change
     * between updates matters here, so the absolute scale never has to be agreed on.
     */
    void track_head(Modifiable& stackchan, uint32_t now)
    {
        const uitk::Vector2i angles = stackchan.motion().getCurrentAngles();

        if (!_have_prev_angles) {
            _prev_angles      = angles;
            _have_prev_angles = true;
            return;
        }

        const int dx = angles.x - _prev_angles.x;
        const int dy = angles.y - _prev_angles.y;
        _prev_angles = angles;

        // A large jump is the head being commanded somewhere new rather than tracking, and
        // countering it fully would peg the pupils at the edge of the eye.
        if (std::abs(dx) < kTrackingIgnore && std::abs(dy) < kTrackingIgnore) {
            _vor.x -= dx / kTrackingDivisor;
            _vor.y -= dy / kTrackingDivisor;
        }

        // Decay back toward centre, so the counter-swing is a transient and the eyes end
        // up looking where the head is pointed rather than permanently off to one side.
        if (now >= _next_decay_tick) {
            _next_decay_tick = now + kDecayIntervalMs;
            _vor.x -= _vor.x / 3;
            _vor.y -= _vor.y / 3;
        }

        _vor.clamp({-kVorLimit, -kVorLimit}, {kVorLimit, kVorLimit});
    }

    /* -------------------------------- Saccades -------------------------------- */

    void perform_idle_glance(avatar::Avatar& avatar)
    {
        auto& random = Random::getInstance();
        const int roll = random.getInt(0, 100);

        if (roll < 45) {
            // Small glance: the robot is still looking at you, just not rigidly.
            _target.x = random.getInt(-28, 28);
            _target.y = random.getInt(-22, 22);
        } else if (roll < 70) {
            // Look away and off to one side. Committing to a direction reads as attention
            // going somewhere, where a small symmetric jitter reads as a twitch.
            _target.x = random.getInt(0, 1) ? random.getInt(38, 78) : random.getInt(-78, -38);
            _target.y = random.getInt(-30, 20);
        } else if (roll < 82) {
            // Up: thinking. Down: a little shy.
            _target.x = random.getInt(-20, 20);
            _target.y = random.getInt(0, 1) ? random.getInt(-70, -40) : random.getInt(40, 66);
        } else if (roll < 94) {
            // Back to front and centre, which is where it should spend most of its time.
            _target = {0, 0};
        } else {
            // Rarely, the whole face shifts and the mouth tilts -- the old behaviour, kept
            // as an occasional accent rather than as the whole idle animation.
            const int ox = random.getInt(-8, 8);
            const int oy = random.getInt(-6, 6);
            avatar.leftEye().setPosition({ox, oy});
            avatar.rightEye().setPosition({ox, oy});
            avatar.mouth().setPosition({0, random.getInt(0, 6)});
            const int rotation = random.getInt(-30, 30);
            avatar.mouth().setRotation(rotation < 0 ? 3600 + rotation : rotation);
            _target = {0, 0};
        }
    }

    void apply_gaze(avatar::Avatar& avatar)
    {
        uitk::Vector2i gaze = {_target.x + _vor.x, _target.y + _vor.y};
        gaze.clamp({-100, -100}, {100, 100});

        // Writing the same position every frame still marks the eye dirty, and the eyes are
        // the largest thing on the face to redraw. Most frames the gaze has not changed.
        if (gaze.x == _applied.x && gaze.y == _applied.y) {
            return;
        }
        _applied = gaze;

        avatar.leftEye().setGaze(gaze);
        avatar.rightEye().setGaze(gaze);
    }

    // Head movement larger than this in one update is a commanded move, not tracking.
    static constexpr int kTrackingIgnore = 220;
    // Servo units per unit of gaze. Chosen so an ordinary look-around swings the pupils
    // noticeably without slamming them into the corner of the eye.
    static constexpr int kTrackingDivisor = 6;
    static constexpr int kVorLimit        = 55;
    static constexpr uint32_t kDecayIntervalMs = 120;

    uint32_t _next_tick  = 0;
    uint32_t _interval_min;
    uint32_t _interval_max;

    uitk::Vector2i _target{};   ///< where the saccade put the pupils
    uitk::Vector2i _vor{};      ///< the counter-swing owed to head movement
    uitk::Vector2i _applied{};  ///< last gaze actually written to the eyes

    uitk::Vector2i _prev_angles{};
    bool _have_prev_angles     = false;
    uint32_t _next_decay_tick  = 0;
};

}  // namespace stackchan
