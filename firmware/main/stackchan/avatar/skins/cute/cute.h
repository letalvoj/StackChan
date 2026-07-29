/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../../avatar/avatar.h"
#include "../../avatar/elements/feature.h"
#include "../default/default.h"
#include <lvgl.h>
#include <smooth_lvgl.hpp>
#include <memory>

namespace stackchan::avatar {

/**
 * @brief "Cute" skin: glowing rounded-square face, big round eyes, curved smile, blush.
 *
 * Implements the same Feature contract as DefaultAvatar, so every existing modifier
 * (blink, breath, speaking, head-pet, IMU) drives it unchanged -- none of them know or
 * care what a weight of 0 looks like.
 *
 * Two things differ structurally from the default skin, both chosen to stay inside the
 * partial-redraw budget (see AVATAR.md 6):
 *
 *   * The face frame and blush are STATIC. They are drawn once and never marked dirty,
 *     so they cost nothing per frame no matter how elaborate they look.
 *   * The mouth cross-fades between a smile arc and an open ellipse rather than morphing
 *     one shape. LVGL cannot animate a path, but it can fade two small overlapping
 *     objects, and only their bounding box is redrawn.
 */
class CuteAvatar : public Avatar {
public:
    lv_color_t primaryColor   = lv_color_white();
    lv_color_t secondaryColor = lv_color_hex(0x0A0F1E);   // deep navy, not pure black
    lv_color_t blushColor     = lv_color_hex(0xFF8A96);

    void init(lv_obj_t* parent, const lv_font_t* font = &lv_font_montserrat_16);
    uitk::lvgl_cpp::Container* getPanel() const;

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _pannel;
    std::unique_ptr<uitk::lvgl_cpp::Container> _frame;
    std::unique_ptr<uitk::lvgl_cpp::Container> _blush_l;
    std::unique_ptr<uitk::lvgl_cpp::Container> _blush_r;
};

/**
 * @brief Big round eye with a dark pupil.
 *
 * Three layers: a white disc, a small dark pupil that sits slightly low and inward (which
 * is most of what reads as "cute" rather than "staring"), and an eyelid that slides down
 * to blink -- the same mechanism the default skin uses, because it is genuinely the right
 * one: closing an eye is a rectangle moving, not a redraw.
 */
class CuteEyes : public Feature {
public:
    CuteEyes(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor, bool isLeftEye);
    ~CuteEyes();

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setRotation(int rotation) override;
    void setEmotion(const Emotion& emotion) override;
    void setVisible(bool visible) override;
    void setSize(int size) override;

private:
    bool _is_left_eye    = false;
    int _eyelid_offset_y = 0;

    std::unique_ptr<uitk::lvgl_cpp::Container> _container;
    std::unique_ptr<uitk::lvgl_cpp::Container> _eye;
    std::unique_ptr<uitk::lvgl_cpp::Container> _pupil;
    std::unique_ptr<uitk::lvgl_cpp::Container> _eyelid;
};

/**
 * @brief Smile at rest, open mouth when speaking.
 *
 * weight 0    -> a curved smile (an arc, drawn with LVGL's arc widget)
 * weight 100  -> an open rounded ellipse
 * in between  -> the two cross-fade, and the ellipse grows
 *
 * A single rectangle cannot be a smile, and that is the main thing that made the default
 * face read as a machine rather than a character.
 */
class CuteMouth : public Feature {
public:
    CuteMouth(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor);
    ~CuteMouth();

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setRotation(int rotation) override;
    void setVisible(bool visible) override;
    void setEmotion(const Emotion& emotion) override;

private:
    void applyShape();

    lv_obj_t* _smile = nullptr;                        // lv_arc: the resting smile
    std::unique_ptr<uitk::lvgl_cpp::Container> _open;  // the talking mouth
    Emotion _emotion = Emotion::Neutral;
};

}  // namespace stackchan::avatar
