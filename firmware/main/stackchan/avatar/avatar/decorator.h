/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "elements/element.h"
#include "../../utils/object_pool.h"

namespace stackchan::avatar {

/**
 * @brief Decorator base class
 *
 */
class Avatar;

class Decorator : public Poolable, public Element {
public:
    /**
     * @brief Called once by Avatar::addDecorator, before the decorator is first updated.
     *
     * Decorators build their LVGL objects here rather than in the constructor, because
     * where an overlay goes and what it looks like are properties of the skin it is drawn
     * on, and a constructor has no way to see that skin.
     */
    virtual void onAttach(Avatar& avatar)
    {
        (void)avatar;
    }
};

}  // namespace stackchan::avatar
