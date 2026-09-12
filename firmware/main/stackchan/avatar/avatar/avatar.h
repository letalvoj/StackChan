/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "elements/key_elements.h"
#include "decorator.h"
#include <lvgl.h>
#include <memory>

namespace stackchan::avatar {

/**
 * @brief Where a skin draws its features, in pixels from the panel centre.
 *
 * Decorators used to carry the default skin's eye and cheek slots as file-scope constants,
 * so every overlay landed in the wrong place the moment a second skin existed: the dizzy
 * spirals sat on the old 16 px eye slots while the face around them had moved and grown.
 * A skin declares its own geometry here and decorators position against it.
 */
struct FaceAnchors {
    uitk::Vector2i leftEye;
    uitk::Vector2i rightEye;
    uitk::Vector2i leftCheek;
    uitk::Vector2i rightCheek;
    uitk::Vector2i mouth;

    /// Half-width of the open eye. Overlays that stand in for an eye scale to this.
    int eyeRadius = 16;

    /// True when the skin already draws blush of its own, so the shy overlay is redundant
    /// and would land a second set of cheek marks a few pixels off the first.
    bool skinDrawsBlush = false;
};

/**
 * @brief An overlay a decorator draws, supplied by the skin rather than by the decorator.
 *
 * Each skin owns its own artwork for these. A decorator asks for the sprite and the slot
 * and only contributes the animation, which is the part that is genuinely skin-independent.
 */
enum class OverlayKind {
    Heart,
    HeartSparkle,
    Shy,
    AngryMark,
    Sweat,
    DizzyEye,
    DizzyMouth,
    DizzyWobble,
};

struct OverlayArt {
    /// Primary sprite, and its mirrored partner for the paired overlays (cheeks, eyes).
    const lv_image_dsc_t* left  = nullptr;
    const lv_image_dsc_t* right = nullptr;
    uitk::Vector2i leftPos{};
    uitk::Vector2i rightPos{};
    /// Recolour applied to the sprite. Alpha-only art is tinted with this.
    uint32_t color = 0xFFFFFF;
    /// False when this skin has nothing to draw for the kind, and the decorator skips it.
    bool valid = false;
};

/**
 * @brief Avatar base class
 *
 */
class Avatar {
public:
    /**
     * @brief Update avatar, trigger all elements, decorators and modifiers to update
     *
     */
    virtual void update()
    {
        _key_elements.forEach([](Element* element) {
            // Update all elements
            element->_update();
        });

        _decorator_pool.forEach([this](Decorator* decorator, int id) {
            // Update all decorators
            decorator->_update();
        });

        // Cleanup pools
        _decorator_pool.cleanup();
    }

    const KeyElements_t& getKeyElements()
    {
        return _key_elements;
    }

    virtual void setEmotion(const Emotion& emotion)
    {
        _emotion = emotion;

        _key_elements.forEach([&emotion](Element* element) {
            // Set for all elements
            element->setEmotion(emotion);
        });

        _decorator_pool.forEach([&emotion](Decorator* decorator, int id) {
            // Set for all decorators
            decorator->setEmotion(emotion);
        });
    }

    Emotion getEmotion() const
    {
        return _emotion;
    }

    /**
     * @brief Put every feature back where the emotion alone would have it.
     *
     * Modifiers write to features directly and are not obliged to tidy up after
     * themselves -- a shake hides the eyes, speaking leaves the mouth open, idle leaves a
     * drift and a gaze. Whoever takes over needs a defined starting point rather than
     * whatever the last owner happened to leave behind, and reconstructing that by hand
     * at each call site is how states nobody intended get on screen.
     */
    virtual void resetFeatures()
    {
        _key_elements.forEach([](Element* element) {
            element->setVisible(true);
            element->setPosition({0, 0});
            element->setRotation(0);
        });
        leftEye().setGaze({0, 0});
        rightEye().setGaze({0, 0});
        mouth().setWeight(0);
        clearSpeech();
        setEmotion(_emotion);   // restores each feature's resting weight for the emotion
    }

    /**
     * @brief The full-screen object the face is drawn on.
     *
     * Tap-to-talk is the device's only control and it lives on this panel, so "where do
     * taps land" is part of being an avatar rather than a detail of one skin. Callers that
     * only want the tap should use face::onFaceTapped instead of reaching in here.
     */
    virtual lv_obj_t* panel() const
    {
        return nullptr;
    }

    /**
     * @brief Blush, or stop blushing.
     *
     * In this artwork a blush is not an overlay: it is the cheek track drawing a different
     * family. Adding pink marks on top of the chalk ones puts two sets of cheeks on the
     * face a few pixels apart, which is exactly what the reference does not do.
     */
    virtual void setBlush(bool blushing)
    {
        (void)blushing;
    }

    Feature& leftEye()
    {
        return *getKeyElements().leftEye;
    }

    Feature& rightEye()
    {
        return *getKeyElements().rightEye;
    }

    Feature& mouth()
    {
        return *getKeyElements().mouth;
    }

    void setSpeech(std::string_view text)
    {
        if (getKeyElements().speechBubble) {
            getKeyElements().speechBubble->setSpeech(text);
        }
    }

    void clearSpeech()
    {
        if (getKeyElements().speechBubble) {
            getKeyElements().speechBubble->clearSpeech();
        }
    }

    void setSpeechTextFont(void* font)
    {
        if (getKeyElements().speechBubble) {
            getKeyElements().speechBubble->setTextFont(font);
        }
    }

    void setModifyLock(bool locked)
    {
        _is_modify_locked = locked;
    }

    bool isModifyLocked()
    {
        return _is_modify_locked;
    }

    /* ------------------------------ Skin geometry ----------------------------- */

    /**
     * @brief Where this skin puts its eyes, cheeks and mouth.
     *
     * The default is the original skin's layout, so a skin that does not override this
     * keeps rendering exactly as it did.
     */
    virtual FaceAnchors anchors() const
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

    /**
     * @brief The sprite and slot this skin lends to a decorator.
     *
     * Returning an invalid OverlayArt means the skin draws nothing for that kind, and the
     * decorator quietly renders nothing rather than falling back to another skin's art.
     */
    virtual OverlayArt overlayArt(OverlayKind kind) const
    {
        (void)kind;
        return {};
    }

    /* ---------------------------- Decorator helpers --------------------------- */

    /**
     * @brief Adopt a decorator and give it the face it is being drawn on.
     *
     * Attachment is separate from construction so a decorator can read anchors and artwork
     * from the avatar before it builds any LVGL objects.
     */
    int addDecorator(std::unique_ptr<Decorator> decorator)
    {
        if (decorator) {
            decorator->onAttach(*this);
        }
        return _decorator_pool.create(std::move(decorator));
    }

    bool removeDecorator(int id)
    {
        return _decorator_pool.destroy(id);
    }

    void clearDecorators()
    {
        _decorator_pool.clear();
    }

protected:
    Avatar() = default;

    Emotion _emotion = Emotion::Neutral;
    KeyElements_t _key_elements;
    ObjectPool<Decorator> _decorator_pool;

    bool _is_modify_locked = false;
};

}  // namespace stackchan::avatar
