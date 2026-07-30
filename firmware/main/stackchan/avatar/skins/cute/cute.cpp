/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "cute.h"

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

// There used to be a rounded-square outline around the face here -- a "screen" the
// features sat inside. It is gone: on an actual screen it reads as a bezel drawn on a
// bezel, and it is the one part of this face people ask to have removed.
//
// Removing it freed a ring about 12 px wide all the way round, so every layout constant
// in this skin (here, eyes.cpp and mouth.cpp) is 1.2x what it was. Scaling the positions
// as well as the sizes is what keeps it the same face, larger, rather than larger parts
// in the old arrangement.

// Small, soft, low and inboard of the eyes. The first attempt sat at eye level, wide and
// saturated, which read as war paint rather than a blush -- the reference's marks are
// barely there, and that restraint is what keeps the face sweet instead of clownish.
static const Vector2i _blush_size   = Vector2i(29, 19);
static const int _blush_stroke_len  = 16;
static const int _blush_stroke_w    = 5;
static const int _blush_gap         = 6;
static const Vector2i _blush_pos    = Vector2i(94, 36);

void CuteAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    _pannel = std::make_unique<Container>(parent);
    _pannel->align(LV_ALIGN_CENTER, 0, 0);
    _pannel->setSize(320, 240);
    _pannel->setRadius(0);
    _pannel->setBorderWidth(0);
    _pannel->setBgColor(secondaryColor);
    _pannel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    // Two slanted strokes per cheek, not one oval. The reference's blush is a pair of
    // little diagonal dashes, and that detail is most of what separates "blushing" from
    // "wearing rouge" -- a single horizontal blob reads as makeup.
    //
    // Each cheek is an invisible container holding two rotated bars, so the pair moves and
    // is styled as one thing.
    auto make_blush = [&](bool left) {
        auto cheek = std::make_unique<Container>(_pannel->get());
        cheek->setAlign(LV_ALIGN_CENTER);
        cheek->setPos(left ? -_blush_pos.x : _blush_pos.x, _blush_pos.y);
        cheek->setSize(_blush_size.x, _blush_size.y);
        cheek->setBgOpa(0);
        cheek->setBorderWidth(0);
        cheek->setRadius(0);
        cheek->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
        // CRITICAL: LVGL objects are clickable by default. Anything laid over the face
        // swallows taps before the panel's onClick() sees them -- and that is
        // tap-to-talk, the device's only control.
        cheek->removeFlag(LV_OBJ_FLAG_CLICKABLE);
        cheek->setPadding(0, 0, 0, 0);

        for (int i = 0; i < 2; ++i) {
            lv_obj_t* bar = lv_obj_create(cheek->get());
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, _blush_stroke_len, _blush_stroke_w);
            lv_obj_align(bar, LV_ALIGN_CENTER, (i == 0 ? -_blush_gap : _blush_gap), 0);
            lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(bar, blushColor, 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
            lv_obj_set_style_transform_pivot_x(bar, _blush_stroke_len / 2, 0);
            lv_obj_set_style_transform_pivot_y(bar, _blush_stroke_w / 2, 0);
            // Slant inward-up, mirrored per side, like a little pair of speed lines.
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_transform_rotation(bar, left ? -600 : 600, 0);
        }
        return cheek;
    };
    _blush_l = make_blush(true);
    _blush_r = make_blush(false);

    _key_elements.leftEye  = std::make_unique<CuteEyes>(_pannel->get(), primaryColor, secondaryColor, true);
    _key_elements.rightEye = std::make_unique<CuteEyes>(_pannel->get(), primaryColor, secondaryColor, false);
    _key_elements.mouth    = std::make_unique<CuteMouth>(_pannel->get(), primaryColor, secondaryColor);
    // Reuse the default bubble: it is a text container, orthogonal to the face's look, and
    // duplicating it would mean maintaining two.
    _key_elements.speechBubble =
        std::make_unique<DefaultSpeechBubble>(_pannel->get(), primaryColor, secondaryColor, font);
}

Container* CuteAvatar::getPanel() const
{
    return _pannel ? _pannel.get() : nullptr;
}
