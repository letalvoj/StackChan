/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

/**
 * @brief The feature weights the runtime actually produces.
 *
 * These live apart from the modifiers that use them so the preview harness can reach them
 * without dragging in servos and FreeRTOS. That matters more than it sounds: the harness
 * used to carry its own hand-typed copies, and they drifted. It rendered blinks at weight
 * 0 when the blink modifier closes to 25, and speaking mouths at 30 and 100 when the
 * speaking modifier only ever emits 0 to 20 and 40 to 80. Nine of its eighteen tiles were
 * showing states the device could not reach.
 *
 * Anything here is a fact about the face's behaviour, not about one skin's drawing.
 */
namespace stackchan::face {

/// The panel the face is drawn on. Every skin used to hardcode this in its own init() and
/// the preview harness hardcoded it again, so nothing could tell you whether a face that
/// fit the preview would fit the device.
inline constexpr int kScreenWidth  = 320;
inline constexpr int kScreenHeight = 240;

/// Every emotion rests with the eyes wide; the emotion is carried by shape, not by lid.
inline constexpr int kEyeRestWeight = 100;

/// Eye weight at the bottom of a blink, and the intermediate the lid passes through.
inline constexpr int kBlinkClosedWeight = 25;
inline constexpr int kBlinkHalfWeight   = 45;

/// Mouth weight bands while speaking: shut between syllables, open on them.
///
/// The closed band starts at 1, not 0, and that one unit matters. Weight zero is the
/// signal for "not speaking at all", and a skin reads it that way -- so a closed beat that
/// happened to roll a literal 0 dropped the mouth out of its speech family and back to the
/// emotion's resting shape for that beat. At one chance in twenty-one it showed up as a
/// shape popping through a sentence every few seconds.
inline constexpr int kSpeakClosedMin = 1;
inline constexpr int kSpeakClosedMax = 20;
inline constexpr int kSpeakOpenMin   = 40;
inline constexpr int kSpeakOpenMax   = 80;

/// The three mouth openings the artwork defines, and which the skin interpolates between.
inline constexpr int kMouthSmall  = 25;
inline constexpr int kMouthMedium = 60;
inline constexpr int kMouthWide   = 100;

}  // namespace stackchan::face
