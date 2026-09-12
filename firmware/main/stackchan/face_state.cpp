/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "face_state.h"

namespace stackchan::face {

void FaceState::applyTo(avatar::Avatar& avatar) const
{
    // Start from a defined face rather than from whatever the last writer left. The
    // emotion is set first because a skin's setEmotion chooses each feature's resting
    // weight, and the overrides below are deltas on top of that.
    avatar.setEmotion(emotion);
    avatar.resetFeatures();

    if (eyeWeight >= 0) {
        avatar.leftEye().setWeight(eyeWeight);
        avatar.rightEye().setWeight(eyeWeight);
    }
    if (mouthWeight >= 0) {
        avatar.mouth().setWeight(mouthWeight);
    }

    avatar.leftEye().setPosition(eyePosition);
    avatar.rightEye().setPosition(eyePosition);
    avatar.leftEye().setGaze(gaze);
    avatar.rightEye().setGaze(gaze);
    avatar.mouth().setPosition(mouthPosition);
    avatar.mouth().setRotation(mouthRotation);

    if (hideEyes) {
        avatar.leftEye().setVisible(false);
        avatar.rightEye().setVisible(false);
    }
    if (hideMouth) {
        avatar.mouth().setVisible(false);
    }
    if (!speech.empty()) {
        avatar.setSpeech(speech);
    }
}

std::vector<int> attachReaction(avatar::Avatar& avatar, lv_obj_t* parent, Reaction reaction,
                                uint32_t lifetimeMs)
{
    std::vector<int> ids;
    auto add = [&](std::unique_ptr<avatar::Decorator> d) { ids.push_back(avatar.addDecorator(std::move(d))); };

    switch (reaction) {
        case Reaction::Affection:
            add(std::make_unique<avatar::HeartDecorator>(parent, lifetimeMs, 500));
            break;

        case Reaction::Bashful:
            avatar.setBlush(true);
            break;

        case Reaction::HeadPet:
            add(std::make_unique<avatar::HeartDecorator>(parent, lifetimeMs, 500));
            avatar.setBlush(true);
            break;

        case Reaction::Irritation:
            add(std::make_unique<avatar::AngryDecorator>(parent, lifetimeMs, 500));
            break;

        case Reaction::Discomfort:
            add(std::make_unique<avatar::SweatDecorator>(parent, lifetimeMs, 700));
            break;

        case Reaction::Disoriented:
            add(std::make_unique<avatar::DizzyDecorator>(parent, lifetimeMs, 300));
            avatar.setBlush(true);
            break;

        case Reaction::None:
        default:
            break;
    }
    return ids;
}

void detachReaction(avatar::Avatar& avatar, const std::vector<int>& ids)
{
    for (int id : ids) {
        avatar.removeDecorator(id);
    }
    avatar.setBlush(false);
}

}  // namespace stackchan::face
