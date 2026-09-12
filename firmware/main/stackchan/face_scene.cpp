/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_scene.h"
#include "avatar/skins/chalk/chalk.h"
#include "avatar/skins/default/default.h"
#include <vector>

namespace stackchan::face {

std::unique_ptr<avatar::Avatar> createAvatar(lv_obj_t* parent, Skin skin, const lv_font_t* font)
{
    switch (skin) {
        case Skin::Default: {
            auto a = std::make_unique<avatar::DefaultAvatar>();
            a->init(parent, font);
            return a;
        }
        case Skin::Chalk:
        default: {
            auto a = std::make_unique<avatar::ChalkAvatar>();
            a->init(parent, font);
            return a;
        }
    }
}

namespace {
/// Kept alive for the life of the program; there is one face and it outlives every caller.
std::vector<std::function<void()>>& tapHandlers()
{
    static std::vector<std::function<void()>> handlers;
    return handlers;
}
}  // namespace

bool onFaceTapped(avatar::Avatar& avatar, std::function<void()> handler)
{
    lv_obj_t* panel = avatar.panel();
    if (!panel || !handler) {
        return false;
    }
    tapHandlers().push_back(std::move(handler));
    lv_obj_add_event_cb(
        panel,
        [](lv_event_t* e) {
            auto* fn = static_cast<std::function<void()>*>(lv_event_get_user_data(e));
            if (fn && *fn) {
                (*fn)();
            }
        },
        LV_EVENT_CLICKED, &tapHandlers().back());
    return true;
}

FaceMargins measureMargins(avatar::Avatar& avatar)
{
    // Walk the panel's children and union their drawn areas. Coordinates come from LVGL
    // after layout, so this measures what is actually on screen rather than what the
    // sprite table says should be.
    FaceMargins m;

    // The avatar does not expose its panel through the base interface, and it does not
    // need to: the panel covers the screen and every drawn element is inside it.
    lv_obj_t* panel = lv_screen_active();
    if (!panel) {
        return m;
    }

    lv_area_t bounds{kScreenWidth, kScreenHeight, 0, 0};
    bool any = false;

    // Two levels deep covers panel -> feature; deeper nesting would need recursion, and
    // no skin currently has any.
    const uint32_t children = lv_obj_get_child_count(panel);
    for (uint32_t i = 0; i < children; ++i) {
        lv_obj_t* child = lv_obj_get_child(panel, i);
        const uint32_t grandchildren = lv_obj_get_child_count(child);
        for (uint32_t j = 0; j < grandchildren; ++j) {
            lv_obj_t* leaf = lv_obj_get_child(child, j);
            if (lv_obj_has_flag(leaf, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            lv_area_t a;
            lv_obj_get_coords(leaf, &a);
            if (a.x2 <= a.x1 || a.y2 <= a.y1) {
                continue;
            }
            if (!any) {
                bounds = a;
                any    = true;
            } else {
                if (a.x1 < bounds.x1) bounds.x1 = a.x1;
                if (a.y1 < bounds.y1) bounds.y1 = a.y1;
                if (a.x2 > bounds.x2) bounds.x2 = a.x2;
                if (a.y2 > bounds.y2) bounds.y2 = a.y2;
            }
        }
    }

    if (!any) {
        return m;
    }
    m.left   = bounds.x1;
    m.top    = bounds.y1;
    m.right  = kScreenWidth - 1 - bounds.x2;
    m.bottom = kScreenHeight - 1 - bounds.y2;
    return m;
}

}  // namespace stackchan::face
