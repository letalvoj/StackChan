/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * GENERATED FILE -- do not edit by hand.
 * Produced by tools/facegen/gen_clips.py from the artist's expression pack.
 * Regenerate with:  python3 tools/facegen/gen_clips.py
 */

#pragma once
#include <lvgl.h>

namespace stackchan::avatar::chalk {

/// One drawn frame of one colour layer. A null image means this layer draws
/// nothing on this frame, which is common: a mouth has no dark inside until it opens.
struct ClipFrame {
    const lv_image_dsc_t* dsc;
    int16_t cx;
    int16_t cy;
};

/// One colour of a clip, across all its frames.
struct ClipLayer {
    const ClipFrame* frames;
    uint32_t color;
};

struct Clip;

/// What the rest of the face does while one mouth frame is showing.
///
/// Most mouths leave the face alone. A laugh does not: the artist drew it as one
/// performance, and a "ha" always wears the > < squeeze and lifted cheeks, a "catch"
/// the softer arc. Pairing them per mouth frame keeps the face coherent however the
/// mouth got there -- played as a burst, or walked by a speech amplitude.
struct ClipCompanion {
    const Clip* eyesLeft;
    const Clip* eyesRight;
    uint8_t eyesFrame;
    const Clip* cheeksLeft;
    const Clip* cheeksRight;
    uint8_t cheeksFrame;
    /// Whole-face vertical lift in panel pixels, negative is up.
    int8_t faceY;
};

/// A drawn motion family: layers to stack, and how long each frame is held.
struct Clip {
    const ClipLayer* layers;
    const uint16_t* holdMs;
    /// Per frame, which pupils may be drawn: bit 0 left, bit 1 right. Null for
    /// anything that is not an eye. The artist gates these because an arc or a
    /// narrowed dome has no white to put a pupil in, and the curious frames drop
    /// one side on purpose.
    const uint8_t* pupils;
    /// Per frame, what the rest of the face does. Null for a mouth that leads nothing.
    const ClipCompanion* companions;
    uint8_t layerCount;
    uint8_t frameCount;
};

static constexpr float kPackScale = 1.5f;

/// Whether this eye may draw its pupil on this frame.
inline bool pupilVisible(const Clip* clip, uint8_t frame, bool leftEye)
{
    if (!clip || !clip->pupils || frame >= clip->frameCount) {
        return clip && !clip->pupils;   // non-eye clips do not gate anything
    }
    const uint8_t mask = clip->pupils[frame];
    return leftEye ? (mask & 1u) != 0u : (mask & 2u) != 0u;
}

extern const uint8_t clip_mouth_quiet_smile_openness[];
static constexpr uint8_t clip_mouth_quiet_smile_opennessCount = 6;
extern const Clip clip_mouth_quiet_smile;
extern const uint8_t clip_mouth_speech_neutral_openness[];
static constexpr uint8_t clip_mouth_speech_neutral_opennessCount = 6;
extern const Clip clip_mouth_speech_neutral;
extern const uint8_t clip_mouth_speech_delighted_openness[];
static constexpr uint8_t clip_mouth_speech_delighted_opennessCount = 6;
extern const Clip clip_mouth_speech_delighted;
extern const uint8_t clip_mouth_speech_grumpy_openness[];
static constexpr uint8_t clip_mouth_speech_grumpy_opennessCount = 6;
extern const Clip clip_mouth_speech_grumpy;
extern const uint8_t clip_mouth_speech_sad_openness[];
static constexpr uint8_t clip_mouth_speech_sad_opennessCount = 6;
extern const Clip clip_mouth_speech_sad;
extern const uint8_t clip_mouth_thinking_openness[];
static constexpr uint8_t clip_mouth_thinking_opennessCount = 6;
extern const Clip clip_mouth_thinking;
extern const uint8_t clip_mouth_yawn_openness[];
static constexpr uint8_t clip_mouth_yawn_opennessCount = 6;
extern const Clip clip_mouth_yawn;
extern const Clip clip_eyes_blink_left;
extern const Clip clip_eyes_blink_right;
extern const Clip clip_eyes_smile_squeeze_left;
extern const Clip clip_eyes_smile_squeeze_right;
extern const Clip clip_eyes_curious_widen_left;
extern const Clip clip_eyes_curious_widen_right;
extern const Clip clip_eyes_grumpy_squint_left;
extern const Clip clip_eyes_grumpy_squint_right;
extern const Clip clip_eyes_heavy_sad_left;
extern const Clip clip_eyes_heavy_sad_right;
extern const Clip clip_eyes_sleepy_drift_left;
extern const Clip clip_eyes_sleepy_drift_right;
extern const Clip clip_brows_curiosity_left;
extern const Clip clip_brows_curiosity_right;
extern const Clip clip_brows_stubborn_left;
extern const Clip clip_brows_stubborn_right;
extern const Clip clip_brows_concern_left;
extern const Clip clip_brows_concern_right;
extern const Clip clip_brows_soft_delight_left;
extern const Clip clip_brows_soft_delight_right;
extern const Clip clip_cheeks_warm_blush_left;
extern const Clip clip_cheeks_warm_blush_right;
extern const Clip clip_cheeks_smile_lift_left;
extern const Clip clip_cheeks_smile_lift_right;
extern const Clip clip_cheeks_puffed_hmph_left;
extern const Clip clip_cheeks_puffed_hmph_right;
extern const Clip clip_cheeks_pet_warmth_left;
extern const Clip clip_cheeks_pet_warmth_right;
extern const Clip clip_accents_heart_pulse;
extern const Clip clip_accents_dizzy_spin;
extern const Clip clip_accents_sweat_slip;
extern const Clip clip_accents_anger_twitch;
extern const uint8_t clip_mouth_laugh_openness[];
static constexpr uint8_t clip_mouth_laugh_opennessCount = 6;
extern const Clip clip_mouth_laugh;
extern const uint8_t clip_mouth_laugh_burst_openness[];
static constexpr uint8_t clip_mouth_laugh_burst_opennessCount = 9;
extern const Clip clip_mouth_laugh_burst;
extern const Clip clip_eyes_laugh_left;
extern const Clip clip_eyes_laugh_right;

/// The pupil, placed by the skin rather than drawn per frame: every gaze frame
/// in the pack is this same dot translated, and a continuous offset covers all of
/// them plus everything in between.
extern const ClipFrame pupil;

/// Anchor points, in pixels from the panel centre.
static constexpr lv_point_t kAnchor_eye_left = {-64, -20};
static constexpr lv_point_t kAnchor_eye_right = {64, -22};
static constexpr lv_point_t kAnchor_cheek_left = {-96, 26};
static constexpr lv_point_t kAnchor_cheek_right = {92, 26};
static constexpr lv_point_t kAnchor_brow_left = {-66, -66};
static constexpr lv_point_t kAnchor_brow_right = {66, -65};
static constexpr lv_point_t kAnchor_mouth = {0, 48};
static constexpr int kEyeRadius = 27;

/// How far the pupil may travel inside each eye family, in local units.
static constexpr lv_point_t kGazeLimit_blink = {12, 9};
static constexpr lv_point_t kGazeLimit_smile_squeeze = {12, 9};
static constexpr lv_point_t kGazeLimit_curious_widen = {12, 9};
static constexpr lv_point_t kGazeLimit_grumpy_squint = {4, 4};
static constexpr lv_point_t kGazeLimit_heavy_sad = {4, 4};
static constexpr lv_point_t kGazeLimit_sleepy_drift = {12, 9};

/// Every baked family, for review tooling.
struct NamedClip {
    const char* name;
    const Clip* clip;
};
extern const NamedClip kAllClips[];
static constexpr int kAllClipCount = 43;

}  // namespace stackchan::avatar::chalk
