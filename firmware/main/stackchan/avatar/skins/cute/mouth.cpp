/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "cute.h"

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

// Higher and smaller than the first pass. A mouth low on the face reads adult and a
// bit glum; lifting it and shrinking it is most of what makes a face read as babyish.
static const Vector2i _mouth_pos        = Vector2i(0, 28);
static const Vector2i _mouth_min_offset = Vector2i(-12, -12);
static const Vector2i _mouth_max_offset = Vector2i(12, 12);

static const int _smile_size   = 58;    // bounding box of the arc
static const int _smile_width  = 7;     // stroke thickness

// Handoff band: the smile holds until the mouth is clearly opening, then swaps quickly.
// Narrow band on purpose. Any frame that lands mid-fade shows two half-opaque shapes
// overlapping, which reads as grey mush rather than a mouth -- so the swap is quick
// enough that speech never lingers there. At 30 fps a fast swap looks like a mouth
// opening; a slow one looks like a rendering bug.
static const int kSmileHold = 11;
static const int kSwap      = 19;

// Quiet speech should be a small movement, and a wide-open mouth is TALLER than it is
// wide -- widening it instead just looks like a grimace.
static const Vector2i _open_min_size = Vector2i(14, 5);
static const Vector2i _open_max_size = Vector2i(54, 60);

CuteMouth::CuteMouth(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor)
{
    // lv_arc rather than a container: a smile is a curve, and this is the only primitive
    // LVGL has that draws one. Its angle range is what carries the expression -- a wide
    // shallow arc is a grin, a narrow one is a polite smile.
    _smile = lv_arc_create(parent);
    lv_obj_set_size(_smile, _smile_size, _smile_size);
    lv_obj_align(_smile, LV_ALIGN_CENTER, _mouth_pos.x, _mouth_pos.y);
    lv_obj_remove_style(_smile, nullptr, LV_PART_KNOB);          // no drag handle
    lv_obj_remove_flag(_smile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(_smile, _smile_width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(_smile, _smile_width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(_smile, LV_OPA_TRANSP, LV_PART_MAIN);   // hide the track
    lv_obj_set_style_arc_color(_smile, primaryColor, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(_smile, true, LV_PART_INDICATOR);
    // 30..150 degrees, measured clockwise from 3 o'clock, is the lower arc: a smile.
    lv_arc_set_bg_angles(_smile, 30, 150);
    lv_arc_set_angles(_smile, 30, 150);

    _open = std::make_unique<Container>(parent);
    _open->setAlign(LV_ALIGN_CENTER);
    _open->setBorderWidth(0);
    _open->setBgColor(primaryColor);
    _open->setRadius(LV_RADIUS_CIRCLE);
    _open->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    setPosition(_position);
    setWeight(0);
    setRotation(0);
}

CuteMouth::~CuteMouth()
{
    _open.reset();
    if (_smile) {
        lv_obj_delete(_smile);
        _smile = nullptr;
    }
}

void CuteMouth::applyShape()
{
    // A HANDOFF, not a 50/50 blend. Fading both shapes linearly leaves them each at half
    // opacity across the middle of the range, which renders as two grey ghosts rather than
    // a mouth -- the shapes overlap, so the eye reads mud. Instead each owns a band and
    // they swap over a short overlap.
    //
    //   0..kSmileHold      smile only, fully opaque
    //   kSmileHold..kSwap  smile fades out, open mouth fades in
    //   kSwap..100         open mouth only, growing
    const int smile_opa = (_weight <= kSmileHold)
                              ? 255
                              : 255 - map_range(uitk::clamp(_weight, kSmileHold, kSwap),
                                                kSmileHold, kSwap, 0, 255);
    const int open_opa = (_weight <= kSmileHold)
                             ? 0
                             : map_range(uitk::clamp(_weight, kSmileHold, kSwap),
                                         kSmileHold, kSwap, 0, 255);

    lv_obj_set_style_arc_opa(_smile, (lv_opa_t)uitk::clamp(smile_opa, 0, 255), LV_PART_INDICATOR);

    // Height ramps linearly; width EASES IN. Linear on both axes meant mid-volume speech
    // was already near full width, so from there to fully-open the mouth only ever grew
    // taller -- the movement read as one-dimensional. Easing width keeps ordinary speech
    // narrow and saves the widening for the big openings, so the mouth visibly moves on
    // both axes across a sentence.
    //
    // t^1.6, in integer arithmetic: t*t gives the curve, then a partial blend back toward
    // linear so quiet speech does not collapse to a slit.
    const int t     = uitk::clamp(_weight, 0, 100);
    const int eased = (t * t) / 100;                 // t^2
    const int wcurve = (eased * 6 + t * 4) / 10;     // 60% quadratic, 40% linear ~= t^1.6

    const int w = map_range(wcurve, 0, 100, _open_min_size.x, _open_max_size.x);
    const int h = map_range(_weight, 0, 100, _open_min_size.y, _open_max_size.y);
    _open->setSize(w, h);
    _open->setBgOpa((lv_opa_t)uitk::clamp(open_opa, 0, 255));
}

void CuteMouth::setPosition(const Vector2i& position)
{
    Element::setPosition(position);

    const auto x = _mouth_pos.x + map_range(_position.x, -100, 100, _mouth_min_offset.x, _mouth_max_offset.x);
    const auto y = _mouth_pos.y + map_range(_position.y, -100, 100, _mouth_min_offset.y, _mouth_max_offset.y);

    lv_obj_align(_smile, LV_ALIGN_CENTER, x, y);
    _open->setPos(x, y);
}

void CuteMouth::setWeight(int weight)
{
    Feature::setWeight(weight);
    applyShape();
}

void CuteMouth::setRotation(int rotation)
{
    Element::setRotation(rotation);
}

void CuteMouth::setVisible(bool visible)
{
    Element::setVisible(visible);
    lv_obj_set_style_opa(_smile, visible ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    _open->setHidden(!visible);
}

void CuteMouth::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion()) {
        return;
    }
    _emotion = emotion;

    // The arc's angular span IS the expression. A frown is the same arc drawn on the upper
    // half; sadness and anger differ from happiness by where the curve opens, not by size.
    switch (emotion) {
        case Emotion::Happy:
            lv_arc_set_angles(_smile, 20, 160);      // wide grin
            break;
        case Emotion::Sad:
            lv_arc_set_angles(_smile, 210, 330);     // inverted: a frown
            break;
        case Emotion::Angry:
            lv_arc_set_angles(_smile, 220, 320);     // tighter frown
            break;
        case Emotion::Doubt:
            lv_arc_set_angles(_smile, 40, 110);      // short, off-centre: unsure
            break;
        case Emotion::Sleepy:
            lv_arc_set_angles(_smile, 45, 135);      // small but SYMMETRIC and shallow;
                                                     // a tight arc here reads as a smirk
            break;
        case Emotion::Neutral:
        default:
            lv_arc_set_angles(_smile, 30, 150);
            break;
    }
}
