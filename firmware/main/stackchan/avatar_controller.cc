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

        stackchan::addon::GetLedStage().setStatus(stackchan::addon::LedStatus::Listening);
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

        // Idle has two meanings and they deserve to look different: waiting for you to
        // talk, versus nobody is even connected. Pale red says "I am fine, but there is
        // no host". Idle proper goes fully transparent rather than black -- which is the
        // difference that matters now that the LEDs are composited: a host that set a
        // colour gets its colour back when the conversation ends, instead of the robot
        // having quietly stamped black over it.
        //
        // Only the idle state carries this; listening/speaking keep their own wash,
        // because during a conversation the connection state is self-evident.
        stackchan::addon::GetLedStage().setStatus(hal_bridge::is_host_connected()
                                                      ? stackchan::addon::LedStatus::Idle
                                                      : stackchan::addon::LedStatus::Waiting);
        ESP_LOGI(TAG, "Status -> STANDBY");

    } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        if (speaking_modifier_id_ < 0) {
            speaking_modifier_id_ = stackchan.addModifier(std::make_unique<stackchan::SpeakingModifier>(0, 180, false));
        }

        stackchan::addon::GetLedStage().setStatus(stackchan::addon::LedStatus::Speaking);
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

    // A status change wakes the face from its Zzz. Only the Zzz: this used to clear the
    // bubble outright, and because nothing ever reset the flag it tested, one sleepy face
    // meant every later status change wiped whatever text was showing.
    if (showing_zzz_) {
        avatar.clearSpeech();
        showing_zzz_ = false;
    }
}

void AvatarController::SetEmotion(const char* emotion) {
    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar() || !emotion) {
        return;
    }

    auto& avatar = stackchan.avatar();

    // The Zzz belongs to the sleepy face and leaves with it. It used to be written into the
    // bubble and never taken back, so a face set to anything else afterwards still slept
    // under a Zzz until some unrelated status change happened to clear it.
    if (showing_zzz_ && strcmp(emotion, "sleepy") != 0) {
        avatar.clearSpeech();
        showing_zzz_ = false;
    }

    if (strcmp(emotion, "neutral") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Neutral);
    } else if (strcmp(emotion, "happy") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Happy);
    } else if (strcmp(emotion, "laughing") == 0) {
        // Its own expression now, not a second spelling of "happy": open laughing mouths,
        // the > < squeeze, and a burst that plays as soon as it is set.
        avatar.setEmotion(stackchan::avatar::Emotion::Laugh);
    } else if (strcmp(emotion, "angry") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Angry);
    } else if (strcmp(emotion, "sad") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Sad);
    } else if (strcmp(emotion, "crying") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Sad);
    } else if (strcmp(emotion, "sleepy") == 0) {
        avatar.setEmotion(stackchan::avatar::Emotion::Sleepy);
        avatar.setSpeech("Zzz…");
        showing_zzz_ = true;

        ESP_LOGW(TAG, "Stop idle motion");
        if (idle_motion_modifier_id_ >= 0) {
            stackchan.removeModifier(idle_motion_modifier_id_);
            idle_motion_modifier_id_ = -1;
            stackchan.removeModifier(idle_expression_modifier_id_);
            idle_expression_modifier_id_ = -1;
        }

        auto& motion = stackchan.motion();
        motion.movePitchWithSpeed(0, 80);

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

    // The bubble is the robot's own voice, so only the robot's roles write it. "user" -- an
    // `stt` transcript -- is deliberately dropped: upstream draws it in a chat log this face
    // does not have, and putting the person's words in the robot's bubble would show the
    // robot saying them. A host that wants arbitrary text on screen has
    // self.screen.show_speech_bubble for exactly that.
    if (strcmp(role, "system") == 0 || strcmp(role, "assistant") == 0) {
        stackchan.avatar().setSpeech(content);
        showing_zzz_ = false;
    }
}

void AvatarController::ClearChatMessages() {
    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        return;
    }
    stackchan.avatar().clearSpeech();
    showing_zzz_ = false;
    ESP_LOGI(TAG, "Chat messages cleared");
}
