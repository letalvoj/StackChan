#include "wasm_display.h"
#include <stackchan/stackchan.h>
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <assets/lang_config.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

static void sync_xiaozhi_flags() {
    EM_ASM({
        Module._is_xiaozhi_ready = $0 ? true : false;
        Module._is_xiaozhi_idle = $1 ? true : false;
    }, hal_bridge::is_xiaozhi_ready() ? 1 : 0, hal_bridge::is_xiaozhi_idle() ? 1 : 0);
}

WasmDisplay::WasmDisplay() : Display() {
    width_ = 320;
    height_ = 240;
    printf("[WASM_DISPLAY] Instantiating WasmDisplay HAL hardware binding with shared AvatarController.\n");
}

WasmDisplay::~WasmDisplay() {}

void WasmDisplay::SetupUI() {
    setup_ui_called_ = true;
    printf("[WASM_DISPLAY] SetupUI() invoked — checking avatar attachment on active LVGL screen...\n");
    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        auto av = std::make_unique<stackchan::avatar::DefaultAvatar>();
        av->init(lv_screen_active());
        av->getPanel()->onClick().connect([]() {
            static uint32_t last_toggle_tick = 0;
            const uint32_t now = GetHAL().millis();
            if (last_toggle_tick != 0 && now - last_toggle_tick < 2000) {
                return;
            }
            if (hal_bridge::is_xiaozhi_ready()) {
                last_toggle_tick = now;
                printf("[WASM_DISPLAY] Avatar face panel clicked -> routing via hal_bridge::toggle_xiaozhi_chat_state().\n");
                hal_bridge::toggle_xiaozhi_chat_state();
            }
        });
        stackchan.attachAvatar(std::move(av));
        printf("[WASM_DISPLAY] ✓ DefaultAvatar attached and initialized with debounced click handler.\n");
    }
    auto& avatar = stackchan.avatar();
    avatar.leftEye().setVisible(true);
    avatar.rightEye().setVisible(true);
    avatar.mouth().setVisible(true);
    avatar.setEmotion(stackchan::avatar::Emotion::Neutral);

    stackchan.addModifier(std::make_unique<stackchan::BreathModifier>());
    controller_.SetBlinkModifierId(stackchan.addModifier(std::make_unique<stackchan::BlinkModifier>()));
    stackchan.addModifier(std::make_unique<stackchan::HeadPetModifier>());
    stackchan.addModifier(std::make_unique<stackchan::ImuEventModifier>());

    auto config = hal_bridge::get_xiaozhi_config();
    int idle_level = config.idleRandomMovementLevel > 0 ? config.idleRandomMovementLevel : 2;
    controller_.SetIdleMotionLevel(idle_level);
    printf("[WASM_DISPLAY] ✓ Initialized Breath/Blink/HeadPet/Imu modifiers, click handler & SetIdleMotionLevel(%d).\n", idle_level);
}

void WasmDisplay::SetStatus(const char* status) {
    controller_.SetStatus(status);
    sync_xiaozhi_flags();
}

void WasmDisplay::SetEmotion(const char* emotion) {
    controller_.SetEmotion(emotion);
}

void WasmDisplay::SetChatMessage(const char* role, const char* content) {
    controller_.SetChatMessage(role, content, setup_ui_called_);
}

void WasmDisplay::ClearChatMessages() {
    controller_.ClearChatMessages();
}
