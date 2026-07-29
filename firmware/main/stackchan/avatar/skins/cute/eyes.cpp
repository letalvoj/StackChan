/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "cute.h"

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

// Big eyes, set fairly wide and slightly above centre. Eye size is the single strongest
// lever on "cute" -- the default skin's 16px discs read as instrumentation, these read as
// a character.
static const int _eye_diameter        = 46;
static const int _pupil_diameter      = 13;
static const Vector2i _eye_pos        = Vector2i(-52, -14);
static const Vector2i _eye_min_offset = Vector2i(-14, -14);
static const Vector2i _eye_max_offset = Vector2i(14, 14);

// The pupil sits low and toward the nose. Centred pupils stare; offset pupils look at
// something, and the reference image does exactly this.
static const Vector2i _pupil_offset = Vector2i(3, 5);

// Pixels of eye left visible when the lid is fully down -- the closed-eye line.
static const int kClosedSliver = 5;

CuteEyes::CuteEyes(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor, bool isLeftEye)
{
    _is_left_eye = isLeftEye;

    _container = std::make_unique<Container>(parent);
    _container->setRadius(0);
    _container->setAlign(LV_ALIGN_CENTER);
    _container->setBorderWidth(0);
    _container->setBgOpa(0);
    _container->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _container->setPadding(0, 0, 0, 0);
    _container->setTransformPivot(_eye_diameter / 2, _eye_diameter / 2);
    _container->setSize(_eye_diameter, _eye_diameter);

    _eye = std::make_unique<Container>(_container->get());
    _eye->setRadius(LV_RADIUS_CIRCLE);
    _eye->align(LV_ALIGN_CENTER, 0, 0);
    _eye->setSize(_eye_diameter, _eye_diameter);
    _eye->setBorderWidth(0);
    _eye->setBgColor(primaryColor);
    _eye->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _pupil = std::make_unique<Container>(_container->get());
    _pupil->setRadius(LV_RADIUS_CIRCLE);
    _pupil->setAlign(LV_ALIGN_CENTER);
    _pupil->setPos(_is_left_eye ? _pupil_offset.x : -_pupil_offset.x, _pupil_offset.y);
    _pupil->setSize(_pupil_diameter, _pupil_diameter);
    _pupil->setBorderWidth(0);
    _pupil->setBgColor(secondaryColor);
    _pupil->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    // Eyelid last so it covers both disc and pupil when it slides down.
    _eyelid = std::make_unique<Container>(_container->get());
    _eyelid->setRadius(0);
    _eyelid->align(LV_ALIGN_CENTER, 0, 0);
    _eyelid->setSize(_eye_diameter + 4, _eye_diameter + 4);
    _eyelid->setBorderWidth(0);
    _eyelid->setBgColor(secondaryColor);
    _eyelid->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    setSize(0);
    setWeight(100);
    setPosition(_position);
    setRotation(0);
}

CuteEyes::~CuteEyes()
{
    _eyelid.reset();
    _pupil.reset();
    _eye.reset();
    _container.reset();
}

void CuteEyes::setPosition(const Vector2i& position)
{
    Element::setPosition(position);

    auto pos_x = _is_left_eye ? _eye_pos.x : -_eye_pos.x;
    pos_x += map_range(_position.x, -100, 100, _eye_min_offset.x, _eye_max_offset.x);
    auto pos_y = _eye_pos.y + map_range(_position.y, -100, 100, _eye_min_offset.y, _eye_max_offset.y);

    _container->setPos(pos_x, pos_y);
    _eyelid->setY(_eyelid_offset_y);
}

void CuteEyes::setWeight(int weight)
{
    Feature::setWeight(weight);

    // weight 100 -> lid fully retracted; 0 -> almost covering, but NOT quite.
    //
    // kClosedSliver leaves a few pixels of the white disc showing at the bottom, which
    // reads as a closed eye. Covering completely makes the eyes vanish, and a blink where
    // the face briefly has no eyes at all looks broken rather than sleepy -- the default
    // skin has exactly this problem.
    const int travel = (int)_eyelid->getHeight() - kClosedSliver;
    _eyelid_offset_y = -map_range(_weight, 0, 100, 0, travel) - kClosedSliver;
    _eyelid->setY(_eyelid_offset_y);
}

void CuteEyes::setRotation(int rotation)
{
    Element::setRotation(rotation);
    _container->setRotation(rotation);
}

void CuteEyes::setVisible(bool visible)
{
    Element::setVisible(visible);
    _container->setHidden(!visible);
}

void CuteEyes::setSize(int size)
{
    Feature::setSize(size);

    const int d = uitk::clamp(_eye_diameter + map_range(_size, -100, 100, -16, 16), 8, 80);
    _eye->setSize(d, d);
}

void CuteEyes::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion()) {
        return;
    }

    // Emotion is carried by lid opening and lid TILT. Rotating the whole eye container
    // rotates the square lid across the round eye, which is what produces an angled lid --
    // the cheapest convincing eyebrow substitute there is.
    auto apply = [this](int weight, int rotation) {
        setWeight(weight);
        setRotation(_is_left_eye ? rotation : (3600 - rotation) % 3600);
    };

    // Rotation near 180 degrees flips the lid to come from the BOTTOM of the eye, which
    // is the ^_^ squint. That distinction matters more than any other value here: a lid
    // descending from the top is a glare no matter how you tune it, and it is why the
    // first pass had a "happy" face that looked ready for a fight.
    switch (emotion) {
        case Emotion::Happy:
            // Lid rises from below: pleased, not narrowed.
            apply(72, 1550);
            break;
        case Emotion::Angry:
            // Inner corners DOWN. Sign convention matches the default skin, which I had
            // inverted -- angry read as sad and sad as angry.
            apply(66, 450);
            break;
        case Emotion::Sad:
            // Inner corners UP: the opposite tilt, and the opposite reading.
            apply(64, 3200);
            break;
        case Emotion::Doubt:
            apply(76, 3520);
            break;
        case Emotion::Sleepy:
            // Heavy lids, barely tilted. Any real tilt here turns drowsy into smug.
            apply(28, 3550);
            break;
        case Emotion::Neutral:
        default:
            apply(100, 0);
            break;
    }
}
