#include "avatar_controller.h"
#include <stackchan/stackchan.h>
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <assets/lang_config.h>
#include <cstring>
#include <cstdio>
#include <memory>

#ifndef ESP_LOGI
#define ESP_LOGI(t, fmt, ...) printf("[%s] " fmt "\n", t, ##__VA_ARGS__)
#define ESP_LOGW(t, fmt, ...) printf("[%s:WARN] " fmt "\n", t, ##__VA_ARGS__)
#define ESP_LOGE(t, fmt, ...) printf("[%s:ERR] " fmt "\n", t, ##__VA_ARGS__)
#endif

static const char* TAG = "AvatarController";

#ifndef __EMSCRIPTEN__
static bool _is_xiaozhi_ready = false;
static bool _is_xiaozhi_idle  = false;

namespace hal_bridge {
    bool is_xiaozhi_ready() {
        return _is_xiaozhi_ready;
    }
    bool is_xiaozhi_idle() {
        return _is_xiaozhi_idle;
    }
}
#endif

AvatarController::AvatarController() {}
AvatarController::~AvatarController() {}

void AvatarController::CreateIdleMotionModifier() {
    auto& stackchan = GetStackChan();

    switch (idle_motion_level_) {
        case 0:
            idle_motion_modifier_id_ = -1;
            return;
        case 1:
            idle_motion_modifier_id_ = stackchan.addModifier(std::make_unique<stackchan::IdleMotionModifier>(8000, 12000));
            return;
        case 3:
            idle_motion_modifier_id_ = stackchan.addModifier(std::make_unique<stackchan::IdleMotionModifier>(2000, 4000));
            return;
        case 2:
        default:
            idle_motion_modifier_id_ = stackchan.addModifier(std::make_unique<stackchan::IdleMotionModifier>());
            return;
    }
}

void AvatarController::SetStatus(const char* status) {
    auto& stackchan = GetStackChan();

    // Readiness is a fact about xiaozhi, not about the avatar existing, so latch it
    // BEFORE the guard below. It used to live inside the STANDBY branch, which sits
    // after that early return -- and the tap-to-talk handler consults this flag, so
    // dropping the first STANDBY meant every tap was silently ignored for the rest of
    // the boot, with no log line to show for it.
    //
    // That is exactly what the USB path does: it skips the Mooncake launcher and goes
    // straight into xiaozhi (main.cpp), so the state machine reaches idle and fires
    // SetStatus(STANDBY) before SetupUI() has built the avatar. The launcher path
    // happened to be slow enough to hide the race.
#ifndef __EMSCRIPTEN__
    if (strcmp(status, Lang::Strings::STANDBY) == 0 && !_is_xiaozhi_ready) {
        _is_xiaozhi_ready = true;
        ESP_LOGI(TAG, "xiaozhi ready; face taps now toggle chat");
    }
#endif

    if (!stackchan.hasAvatar()) {
        ESP_LOGE(TAG, "Avatar is invalid in SetStatus");
        return;
    }

    auto& avatar = stackchan.avatar();

    bool is_idle = false;

    if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        if (speaking_modifier_id_ >= 0) {
            stackchan.removeModifier(speaking_modifier_id_);
            avatar.mouth().setWeight(0);
            speaking_modifier_id_ = -1;
        }

        GetHAL().setRgbColor(0, 0, 50, 0);
        GetHAL().refreshRgb();
        ESP_LOGI(TAG, "Status -> LISTENING");

    } else if (strcmp(status, Lang::Strings::STANDBY) == 0) {
#ifndef __EMSCRIPTEN__
        _is_xiaozhi_ready = true;
#endif

        if (speaking_modifier_id_ >= 0) {
            stackchan.removeModifier(speaking_modifier_id_);
            avatar.mouth().setWeight(0);
            speaking_modifier_id_ = -1;
        }

        is_idle = true;

        GetHAL().setRgbColor(0, 0, 0, 0);
        GetHAL().refreshRgb();
        ESP_LOGI(TAG, "Status -> STANDBY");

    } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        if (speaking_modifier_id_ < 0) {
            speaking_modifier_id_ = stackchan.addModifier(std::make_unique<stackchan::SpeakingModifier>(0, 180, false));
        }

        GetHAL().setRgbColor(0, 0, 0, 50);
        GetHAL().refreshRgb();
        ESP_LOGI(TAG, "Status -> SPEAKING");
    } else {
        avatar.setSpeech(status);
        ESP_LOGI(TAG, "Status -> '%s'", status);
    }

    if (is_idle) {
        ESP_LOGW(TAG, "Start idle motion");
        if (idle_motion_modifier_id_ < 0) {
            if (idle_motion_level_ > 0) {
                CreateIdleMotionModifier();
            }
            idle_expression_modifier_id_ = stackchan.addModifier(std::make_unique<stackchan::IdleExpressionModifier>());
        }

#ifndef __EMSCRIPTEN__
        _is_xiaozhi_idle = true;
#endif
    } else {
        ESP_LOGW(TAG, "Stop idle motion");
        if (idle_motion_modifier_id_ >= 0) {
            stackchan.removeModifier(idle_motion_modifier_id_);
            idle_motion_modifier_id_ = -1;
            stackchan.removeModifier(idle_expression_modifier_id_);
            idle_expression_modifier_id_ = -1;
        }

#ifndef __EMSCRIPTEN__
        _is_xiaozhi_idle = false;
#endif
    }

    if (is_sleeping_) {
        avatar.setSpeech("");
    }
}

void AvatarController::SetEmotion(const char* emotion) {
    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar() || !emotion) {
        return;
    }

    auto& avatar = stackchan.avatar();

    if (strcmp(emotion, "neutral") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Neutral);
    } else if (strcmp(emotion, "happy") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Happy);
    } else if (strcmp(emotion, "laughing") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Happy);
    } else if (strcmp(emotion, "angry") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Angry);
    } else if (strcmp(emotion, "sad") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Sad);
    } else if (strcmp(emotion, "crying") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Sad);
    } else if (strcmp(emotion, "sleepy") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Sleepy);
        avatar.setSpeech("Zzz…");
        is_sleeping_ = true;

        ESP_LOGW(TAG, "Stop idle motion");
        if (idle_motion_modifier_id_ >= 0) {
            stackchan.removeModifier(idle_motion_modifier_id_);
            idle_motion_modifier_id_ = -1;
            stackchan.removeModifier(idle_expression_modifier_id_);
            idle_expression_modifier_id_ = -1;
        }

        auto& motion = stackchan.motion();
        motion.pitchServo().moveWithSpeed(0, 80);

    } else if (strcmp(emotion, "doubtful") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Doubt);
    } else {
        ESP_LOGW(TAG, "Unknown emotion: %s, using NEUTRAL", emotion);
        avatar.setEmotion(stackchan::avatar::Emotion::Neutral);
    }

    if (blink_modifier_id_ >= 0) {
        auto blink_modifier = static_cast<stackchan::BlinkModifier*>(stackchan.getModifier(blink_modifier_id_));
        if (blink_modifier) {
            blink_modifier->resyncEyeWeights();
        }
    }
}

void AvatarController::SetChatMessage(const char* role, const char* content, bool setup_ui_called) {
    if (!setup_ui_called) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role ? role : "null", content ? content : "null");
    }

    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        return;
    }

    if (strcmp(role, "system") == 0) {
        stackchan.avatar().setSpeech(content);
    } else if (strcmp(role, "assistant") == 0) {
        stackchan.avatar().setSpeech(content);
    }
}

void AvatarController::ClearChatMessages() {
    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        return;
    }
    stackchan.avatar().clearSpeech();
    ESP_LOGI(TAG, "Chat messages cleared");
}
