/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "decorators.h"
#include <hal/hal.h>

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

// A heartbeat is two angles, not a sweep: alternating reads as a pulse where a smooth
// rotation reads as the sprite being dragged around.
// Relative to however the artwork is already angled: the first frame is the sprite's
// own rest angle, so a pre-tilted heart is not tilted a second time.
static const int kWobbleFrames[] = {0, 60};

// How far a bead of sweat has slid, in pixels, at each step of its run. The last entry
// ends the animation; the bead does not fade, it is simply gone.
static const int kDripOffsets[] = {0, 4, 10, 14};

// 30 degrees per tick, which at the shake handler's interval is a brisk but readable spin.
static const int kSpinStep = 300;

// The second spiral runs a quarter turn behind the first, so the two eyes do not lock into
// looking like one rigid object.
static const int kSpinPhase = 450;

SpriteDecorator::SpriteDecorator(lv_obj_t* parent, uint32_t destroyAfterMs, uint32_t animationIntervalMs,
                                 std::vector<OverlayKind> kinds, Motion motion)
    : _parent(parent), _kinds(std::move(kinds)), _motion(motion), _animation_interval_ms(animationIntervalMs)
{
    uint32_t now = GetHAL().millis();

    if (destroyAfterMs > 0) {
        _destroy_at   = now + destroyAfterMs;
        _has_lifetime = true;
    }
    if (_animation_interval_ms > 0) {
        _next_animation_tick = now + _animation_interval_ms;
    }
}

SpriteDecorator::~SpriteDecorator()
{
}

void SpriteDecorator::addLayer(const lv_image_dsc_t* src, const Vector2i& pos, uint32_t color, bool animated)
{
    if (!src) {
        return;
    }
    Layer layer;
    layer.image = std::make_unique<Image>(_parent);
    layer.image->setSrc(src);
    layer.image->setAlign(LV_ALIGN_CENTER);
    layer.image->setPos(pos.x, pos.y);
    layer.image->setPivot(src->header.w / 2, src->header.h / 2);
    // CRITICAL: LVGL objects are clickable by default, and an overlay across the face
    // swallows taps before the panel sees them -- that is tap-to-talk.
    layer.image->removeFlag(LV_OBJ_FLAG_CLICKABLE);
    layer.image->setImageRecolorOpa(LV_OPA_COVER);
    layer.image->setImageRecolor(lv_color_hex(color));
    layer.home     = pos;
    layer.animated = animated;
    _layers.push_back(std::move(layer));
}

void SpriteDecorator::onAttach(Avatar& avatar)
{
    // Only the first kind animates; the rest are garnish belonging to the same overlay.
    bool first = true;
    for (OverlayKind kind : _kinds) {
        const OverlayArt art = avatar.overlayArt(kind);
        if (!art.valid) {
            // The skin draws nothing for this kind. Silence is the right answer -- falling
            // back to another skin's artwork is how the old placement bugs looked.
            first = false;
            continue;
        }
        addLayer(art.left, art.leftPos, art.color, first);
        addLayer(art.right, art.rightPos, art.color, first);
        first = false;
    }
    applyMotion();
}

void SpriteDecorator::applyMotion()
{
    int index_in_pair = 0;
    for (Layer& layer : _layers) {
        if (!layer.animated) {
            continue;
        }
        switch (_motion) {
            case Motion::Wobble:
                layer.image->setRotation(kWobbleFrames[_animation_index % 2]);
                break;

            case Motion::Spin: {
                const int base = -(_animation_index * kSpinStep) % 3600;
                layer.image->setRotation((base + index_in_pair * kSpinPhase + 3600) % 3600);
                break;
            }

            case Motion::Drip: {
                const int steps = (int)(sizeof(kDripOffsets) / sizeof(kDripOffsets[0]));
                if (_animation_index >= steps) {
                    layer.image->setHidden(true);
                } else {
                    layer.image->setPos(layer.home.x, layer.home.y + kDripOffsets[_animation_index]);
                }
                break;
            }

            case Motion::None:
            default:
                break;
        }
        ++index_in_pair;
    }
}

void SpriteDecorator::_update()
{
    uint32_t now = GetHAL().millis();

    if (_has_lifetime && now >= _destroy_at) {
        requestDestroy();
        return;
    }

    if (_animation_interval_ms > 0 && now >= _next_animation_tick) {
        _next_animation_tick = now + _animation_interval_ms;
        ++_animation_index;

        // A bead of sweat runs once and is done; everything else loops.
        if (_motion == Motion::Drip &&
            _animation_index >= (int)(sizeof(kDripOffsets) / sizeof(kDripOffsets[0]))) {
            requestDestroy();
            return;
        }
        applyMotion();
    }
}

void SpriteDecorator::setPosition(int x, int y)
{
    for (Layer& layer : _layers) {
        layer.image->setPos(layer.home.x + x, layer.home.y + y);
    }
}

void SpriteDecorator::setRotation(int rotation)
{
    Element::setRotation(rotation);
    for (Layer& layer : _layers) {
        layer.image->setRotation(_rotation);
    }
}

void SpriteDecorator::setColor(lv_color_t color)
{
    for (Layer& layer : _layers) {
        layer.image->setImageRecolor(color);
    }
}

void SpriteDecorator::setVisible(bool visible)
{
    Element::setVisible(visible);
    for (Layer& layer : _layers) {
        layer.image->setHidden(!visible);
    }
}
