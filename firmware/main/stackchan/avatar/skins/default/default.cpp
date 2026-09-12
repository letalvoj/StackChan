/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "default.h"
#include "../../../face_states.h"

using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

void DefaultAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    _pannel = std::make_unique<Container>(parent);
    _pannel->align(LV_ALIGN_CENTER, 0, 0);
    _pannel->setSize(face::kScreenWidth, face::kScreenHeight);
    _pannel->setRadius(0);
    _pannel->setBorderWidth(0);
    _pannel->setBgColor(secondaryColor);
    _pannel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _key_elements.leftEye  = std::make_unique<DefaultEyes>(_pannel->get(), primaryColor, secondaryColor, true);
    _key_elements.rightEye = std::make_unique<DefaultEyes>(_pannel->get(), primaryColor, secondaryColor, false);
    _key_elements.mouth    = std::make_unique<DefaultMouth>(_pannel->get(), primaryColor, secondaryColor);
    _key_elements.speechBubble =
        std::make_unique<DefaultSpeechBubble>(_pannel->get(), primaryColor, secondaryColor, font);
}

Container* DefaultAvatar::getPanel() const
{
    if (_pannel) {
        return _pannel.get();
    }
    return NULL;
}

/* ------------------------------- Skin geometry ------------------------------- */

LV_IMAGE_DECLARE(decorator_heart);
LV_IMAGE_DECLARE(decorator_angry);
LV_IMAGE_DECLARE(decorator_sweat);
LV_IMAGE_DECLARE(decorator_shy);
LV_IMAGE_DECLARE(decorator_dizzy);

FaceAnchors DefaultAvatar::anchors() const
{
    FaceAnchors a;
    a.leftEye        = {-70, -16};
    a.rightEye       = {70, -16};
    a.leftCheek      = {-108, 28};
    a.rightCheek     = {108, 28};
    a.mouth          = {0, 48};
    a.eyeRadius      = 16;
    a.skinDrawsBlush = false;
    return a;
}

OverlayArt DefaultAvatar::overlayArt(OverlayKind kind) const
{
    OverlayArt o;
    o.valid = true;

    switch (kind) {
        case OverlayKind::Heart:
            o.left    = &decorator_heart;
            o.leftPos = {108, -70};
            o.color   = 0xE13232;
            break;

        case OverlayKind::AngryMark:
            o.left    = &decorator_angry;
            o.leftPos = {108, -70};
            o.color   = 0xE13232;
            break;

        case OverlayKind::Sweat:
            o.left    = &decorator_sweat;
            o.leftPos = {-116, -72};
            o.color   = 0x53D2E8;
            break;

        case OverlayKind::Shy:
            o.left     = &decorator_shy;
            o.right    = &decorator_shy;
            o.leftPos  = {-108, 28};
            o.rightPos = {108, 28};
            o.color    = 0xFF6464;
            break;

        case OverlayKind::DizzyEye:
            // Anchored to this skin's own eye slots rather than to a copy of them.
            o.left     = &decorator_dizzy;
            o.right    = &decorator_dizzy;
            o.leftPos  = anchors().leftEye;
            o.rightPos = anchors().rightEye;
            o.color    = 0xFFFFFF;
            break;

        default:
            // This skin has no wavy mouth or balance marks; the dizzy decorator draws the
            // spirals alone here, exactly as it always did.
            o.valid = false;
            break;
    }
    return o;
}
