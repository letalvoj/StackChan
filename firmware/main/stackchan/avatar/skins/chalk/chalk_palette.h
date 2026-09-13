/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../../avatar/elements/emotion.h"
#include "chalk_clips.h"
#include <lvgl.h>

namespace stackchan::avatar::chalk {

/**
 * @brief Which drawn families an emotion is made of.
 *
 * An emotion used to select one drawing per feature. The expression pack draws each
 * feature as a *family* of frames instead -- a blink, a squint settling in, a mouth
 * working through its speech poses -- so an emotion now selects a family per track and
 * each track runs on its own clock. That independence is what the art direction asks for:
 * parts that all start together read as a machine.
 *
 * A null clip means the emotion does not draw that feature at all, which is how the
 * reference draws neutral (no brows) and sleepy (no brows, no cheeks).
 */
struct EmotionClips {
    const Clip* eyesLeft   = nullptr;
    const Clip* eyesRight  = nullptr;
    /// Which frame of the eye family is this emotion's settled look. The families are
    /// transitions, so the resting face is a frame partway through rather than frame zero.
    uint8_t eyesRestFrame = 0;

    const Clip* mouthRest   = nullptr;   ///< held when not speaking
    uint8_t mouthRestFrame  = 0;
    const Clip* mouthSpeech = nullptr;   ///< the emotional speech family

    /// The family a silent mouth may play through as an occasional fidget, or null to
    /// hold still.
    ///
    /// Deliberately not the same field as mouthRest, and the distinction is the artist's.
    /// Every speech row is labelled "audio-driven", and three emotions rest on a frame of
    /// one -- so fidgeting with mouthRest meant a silent grumpy or sad face played its
    /// talking animation to itself. Only quiet-smile, thinking and yawn are drawn as
    /// things a quiet mouth does; the rest simply stay put.
    const Clip* mouthIdle = nullptr;

    /// Play mouthIdle as soon as the face takes on this emotion, instead of waiting out the
    /// usual quiet gap. Being told to laugh and then sitting there for four seconds first is
    /// not laughing.
    bool mouthIdleOnEnter = false;

    const Clip* browsLeft  = nullptr;
    const Clip* browsRight = nullptr;
    uint8_t browsRestFrame = 0;

    const Clip* cheeksLeft  = nullptr;
    const Clip* cheeksRight = nullptr;
    uint8_t cheeksRestFrame = 0;

    /// How far the pupil may travel in this emotion's eye, from the artist's metadata.
    /// A narrowed sad or grumpy eye has far less white to move inside than an open one.
    lv_point_t gazeLimit{12, 9};

    /// Frames of the speech family ordered from most closed to most open.
    ///
    /// The artist is explicit that a speech row is a pose palette and its left-to-right
    /// order is an audition sequence, not a spoken syllable -- so choosing an openness
    /// ladder is ours to make. This is that choice: with only amplitude to go on, the
    /// mouth walks this ladder instead of picking a new random pose every frame.
    const uint8_t* openness = nullptr;
    uint8_t opennessCount   = 0;
};

/// The palette for an emotion. Always returns something; neutral is the fallback.
const EmotionClips& clipsFor(Emotion emotion);

}  // namespace stackchan::avatar::chalk
