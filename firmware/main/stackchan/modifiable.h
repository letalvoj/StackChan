/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "avatar/avatar.h"
#include "motion/motion_control.h"
#include "addons/neon_light/neon_light.h"
#include "utils/object_pool.h"

namespace stackchan {

/**
 * @brief Modifiable base class, expose modifiable APIs to the modifiers
 *
 */
class Modifiable {
public:
    virtual ~Modifiable() = default;

    /// Narrow on purpose: a modifier drives the head, it does not configure servos.
    /// Callers that genuinely need the servos reach them through MotionControl::hardware(),
    /// which is null for a head that has none.
    virtual motion::MotionControl& motion() = 0;

    virtual avatar::Avatar& avatar() = 0;

    virtual bool hasAvatar() = 0;

    // virtual bool hasMotion() = 0; // Motion must be always present

    virtual addon::NeonLight& leftNeonLight() = 0;

    virtual addon::NeonLight& rightNeonLight() = 0;
};

/**
 * @brief Modifier base class
 *
 */
class Modifier : public Poolable {
public:
    virtual void _update(Modifiable& stackchan)
    {
    }
};

}  // namespace stackchan
