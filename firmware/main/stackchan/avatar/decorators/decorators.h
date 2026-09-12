/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../avatar/decorator.h"
#include "../avatar/avatar.h"
#include <lvgl.h>
#include <smooth_lvgl.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace stackchan::avatar {

/**
 * @brief An overlay drawn on top of the face: hearts, blush, spirals, a bead of sweat.
 *
 * These used to be five classes that differed only in which image they loaded and where
 * they put it, and the "where" was a file-scope constant copied from the original skin's
 * layout. That made every overlay wrong on any other face -- the dizzy spirals kept
 * landing on the first skin's eye slots long after the eyes had moved and grown.
 *
 * Now a decorator contributes only the animation. Which sprite to draw and where to put it
 * come from the skin, via Avatar::overlayArt, so an overlay follows the face it is drawn on.
 */
class SpriteDecorator : public Decorator {
public:
    /// What the overlay does over time. The face it decorates does not vary this.
    enum class Motion {
        None,     ///< sits still
        Wobble,   ///< rocks between two angles, a heartbeat
        Drip,     ///< slides downward and vanishes, a bead running down
        Spin,     ///< rotates continuously, the dizzy spiral
    };

    /**
     * @param parent LVGL parent for the overlay images
     * @param destroyAfterMs Destroy after milliseconds, 0 for infinite
     * @param animationIntervalMs Animation update interval in milliseconds, 0 for none
     * @param kinds Overlays to draw. The first is animated; the rest are static garnish
     *              that belongs to the same overlay, such as the heart's sparkles.
     * @param motion How the first overlay animates
     */
    SpriteDecorator(lv_obj_t* parent, uint32_t destroyAfterMs, uint32_t animationIntervalMs,
                    std::vector<OverlayKind> kinds, Motion motion);
    ~SpriteDecorator() override;

    void onAttach(Avatar& avatar) override;
    void _update() override;

    using Element::setPosition;

    /// Nudge every layer away from the slot the skin gave it.
    void setPosition(int x, int y);
    void setRotation(int rotation) override;
    void setColor(lv_color_t color);
    void setVisible(bool visible) override;

private:
    struct Layer {
        std::unique_ptr<uitk::lvgl_cpp::Image> image;
        uitk::Vector2i home;
        bool animated = false;
    };

    void addLayer(const lv_image_dsc_t* src, const uitk::Vector2i& pos, uint32_t color, bool animated);
    void applyMotion();

    lv_obj_t* _parent = nullptr;
    std::vector<OverlayKind> _kinds;
    std::vector<Layer> _layers;
    Motion _motion;

    uint32_t _destroy_at            = 0;
    uint32_t _next_animation_tick   = 0;
    uint32_t _animation_interval_ms = 0;
    bool _has_lifetime              = false;

    int _animation_index = 0;
};

/// Affection: a heart with a couple of sparkles, beating.
class HeartDecorator : public SpriteDecorator {
public:
    HeartDecorator(lv_obj_t* parent, uint32_t destroyAfterMs = 0, uint32_t animationIntervalMs = 500)
        : SpriteDecorator(parent, destroyAfterMs, animationIntervalMs,
                          {OverlayKind::Heart, OverlayKind::HeartSparkle}, Motion::Wobble)
    {
    }
};

/// Irritation: the four-stroke anger mark.
class AngryDecorator : public SpriteDecorator {
public:
    AngryDecorator(lv_obj_t* parent, uint32_t destroyAfterMs = 0, uint32_t animationIntervalMs = 500)
        : SpriteDecorator(parent, destroyAfterMs, animationIntervalMs, {OverlayKind::AngryMark}, Motion::Wobble)
    {
    }
};

/// Discomfort: a bead of sweat that runs down and disappears.
class SweatDecorator : public SpriteDecorator {
public:
    SweatDecorator(lv_obj_t* parent, uint32_t destroyAfterMs = 0, uint32_t animationIntervalMs = 700)
        : SpriteDecorator(parent, destroyAfterMs, animationIntervalMs, {OverlayKind::Sweat}, Motion::Drip)
    {
    }
};

/// Bashfulness: blush across both cheeks.
class ShyDecorator : public SpriteDecorator {
public:
    ShyDecorator(lv_obj_t* parent, uint32_t destroyAfterMs = 0)
        : SpriteDecorator(parent, destroyAfterMs, 0, {OverlayKind::Shy}, Motion::None)
    {
    }
};

/**
 * @brief Disorientation: spiral eyes, a wavy mouth and a pair of balance marks.
 *
 * The spirals stand in for the eyes rather than sitting over them, which is why the shake
 * handler hides the skin's eyes first. They anchor to the skin's eye slots, so they land
 * on the eyes of whichever face is being worn.
 */
class DizzyDecorator : public SpriteDecorator {
public:
    DizzyDecorator(lv_obj_t* parent, uint32_t destroyAfterMs = 0, uint32_t animationIntervalMs = 500)
        : SpriteDecorator(parent, destroyAfterMs, animationIntervalMs,
                          {OverlayKind::DizzyEye, OverlayKind::DizzyMouth, OverlayKind::DizzyWobble},
                          Motion::Spin)
    {
    }
};

}  // namespace stackchan::avatar
