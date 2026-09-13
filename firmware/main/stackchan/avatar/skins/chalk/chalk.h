/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../../avatar/avatar.h"
#include "../default/default.h"
#include "chalk_clips.h"
#include "chalk_palette.h"
#include <smooth_ui_toolkit.hpp>
#include <lvgl.h>
#include <memory>
#include <vector>

namespace stackchan::avatar {

/**
 * @brief "Chalk" skin: the artist's hand-drawn face and its expression pack.
 *
 * The artwork is stroked paths in the pack's own local space with the artist's own
 * anchors. LVGL is built here without vector graphics, so every drawn frame is baked to an
 * alpha-only sprite by tools/facegen/gen_clips.py and tinted at draw time. Everything in
 * chalk_clips.h is generated; nothing in this file retypes a coordinate or a duration.
 *
 * The face is made of independent tracks -- eyes, pupils, brows, cheeks, mouth -- each
 * playing its own drawn family on its own clock. That independence is the point of the
 * pack: an emotion chooses which family each track plays, and a blink or a spoken syllable
 * then runs without disturbing anything else.
 */

/// One baked sprite as an LVGL image, positioned and tinted.
class Sprite {
public:
    Sprite(lv_obj_t* parent);

    /// Show an image at an offset from the panel centre. A null image hides the sprite,
    /// which is how a colour layer with nothing on this frame behaves.
    void setImage(const lv_image_dsc_t* dsc, int cx, int cy, uint32_t color);
    void setOffset(int dx, int dy);
    void setVisible(bool visible);
    void setRotation(int rotation);

private:
    void reposition();

    std::unique_ptr<uitk::lvgl_cpp::Image> _image;
    int _cx = 0, _cy = 0;
    int _dx = 0, _dy = 0;
    bool _has_image = false;
    bool _visible   = true;
};

/**
 * @brief Plays one drawn family, stacking a sprite per colour layer.
 *
 * A talking mouth is a chalk shape with a black cutout inside it, and A8 carries no
 * colour, so each colour is its own sprite and this keeps them in step.
 */
class ClipTrack {
public:
    enum class Mode {
        Hold,   ///< park on one frame
        Once,   ///< run to the end and stay there
        Loop,   ///< run forever
    };

    ClipTrack(lv_obj_t* parent, int layerCapacity = 2);

    /// Park on one frame. The resting case, and by far the common one.
    void hold(const chalk::Clip* clip, uint8_t frame);
    /// Start a clip running from its first frame.
    void play(const chalk::Clip* clip, uint32_t now, Mode mode);
    /// Advance if this frame's hold has elapsed. True when the frame changed.
    bool advance(uint32_t now);

    void setOffset(int dx, int dy);
    void setVisible(bool visible);
    void setRotation(int rotation);

    bool finished() const
    {
        return _finished;
    }
    uint8_t frame() const
    {
        return _frame;
    }
    const chalk::Clip* clip() const
    {
        return _clip;
    }

private:
    void show();

    std::vector<std::unique_ptr<Sprite>> _layers;
    const chalk::Clip* _clip = nullptr;
    uint8_t _frame           = 0;
    uint32_t _next_tick      = 0;
    Mode _mode               = Mode::Hold;
    bool _finished           = true;
    bool _visible            = true;
};

/**
 * @brief One eye: a drawn shell family, plus a pupil that moves inside it.
 */
class ChalkEyes : public Feature {
public:
    ChalkEyes(lv_obj_t* parent, bool isLeftEye);

    void setEmotion(const Emotion& emotion) override;
    void setWeight(int weight) override;
    void setPosition(const uitk::Vector2i& position) override;
    void setGaze(const uitk::Vector2i& gaze) override;
    void setRotation(int rotation) override;
    void setVisible(bool visible) override;
    void _update() override;
    Emotion getEmotion() const override
    {
        return _emotion;
    }

    /// Let a mouth frame choose this eye's drawing, or null to go back to the emotion's own.
    void setLead(const chalk::ClipCompanion* lead);
    /// Whole-face vertical lift, in panel pixels.
    void setFaceY(int dy);

private:
    void refresh();
    void place();

    bool _is_left_eye = true;
    Emotion _emotion  = Emotion::Neutral;
    const chalk::ClipCompanion* _lead = nullptr;
    int _face_y                       = 0;

    std::unique_ptr<ClipTrack> _shell;
    std::unique_ptr<Sprite> _pupil;
};

/**
 * @brief The mouth: a resting family per emotion, opening through that emotion's own
 *        speech poses rather than scaling one shape.
 */
class ChalkMouth : public Feature {
public:
    ChalkMouth(lv_obj_t* parent);

    void setEmotion(const Emotion& emotion) override;
    void setWeight(int weight) override;
    void setPosition(const uitk::Vector2i& position) override;
    void setRotation(int rotation) override;
    void setVisible(bool visible) override;
    void _update() override;
    Emotion getEmotion() const override
    {
        return _emotion;
    }

    /// What the rest of the face should do for the mouth frame showing now, or null when
    /// this mouth leads nothing -- which is every mouth except a laugh.
    const chalk::ClipCompanion* lead() const;
    /// Whole-face vertical lift, in panel pixels.
    void setFaceY(int dy);

private:
    void refresh();
    void place();

    Emotion _emotion = Emotion::Neutral;
    std::unique_ptr<ClipTrack> _track;
    int _face_y = 0;
    /// Set when an emotion wants its idle family played at once rather than after a gap.
    bool _idle_on_enter = false;

    /// A resting mouth is not a still mouth. The artist marks the quiet families
    /// "occasional, not continuous", so one is played through now and then and the mouth
    /// settles back afterwards.
    uint32_t _next_flicker_ms = 0;
    bool _flickering          = false;

    void scheduleFlicker(uint32_t now);
};

/**
 * @brief The face: panel, brows, cheeks, and the skin's geometry declarations.
 *
 * Brows and cheeks are not Features -- nothing at runtime addresses them individually --
 * so the avatar owns them and swaps their family with the emotion.
 */
class ChalkAvatar : public Avatar {
public:
    void init(lv_obj_t* parent, const lv_font_t* font = &lv_font_montserrat_16);

    void setEmotion(const Emotion& emotion) override;
    void setBlush(bool blushing) override;
    void update() override;
    FaceAnchors anchors() const override;
    OverlayArt overlayArt(OverlayKind kind) const override;

    lv_obj_t* panel() const override
    {
        return _pannel ? _pannel->get() : nullptr;
    }

    uitk::lvgl_cpp::Container* getPanel() const
    {
        return _pannel ? _pannel.get() : nullptr;
    }

    static constexpr uint32_t kChalk = 0xF4F1E9;
    static constexpr uint32_t kInk   = 0x000000;

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _pannel;
    std::unique_ptr<ClipTrack> _brow_l;
    std::unique_ptr<ClipTrack> _brow_r;
    std::unique_ptr<ClipTrack> _cheek_l;
    std::unique_ptr<ClipTrack> _cheek_r;
    bool _blushing = false;

    // The features are owned by the base as plain Features; the skin keeps typed handles so
    // the mouth can lead the eyes without a cast on every frame.
    ChalkEyes* _eye_l                 = nullptr;
    ChalkEyes* _eye_r                 = nullptr;
    ChalkMouth* _mouth                = nullptr;
    const chalk::ClipCompanion* _lead = nullptr;

    void applyCheeks();
    void follow(const chalk::ClipCompanion* lead);
};

}  // namespace stackchan::avatar
