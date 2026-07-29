/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "cute.h"

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

// The face "screen": a rounded-square outline the features live inside. Static -- drawn
// once, never dirtied -- so its size costs nothing per frame.
static const Vector2i _frame_size   = Vector2i(232, 196);
static const int _frame_radius      = 44;
static const int _frame_border      = 6;

// Small, soft, low and inboard of the eyes. The first attempt sat at eye level, wide and
// saturated, which read as war paint rather than a blush -- the reference's marks are
// barely there, and that restraint is what keeps the face sweet instead of clownish.
static const Vector2i _blush_size   = Vector2i(24, 16);
static const int _blush_stroke_len  = 13;
static const int _blush_stroke_w    = 4;
static const int _blush_gap         = 5;
static const Vector2i _blush_pos    = Vector2i(78, 30);

void CuteAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    _pannel = std::make_unique<Container>(parent);
    _pannel->align(LV_ALIGN_CENTER, 0, 0);
    _pannel->setSize(320, 240);
    _pannel->setRadius(0);
    _pannel->setBorderWidth(0);
    _pannel->setBgColor(secondaryColor);
    _pannel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    // Frame first so everything else stacks above it.
    _frame = std::make_unique<Container>(_pannel->get());
    _frame->setAlign(LV_ALIGN_CENTER);
    _frame->setPos(0, 0);
    _frame->setSize(_frame_size.x, _frame_size.y);
    _frame->setRadius(_frame_radius);
    _frame->setBgOpa(0);                       // interior stays the panel colour
    _frame->setBorderWidth(_frame_border);
    _frame->setBorderColor(primaryColor);
    _frame->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    // CRITICAL: LVGL objects are clickable by default, and this one covers the whole
    // face. Left clickable it swallows every tap before the panel's onClick() handler
    // sees it -- which is tap-to-talk, the device's only control. The default skin never
    // hit this because it had no full-size children.
    _frame->removeFlag(LV_OBJ_FLAG_CLICKABLE);
    // Cheap approximation of the reference's neon bloom: LVGL can shadow any object, and
    // a coloured shadow with no offset reads as glow. Static, so it is free per frame.
    lv_obj_set_style_shadow_width(_frame->get(), 18, 0);
    lv_obj_set_style_shadow_color(_frame->get(), primaryColor, 0);
    lv_obj_set_style_shadow_opa(_frame->get(), LV_OPA_30, 0);
    lv_obj_set_style_shadow_spread(_frame->get(), 0, 0);

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
        cheek->removeFlag(LV_OBJ_FLAG_CLICKABLE);   // see the frame comment above
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
