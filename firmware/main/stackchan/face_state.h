/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "avatar/avatar.h"
#include "avatar/decorators/decorators.h"
#include "face_states.h"
#include <lvgl.h>
#include <string>
#include <vector>

namespace stackchan::face {

/**
 * @brief A reaction, named by what it means rather than by the overlays it happens to use.
 *
 * Both the modifiers and the preview want "show me a head-pet". Left as raw overlay lists
 * they drift: the harness drew heart-plus-blush for head-pet while the modifier drew
 * something slightly different, and nobody noticed because the two lists lived apart.
 */
enum class Reaction {
    None,
    Affection,    ///< a heart, beating
    Bashful,      ///< blush across both cheeks
    HeadPet,      ///< affection and bashful together, which is what petting looks like
    Irritation,   ///< the anger mark
    Discomfort,   ///< a bead of sweat
    Disoriented,  ///< spirals standing in for the eyes
};

/**
 * @brief Everything that makes one frame of the face.
 *
 * The runtime writes to features one at a time, from several modifiers, none of which is
 * obliged to tidy up. This is the whole-face view: what the face looks like right now, as
 * a value that can be stored, compared, or applied. It is what lets the preview describe a
 * frame without reimplementing how a frame is built.
 */
struct FaceState {
    avatar::Emotion emotion = avatar::Emotion::Neutral;

    /// Lid opening and mouth opening, 0-100. Negative means "whatever the emotion rests
    /// at", which is not the same as zero and is the common case.
    int eyeWeight   = -1;
    int mouthWeight = -1;

    /// Where the pupils point inside the eye, -100..100. Distinct from eyePosition, which
    /// slides the whole eye across the face.
    uitk::Vector2i gaze{0, 0};
    uitk::Vector2i eyePosition{0, 0};
    uitk::Vector2i mouthPosition{0, 0};
    int mouthRotation = 0;

    /// A reaction may replace a feature rather than sit over it -- the spirals stand in
    /// for the eyes, and a skin that draws its own dizzy mouth hides the real one.
    bool hideEyes  = false;
    bool hideMouth = false;

    std::string speech;

    Reaction reaction = Reaction::None;

    /// Reset the face, then apply this state to it. The reset half matters: without it
    /// whatever the previous owner left behind is still set.
    void applyTo(avatar::Avatar& avatar) const;
};

/**
 * @brief Attach the overlays a reaction shows, and return their ids so they can be removed.
 *
 * Parented wherever the caller says, because the modifiers parent to the active screen and
 * anything previewing them has to match that to get the same z-order.
 */
std::vector<int> attachReaction(avatar::Avatar& avatar, lv_obj_t* parent, Reaction reaction,
                                uint32_t lifetimeMs = 0);

/// Remove overlays attached by attachReaction.
void detachReaction(avatar::Avatar& avatar, const std::vector<int>& ids);

}  // namespace stackchan::face
