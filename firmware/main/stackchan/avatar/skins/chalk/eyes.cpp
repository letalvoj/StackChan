/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "chalk.h"
#include "../../../face_states.h"
#include <hal/hal.h>

using namespace uitk;
using namespace stackchan::avatar;
namespace art  = stackchan::avatar::chalk;
namespace face = stackchan::face;

// Weight is 0..100 lid opening. The artist drew lids as frames rather than a continuum, so
// weight selects a drawing. The thresholds sit on the values the blink modifier actually
// produces -- it closes to 25 through a half step at 45 -- so nothing lands in a state by
// accident and every state the modifier reaches has a drawing.
static const int kClosedBelow = 35;
static const int kHalfBelow   = 60;

// Frames of the blink family: 0 open, 1 lids starting down, 2 half, 3 shut.
static const uint8_t kBlinkHalfFrame   = 1;
static const uint8_t kBlinkClosedFrame = 3;

// How far the whole eye may slide across the face, for the idle drift and breathing.
// Much smaller than the pupil's travel: the art direction is explicit that whole-face
// movement should be far less than gaze, or the face reads as sliding rather than looking.
static const int kPositionRangeX = 8;
static const int kPositionRangeY = 8;

ChalkEyes::ChalkEyes(lv_obj_t* parent, bool isLeftEye)
{
    _is_left_eye = isLeftEye;

    // The shell first, the pupil after, so the pupil draws inside the eye it sits in.
    _shell = std::make_unique<ClipTrack>(parent, 1);
    _pupil = std::make_unique<Sprite>(parent);

    _weight = face::kEyeRestWeight;
    refresh();
    setGaze({0, 0});
}

void ChalkEyes::refresh()
{
    const art::EmotionClips& clips = art::clipsFor(_emotion);
    const art::Clip* emotionClip   = _is_left_eye ? clips.eyesLeft : clips.eyesRight;
    const art::Clip* blinkClip     = _is_left_eye ? &art::clip_eyes_blink_left
                                                  : &art::clip_eyes_blink_right;

    // A blink borrows the lids and hands them back. The art direction asks for exactly
    // this: the blink temporarily replaces the lids and returns to the emotional eye.
    bool showPupil = true;
    if (_weight < kClosedBelow) {
        _shell->hold(blinkClip, kBlinkClosedFrame);
        showPupil = false;
    } else if (_weight < kHalfBelow) {
        _shell->hold(blinkClip, kBlinkHalfFrame);
        showPupil = false;
    } else {
        _shell->hold(emotionClip, clips.eyesRestFrame);
        // Whether a pupil belongs on this frame is the artist's call, recorded per frame
        // in the pack: an arc or a narrowed dome has no white to put one in, and the
        // asymmetric curious frames drop one side deliberately.
        showPupil = art::pupilVisible(emotionClip, clips.eyesRestFrame, _is_left_eye);
    }

    _shell->setVisible(_visible);
    _pupil->setVisible(_visible && showPupil);
}

void ChalkEyes::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion()) {
        return;
    }
    _emotion = emotion;

    // Every emotion rests wide, and is carried by its own drawing. Weight then means one
    // thing only -- how far the lid is down -- so a blink composes with any emotion
    // instead of fighting it.
    _weight = face::kEyeRestWeight;

    refresh();
    setGaze(_gaze);   // the gaze limit is per emotion, so re-clamp against the new one
}

void ChalkEyes::setWeight(int weight)
{
    Feature::setWeight(weight);
    refresh();
}

void ChalkEyes::setPosition(const uitk::Vector2i& position)
{
    Element::setPosition(position);
    const int dx = map_range(_position.x, -100, 100, -kPositionRangeX, kPositionRangeX);
    const int dy = map_range(_position.y, -100, 100, -kPositionRangeY, kPositionRangeY);

    _shell->setOffset(dx, dy);
    setGaze(_gaze);   // the pupil rides on the eye
}

void ChalkEyes::setGaze(const uitk::Vector2i& gaze)
{
    Feature::setGaze(gaze);

    const art::EmotionClips& clips = art::clipsFor(_emotion);
    const int eye_dx = map_range(_position.x, -100, 100, -kPositionRangeX, kPositionRangeX);
    const int eye_dy = map_range(_position.y, -100, 100, -kPositionRangeY, kPositionRangeY);
    const int gx     = map_range(_gaze.x, -100, 100, -clips.gazeLimit.x, clips.gazeLimit.x);
    const int gy     = map_range(_gaze.y, -100, 100, -clips.gazeLimit.y, clips.gazeLimit.y);

    // The pupil is drawn around its own origin, so a zero gaze puts it dead centre in the
    // eye. Front and centre is where the robot should spend most of its time looking;
    // anything else is the idle animation choosing to look somewhere.
    const lv_point_t anchor = _is_left_eye ? art::kAnchor_eye_left : art::kAnchor_eye_right;
    _pupil->setImage(art::pupil.dsc, anchor.x + art::pupil.cx, anchor.y + art::pupil.cy,
                     ChalkAvatar::kInk);
    _pupil->setOffset(eye_dx + gx, eye_dy + gy);
}

void ChalkEyes::setRotation(int rotation)
{
    Element::setRotation(rotation);
    _shell->setRotation(_rotation);
}

void ChalkEyes::setVisible(bool visible)
{
    Element::setVisible(visible);
    refresh();
}

void ChalkEyes::_update()
{
    // A played family advances itself; a held one does nothing here.
    _shell->advance(GetHAL().millis());
}
