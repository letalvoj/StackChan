/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "chalk.h"
#include "../../../face_states.h"
#include <hal/hal.h>

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;
namespace art = stackchan::avatar::chalk;

/* -------------------------------------------------------------------------- */
/*                                   Sprite                                    */
/* -------------------------------------------------------------------------- */

Sprite::Sprite(lv_obj_t* parent)
{
    _image = std::make_unique<Image>(parent);
    _image->setAlign(LV_ALIGN_CENTER);
    // CRITICAL: LVGL objects are clickable by default, and anything laid over the face
    // swallows taps before the panel's onClick() sees them -- that is tap-to-talk, the
    // device's only control.
    _image->removeFlag(LV_OBJ_FLAG_CLICKABLE);
    _image->setImageRecolorOpa(LV_OPA_COVER);
    _image->setHidden(true);
}

void Sprite::setImage(const lv_image_dsc_t* dsc, int cx, int cy, uint32_t color)
{
    _has_image = (dsc != nullptr);
    if (!_has_image) {
        _image->setHidden(true);
        return;
    }
    _image->setSrc(dsc);
    _image->setImageRecolor(lv_color_hex(color));
    // Rotation acts about the pivot and every sprite is placed by its centre, so the
    // centre is the only pivot that keeps a transform in place.
    _image->setPivot(dsc->header.w / 2, dsc->header.h / 2);
    _cx = cx;
    _cy = cy;
    reposition();
    _image->setHidden(!_visible);
}

void Sprite::setOffset(int dx, int dy)
{
    _dx = dx;
    _dy = dy;
    reposition();
}

void Sprite::reposition()
{
    _image->setPos(_cx + _dx, _cy + _dy);
}

void Sprite::setVisible(bool visible)
{
    _visible = visible;
    _image->setHidden(!(visible && _has_image));
}

void Sprite::setRotation(int rotation)
{
    _image->setRotation(rotation);
}

/* -------------------------------------------------------------------------- */
/*                                  ClipTrack                                  */
/* -------------------------------------------------------------------------- */

ClipTrack::ClipTrack(lv_obj_t* parent, int layerCapacity)
{
    // Layers are created up front and reused. Creating LVGL objects mid-animation would
    // reorder the draw stack, and the cutout has to stay above the shape it cuts.
    for (int i = 0; i < layerCapacity; ++i) {
        _layers.push_back(std::make_unique<Sprite>(parent));
    }
}

void ClipTrack::show()
{
    for (size_t i = 0; i < _layers.size(); ++i) {
        const bool has = _clip && i < _clip->layerCount && _frame < _clip->frameCount;
        if (!has) {
            _layers[i]->setImage(nullptr, 0, 0, 0);
            continue;
        }
        const art::ClipLayer& layer = _clip->layers[i];
        const art::ClipFrame& f     = layer.frames[_frame];
        _layers[i]->setImage(f.dsc, f.cx, f.cy, layer.color);
        _layers[i]->setVisible(_visible);
    }
}

void ClipTrack::hold(const art::Clip* clip, uint8_t frame)
{
    _clip     = clip;
    _frame    = (clip && frame < clip->frameCount) ? frame : 0;
    _mode     = Mode::Hold;
    _finished = true;
    show();
}

void ClipTrack::play(const art::Clip* clip, uint32_t now, Mode mode)
{
    _clip = clip;
    if (!clip || clip->frameCount == 0) {
        _finished = true;
        show();
        return;
    }
    _frame      = 0;
    _mode       = mode;
    _finished   = false;
    _next_tick  = now + clip->holdMs[0];
    show();
}

bool ClipTrack::advance(uint32_t now)
{
    if (!_clip || _mode == Mode::Hold || _finished || now < _next_tick) {
        return false;
    }
    const uint8_t next = _frame + 1;
    if (next >= _clip->frameCount) {
        if (_mode == Mode::Once) {
            _finished = true;
            return false;
        }
        _frame = 0;
    } else {
        _frame = next;
    }
    // Due times are measured from when this frame was *due*, not from the tick that noticed.
    // Counting from `now` added each update's lateness to every hold after it, so a nine-beat
    // laugh at a 33 ms update ran up to ~300 ms long and its bursts lost their shape. If the
    // track has fallen behind by a whole hold -- a stalled loop -- it shows this frame for
    // its full hold rather than racing through the ones it missed.
    const uint32_t planned = _next_tick + _clip->holdMs[_frame];
    _next_tick             = (planned > now) ? planned : now + _clip->holdMs[_frame];
    show();
    return true;
}

void ClipTrack::setOffset(int dx, int dy)
{
    for (auto& l : _layers) {
        l->setOffset(dx, dy);
    }
}

void ClipTrack::setVisible(bool visible)
{
    _visible = visible;
    for (auto& l : _layers) {
        l->setVisible(visible);
    }
}

void ClipTrack::setRotation(int rotation)
{
    for (auto& l : _layers) {
        l->setRotation(rotation);
    }
}

/* -------------------------------------------------------------------------- */
/*                                 ChalkAvatar                                 */
/* -------------------------------------------------------------------------- */

void ChalkAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    _pannel = std::make_unique<Container>(parent);
    _pannel->align(LV_ALIGN_CENTER, 0, 0);
    _pannel->setSize(face::kScreenWidth, face::kScreenHeight);
    _pannel->setRadius(0);
    _pannel->setBorderWidth(0);
    _pannel->setBgColor(lv_color_hex(kInk));
    _pannel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* p = _pannel->get();

    // Brows and cheeks sit behind the eyes, so a brow that dips toward the eye socket
    // reads as being behind it, which is how the reference is drawn.
    _brow_l  = std::make_unique<ClipTrack>(p, 1);
    _brow_r  = std::make_unique<ClipTrack>(p, 1);
    _cheek_l = std::make_unique<ClipTrack>(p, 1);
    _cheek_r = std::make_unique<ClipTrack>(p, 1);

    auto leftEye  = std::make_unique<ChalkEyes>(p, true);
    auto rightEye = std::make_unique<ChalkEyes>(p, false);
    auto mouth    = std::make_unique<ChalkMouth>(p);
    _eye_l        = leftEye.get();
    _eye_r        = rightEye.get();
    _mouth        = mouth.get();
    _key_elements.leftEye  = std::move(leftEye);
    _key_elements.rightEye = std::move(rightEye);
    _key_elements.mouth    = std::move(mouth);

    // Reuse the default bubble: it is a text container, orthogonal to the face's look, and
    // duplicating it would mean maintaining two.
    _key_elements.speechBubble =
        std::make_unique<DefaultSpeechBubble>(p, lv_color_hex(kChalk), lv_color_hex(kInk), font);

    setEmotion(Emotion::Neutral);
}

void ChalkAvatar::setEmotion(const Emotion& emotion)
{
    // Brows and cheeks belong to the avatar, so they are swapped here; the base call then
    // forwards the emotion to the eyes, mouth, bubble and any live decorators.
    const art::EmotionClips& clips = art::clipsFor(emotion);

    _brow_l->hold(clips.browsLeft, clips.browsRestFrame);
    _brow_r->hold(clips.browsRight, clips.browsRestFrame);

    Avatar::setEmotion(emotion);
    // The mouth has just taken its new resting frame. Follow it now rather than at the next
    // update, or the old emotion's lead -- a laugh's > < eyes -- survives into the frame
    // that draws the new one.
    follow(_mouth ? _mouth->lead() : nullptr);
    applyCheeks();
}

void ChalkAvatar::follow(const art::ClipCompanion* lead)
{
    if (lead == _lead || !_eye_l || !_eye_r || !_mouth) {
        return;
    }
    _lead = lead;

    _eye_l->setLead(lead);
    _eye_r->setLead(lead);

    // The artist lifts and drops the whole face a pixel or three with each burst. Every part
    // takes the same offset, so the face moves as one thing instead of coming apart.
    const int dy = lead ? lead->faceY : 0;
    _eye_l->setFaceY(dy);
    _eye_r->setFaceY(dy);
    _mouth->setFaceY(dy);
    _brow_l->setOffset(0, dy);
    _brow_r->setOffset(0, dy);
    _cheek_l->setOffset(0, dy);
    _cheek_r->setOffset(0, dy);

    applyCheeks();
}

void ChalkAvatar::setBlush(bool blushing)
{
    if (_blushing == blushing) {
        return;
    }
    _blushing = blushing;
    applyCheeks();
}

void ChalkAvatar::applyCheeks()
{
    // Blushing swaps the family the cheek track is drawing rather than adding a second set
    // of marks over the first. The reference's petted face has pink cheeks, not pink
    // cheeks on top of chalk ones.
    if (_blushing) {
        // Frame 3 is the settled blush, the one the row holds longest.
        _cheek_l->hold(&art::clip_cheeks_warm_blush_left, 3);
        _cheek_r->hold(&art::clip_cheeks_warm_blush_right, 3);
        return;
    }
    // A laughing mouth chooses the cheeks too: they lift furthest on the widest "ha".
    if (_lead) {
        _cheek_l->hold(_lead->cheeksLeft, _lead->cheeksFrame);
        _cheek_r->hold(_lead->cheeksRight, _lead->cheeksFrame);
        return;
    }
    const art::EmotionClips& clips = art::clipsFor(_emotion);
    _cheek_l->hold(clips.cheeksLeft, clips.cheeksRestFrame);
    _cheek_r->hold(clips.cheeksRight, clips.cheeksRestFrame);
}

void ChalkAvatar::update()
{
    // Brows and cheeks run on the shared clock like everything else, so a family played
    // rather than held animates without anyone driving it frame by frame.
    const uint32_t now = GetHAL().millis();
    _brow_l->advance(now);
    _brow_r->advance(now);
    _cheek_l->advance(now);
    _cheek_r->advance(now);

    Avatar::update();

    // After the features have advanced, so the rest of the face follows the mouth frame that
    // is actually showing, not the one before it.
    follow(_mouth ? _mouth->lead() : nullptr);
}

FaceAnchors ChalkAvatar::anchors() const
{
    FaceAnchors a;
    a.leftEye    = {art::kAnchor_eye_left.x, art::kAnchor_eye_left.y};
    a.rightEye   = {art::kAnchor_eye_right.x, art::kAnchor_eye_right.y};
    a.leftCheek  = {art::kAnchor_cheek_left.x, art::kAnchor_cheek_left.y};
    a.rightCheek = {art::kAnchor_cheek_right.x, art::kAnchor_cheek_right.y};
    a.mouth      = {art::kAnchor_mouth.x, art::kAnchor_mouth.y};
    a.eyeRadius  = art::kEyeRadius;
    // The reference draws the chalk cheek marks *and* the pink blush together on the shy
    // and headpet faces -- the blush is wider and sits over them on purpose.
    a.skinDrawsBlush = true;
    return a;
}

OverlayArt ChalkAvatar::overlayArt(OverlayKind kind) const
{
    OverlayArt o;
    o.valid = true;

    // Accents are drawn families now, so an overlay hands back the first frame and the
    // decorator animates by walking the rest. The decorator asks for one image, which is
    // the interface it has always had.
    auto first = [&o](const art::Clip& clip, int cx, int cy) {
        const art::ClipFrame& f = clip.layers[0].frames[0];
        o.left    = f.dsc;
        o.leftPos = {f.cx + cx, f.cy + cy};
        o.color   = clip.layers[0].color;
    };

    switch (kind) {
        case OverlayKind::Heart:
            first(art::clip_accents_heart_pulse, 0, 0);
            break;
        case OverlayKind::Shy:
            // Blush is a cheek family in this pack, played by the cheek track, so there is
            // no overlay to hand out.
            o.valid = false;
            break;
        case OverlayKind::AngryMark:
            first(art::clip_accents_anger_twitch, 0, 0);
            break;
        case OverlayKind::Sweat:
            first(art::clip_accents_sweat_slip, 0, 0);
            break;
        case OverlayKind::DizzyEye: {
            // One drawn spiral, placed at each eye anchor, standing in for the shells.
            const art::ClipFrame& f = art::clip_accents_dizzy_spin.layers[0].frames[0];
            o.left     = f.dsc;
            o.right    = f.dsc;
            o.leftPos  = {art::kAnchor_eye_left.x, art::kAnchor_eye_left.y};
            o.rightPos = {art::kAnchor_eye_right.x, art::kAnchor_eye_right.y};
            o.color    = art::clip_accents_dizzy_spin.layers[0].color;
            break;
        }
        default:
            o.valid = false;
            break;
    }
    return o;
}
