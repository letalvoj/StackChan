/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <cstdint>

namespace stackchan::motion {

class Motion;

/**
 * @brief What a modifier or an idle behaviour needs from the head.
 *
 * Deliberately smaller than Motion. Motion owns two Servo objects, and a Servo reaches the
 * bus, so anything that merely wanted to *drive* the head had to link the hardware to do
 * it. That made the face impossible to exercise off-device: a preview that wants to run
 * the real BlinkModifier had no way to satisfy it without a servo stack, so it ended up
 * reimplementing the blink instead -- and the reimplementation drifted.
 *
 * Everything here is behaviour, phrased in angles and speeds. Calibration, zeroing and
 * direct servo access stay on Motion, because those are genuinely about hardware and the
 * code that needs them holds a Motion already.
 *
 * Angles are in tenths of a degree, matching Servo. Speed is 0-1000.
 */
class MotionControl {
public:
    virtual ~MotionControl() = default;

    /// Current head pose: x is yaw, y is pitch.
    virtual uitk::Vector2i getCurrentAngles() = 0;
    virtual int getCurrentYawAngle()          = 0;
    virtual int getCurrentPitchAngle()        = 0;
    virtual bool isMoving()                   = 0;

    virtual void moveWithSpeed(int yaw, int pitch, int speed) = 0;
    virtual void moveYawWithSpeed(int yaw, int speed)         = 0;
    virtual void movePitchWithSpeed(int pitch, int speed)     = 0;
    virtual void goHome(int speed = 500)                              = 0;
    virtual void stop()                                               = 0;

    /// While locked, ambient behaviours leave the head alone so a deliberate reaction --
    /// a shake, a head-pet -- can own it without being fought for.
    virtual void setModifyLock(bool locked) = 0;
    virtual bool isModifyLocked()           = 0;

    virtual void setTorqueEnabled(bool enabled)             = 0;
    virtual void setAutoTorqueReleaseEnabled(bool enabled)  = 0;
    virtual void setAutoAngleSyncEnabled(bool enabled)      = 0;

    /// Driven once per frame by whoever owns the head.
    virtual void update() = 0;

    /// Treat wherever the head is now as its home, and undo that.
    virtual void zeroHere()              = 0;
    virtual void resetZeroCalibration()  = 0;

    /// The servo pair behind this head, when there is one. Servo-level configuration --
    /// angle limits, offsets, the JSON settings blob -- is a hardware concern, and a head
    /// with no servos has nothing to configure. Everything else on this interface works
    /// without one, which is what lets the preview harness drive the real runtime.
    virtual Motion* hardware()
    {
        return nullptr;
    }
};

}  // namespace stackchan::motion
