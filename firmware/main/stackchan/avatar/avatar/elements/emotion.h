/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace stackchan::avatar {

enum class Emotion {
    Neutral = 0,
    Happy,
    Angry,
    Sad,
    Doubt,
    Sleepy,
    /// A real laugh, not a wide smile: its own open mouths, the > < squeeze, and a burst
    /// that plays out on its own. Appended rather than placed beside Happy so the values of
    /// the existing emotions do not move.
    Laugh,
};

}  // namespace stackchan::avatar
