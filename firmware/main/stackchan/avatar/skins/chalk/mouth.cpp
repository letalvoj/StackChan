/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "chalk.h"
#include "../../../face_states.h"
#include "../../../utils/random.h"
#include <hal/hal.h>

using namespace uitk;
using namespace stackchan::avatar;
namespace art  = stackchan::avatar::chalk;
namespace face = stackchan::face;

// A mouth with any weight at all is a speaking mouth. Only a weight of exactly zero is
// rest.
//
// This matters more than it looks. The speaking modifier shuts the jaw between syllables,
// dropping into 0..20, and treating that as "at rest" sent the mouth back to the emotion's
// resting shape on every closed beat -- so a talking face flickered in and out of its own
// speech family and lost the corners that family exists to hold. The artist's reference
// keeps a talking mouth inside its family throughout, closed beats included.

// How long a resting mouth stays still between its own small movements. The artist marks
// the quiet mouth families "occasional, not continuous", so this is a long, jittered gap
// rather than a loop: a mouth that fidgets constantly is as wrong as one that never moves.
static const int kFlickerMinMs = 2600;
static const int kFlickerMaxMs = 6200;

// How far the mouth may slide, for the idle drift and breathing.
static const int kPositionRangeX = 6;
static const int kPositionRangeY = 8;

ChalkMouth::ChalkMouth(lv_obj_t* parent)
{
    // Two layers: the chalk shape, and the dark inside that appears once it opens.
    _track = std::make_unique<ClipTrack>(parent, 2);
    _weight = 0;
    refresh();
}

void ChalkMouth::refresh()
{
    const art::EmotionClips& clips = art::clipsFor(_emotion);

    if (_weight <= 0) {
        _track->hold(clips.mouthRest, clips.mouthRestFrame);
        _track->setVisible(_visible);
        return;
    }

    // Talking is not one shape scaled up. Each emotion has its own drawn speech poses, so
    // the corners it holds survive the jaw opening -- which is the whole reason the pack
    // draws a separate mouth row per emotional family instead of resizing one oval.
    const art::Clip* clip = clips.mouthSpeech ? clips.mouthSpeech : clips.mouthRest;
    uint8_t frame         = clips.mouthRestFrame;

    if (clips.openness && clips.opennessCount > 0) {
        // Walk the openness ladder rather than picking a pose at random. With only an
        // amplitude to go on, a new random mouth every frame reads as chewing.
        // Scale against the loudest the speaking modifier actually gets, not against the
        // nominal 100. Indexed over 0..100 the top of every ladder was unreachable during
        // speech -- the widest pose the artist drew for each family simply never appeared,
        // because the jaw tops out at 80.
        const int span  = face::kSpeakOpenMax;
        const int step  = (_weight * (clips.opennessCount - 1) + span / 2) / span;
        const int index = uitk::clamp(step, 0, clips.opennessCount - 1);
        frame           = clips.openness[index];
    }

    _track->hold(clip, frame);
    _track->setVisible(_visible);
}

void ChalkMouth::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion()) {
        return;
    }
    // Being told the emotion it already has is not a reason to cut short what the mouth is
    // doing. A client re-sends "laughing" with every sentence, and snapping back to rest
    // each time would chop the laugh off in its first burst.
    if (emotion == _emotion && _flickering) {
        return;
    }
    const bool changed = emotion != _emotion;
    _emotion           = emotion;
    if (changed) {
        _flickering    = false;
        _idle_on_enter = art::clipsFor(_emotion).mouthIdleOnEnter;
    }
    refresh();
}

const art::ClipCompanion* ChalkMouth::lead() const
{
    const art::Clip* clip = _track->clip();
    if (!clip || !clip->companions || _track->frame() >= clip->frameCount) {
        return nullptr;
    }
    return &clip->companions[_track->frame()];
}

void ChalkMouth::setFaceY(int dy)
{
    if (dy == _face_y) {
        return;
    }
    _face_y = dy;
    place();
}

void ChalkMouth::setWeight(int weight)
{
    Feature::setWeight(weight);
    refresh();
}

void ChalkMouth::setPosition(const uitk::Vector2i& position)
{
    Element::setPosition(position);
    place();
}

void ChalkMouth::place()
{
    const int dx = map_range(_position.x, -100, 100, -kPositionRangeX, kPositionRangeX);
    const int dy = map_range(_position.y, -100, 100, -kPositionRangeY, kPositionRangeY);
    _track->setOffset(dx, dy + _face_y);
}

void ChalkMouth::setRotation(int rotation)
{
    Element::setRotation(rotation);
    // A tilt belongs to a resting mouth. Rotating one mid-syllable reads as a glitch
    // rather than as a wry expression, and the idle modifier is the only caller.
    if (_weight <= 0) {
        _track->setRotation(_rotation);
    }
}

void ChalkMouth::setVisible(bool visible)
{
    Element::setVisible(visible);
    _track->setVisible(visible);
}

void ChalkMouth::scheduleFlicker(uint32_t now)
{
    _next_flicker_ms = now + Random::getInstance().getInt(kFlickerMinMs, kFlickerMaxMs);
}

void ChalkMouth::_update()
{
    const uint32_t now = GetHAL().millis();
    _track->advance(now);

    // Only a mouth that is otherwise doing nothing gets to fidget. While speaking, the
    // weight owns the pose and a flicker on top of it would fight the syllables.
    if (_weight > 0) {
        _flickering = false;
        scheduleFlicker(now);
        return;
    }

    const art::EmotionClips& clips = art::clipsFor(_emotion);
    if (_flickering) {
        if (_track->finished()) {
            _flickering = false;
            _track->hold(clips.mouthRest, clips.mouthRestFrame);
            scheduleFlicker(now);
        }
        return;
    }

    if (_idle_on_enter && clips.mouthIdle) {
        // Played directly rather than by making the scheduled time "now": a clock that
        // reads zero -- the preview's, at its first frame -- is indistinguishable from "not
        // scheduled yet" below, and the laugh would wait out a whole quiet gap first.
        _idle_on_enter = false;
        _flickering    = true;
        _track->play(clips.mouthIdle, now, ClipTrack::Mode::Once);
        return;
    }
    if (_next_flicker_ms == 0) {
        scheduleFlicker(now);
        return;
    }
    if (now >= _next_flicker_ms && clips.mouthIdle) {
        // Play the idle family through once, on the artist's own timing, then settle.
        // Deliberately mouthIdle and not mouthRest: several emotions rest on a frame of
        // their speech row, and running that while silent is a face mouthing words to
        // itself. An emotion with no quiet family simply holds still.
        _flickering = true;
        _track->play(clips.mouthIdle, now, ClipTrack::Mode::Once);
    }
}
