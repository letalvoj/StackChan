/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../face_state.h"
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <cstdint>
#include <vector>

namespace stackchan {

/**
 * @brief
 *
 */
class ImuEventModifier : public Modifier {
public:
    ImuEventModifier(uint32_t reactionDurationMs = 4000) : _reaction_duration_ms(reactionDurationMs)
    {
        _signal_connection = GetHAL().onImuMotionEvent.connect([this](ImuMotionEvent event) {
            if (event == ImuMotionEvent::Shake) {
                _event_shake = true;
            }
        });
    }

    ~ImuEventModifier()
    {
        GetHAL().onImuMotionEvent.disconnect(_signal_connection);
    }

    void _update(Modifiable& stackchan) override
    {
        uint32_t now = GetHAL().millis();

        // 收到晃动事件
        if (_event_shake) {
            _event_shake = false;
            handle_shake_start(stackchan, now);
        }

        // 如果处于晃动反应状态
        if (_is_reacting) {
            // A skin whose dizzy overlay includes its own wavy mouth has already drawn one,
            // and rocking a second mouth underneath it just makes two.
            if (!_overlay_draws_mouth && now >= _next_toggle_tick) {
                _next_toggle_tick = now + 600;

                _toggle_phase      = !_toggle_phase;
                int mouth_rotation = _toggle_phase ? -25 : 25;

                auto& avatar = stackchan.avatar();
                avatar.mouth().setRotation(mouth_rotation);
                avatar.mouth().setWeight(65);
            }

            //  Lock motion modify and move home
            auto& motion = stackchan.motion();
            if (!motion.isModifyLocked()) {
                motion.setModifyLock(true);
                motion.goHome(300);
            }

            // 检查是否结束反应
            if (now >= _restore_at) {
                restore_state(stackchan);
            }
        }
    }

private:
    void handle_shake_start(Modifiable& stackchan, uint32_t now)
    {
        if (!_is_reacting) {
            // 首次触发时，记录状态以便恢复
            _is_reacting = true;

            // Same idea as head_pet: the dizzy eyes are the local reaction, this lets a
            // connected agent notice it was picked up and moved around.
            hal_bridge::report_sensor_event("shaken");

            auto& avatar = stackchan.avatar();

            avatar.setModifyLock(true);
            // The spirals stand in for the eyes rather than sitting on top of them. That
            // substitution is the effect: hiding the eyes first is what makes a spiral
            // read as an eye instead of as a sticker over one.
            avatar.leftEye().setVisible(false);
            avatar.rightEye().setVisible(false);

            _overlay_draws_mouth = avatar.overlayArt(stackchan::avatar::OverlayKind::DizzyMouth).valid;
            if (_overlay_draws_mouth) {
                avatar.mouth().setVisible(false);
            }

            // What "disoriented" looks like is defined once, in the face layer, so the
            // preview cannot show a different set of overlays than the device draws.
            face::detachReaction(avatar, _reaction_ids);
            _reaction_ids = face::attachReaction(avatar, lv_screen_active(), face::Reaction::Disoriented);
        }

        // 刷新恢复时间和切换时间
        _restore_at = now + _reaction_duration_ms;
        if (_next_toggle_tick <= now) {
            _next_toggle_tick = now;  // 立即触发第一次嘴巴动作
        }
    }

    void restore_state(Modifiable& stackchan)
    {
        if (!_is_reacting) {
            return;
        }

        auto& avatar = stackchan.avatar();
        avatar.setModifyLock(false);
        // The shake hid the eyes, hid or rocked the mouth, and left a weight behind. One
        // call puts all of that back rather than four that have to stay in step with it.
        avatar.resetFeatures();
        _overlay_draws_mouth = false;

        face::detachReaction(avatar, _reaction_ids);
        _reaction_ids.clear();

        auto& motion = stackchan.motion();
        motion.setModifyLock(false);

        _is_reacting = false;
    }

    // 信号相关
    int _signal_connection;
    volatile bool _event_shake = false;

    // 状态控制
    bool _is_reacting          = false;
    bool _toggle_phase         = false;
    bool _overlay_draws_mouth  = false;
    uint32_t _restore_at       = 0;
    uint32_t _next_toggle_tick = 0;
    uint32_t _reaction_duration_ms;

    std::vector<int> _reaction_ids;
};

}  // namespace stackchan
