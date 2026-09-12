/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "avatar/avatar.h"
#include "face_state.h"
#include <lvgl.h>
#include <functional>
#include <memory>

namespace stackchan::face {

/// The faces that exist. Adding one means adding a case in createAvatar and nothing else.
enum class Skin {
    Chalk,     ///< the shipping face
    Default,   ///< the original, kept as a reference to diff against
};

/**
 * @brief Build a face on a panel of the right size.
 *
 * Every app used to do `make_unique<SomeAvatar>()` then `init(lv_screen_active())`, six
 * times over, and each skin hardcoded the panel dimensions inside its own init(). That is
 * how the shipping face ended up being chosen in a `.cc` nobody grepped, and how the
 * preview and the device could disagree about how big the screen is without either
 * noticing. One function, one definition of the panel.
 *
 * @param parent   where the face's panel is created
 * @param skin     which face
 * @param font     used by the speech bubble
 */
std::unique_ptr<avatar::Avatar> createAvatar(lv_obj_t* parent, Skin skin = Skin::Chalk,
                                             const lv_font_t* font = &lv_font_montserrat_16);

/**
 * @brief Call back whenever the face is tapped.
 *
 * Tap-to-talk is the device's only control, and the panel it lives on is full-screen and
 * not scrollable -- so LVGL reports any press-then-release over it as a click, however far
 * the finger travelled. That is a real hazard (it once collided with the swipe-up home
 * gesture), and it is better owned once here than rediscovered at each call site.
 *
 * @return false when the avatar has no panel to attach to.
 */
bool onFaceTapped(avatar::Avatar& avatar, std::function<void()> handler);

/**
 * @brief How far the face's ink reaches, relative to the panel.
 *
 * The chalk art is baked larger than the panel it sits on, so "does it fit" is a real
 * question with a real answer rather than something to eyeball. Returns the margin in
 * pixels on each edge; a negative value means the art is being clipped.
 */
struct FaceMargins {
    int left   = 0;
    int right  = 0;
    int top    = 0;
    int bottom = 0;

    int smallest() const
    {
        int m = left;
        if (right < m) m = right;
        if (top < m) m = top;
        if (bottom < m) m = bottom;
        return m;
    }
};

/// Measure the drawn face against the panel. Costs a render, so it is a review tool
/// rather than something to call per frame.
FaceMargins measureMargins(avatar::Avatar& avatar);

}  // namespace stackchan::face
