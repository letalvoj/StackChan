#include "wasm_application.h"
#include <hal/hal.h>
#include <assets/lang_config.h>
#include <emscripten.h>
#include <stdio.h>
#include <cstring>
#include <lvgl.h>
#include <stackchan/stackchan.h>

WasmApplication::WasmApplication()
    : ApplicationCore(), display_()
{
    printf("[WASM_APP] WasmApplication constructor instantiated with 320x240 virtual display.\n");
}

WasmApplication::~WasmApplication() {
}

void WasmApplication::SendPcmFrame(const std::vector<int16_t>& frame) {
    if (!protocol_) return;
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->payload.resize(frame.size() * sizeof(int16_t));
    memcpy(packet->payload.data(), frame.data(), packet->payload.size());
    packet->sample_rate = 16000;
    protocol_->SendAudio(std::move(packet));
}

void WasmApplication::Initialize() {
    printf("[WASM_APP] Initializing WasmApplication (Phase 2 — Option B+ ApplicationCore)...\n");

    display_.SetupUI();

    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        last_old_state_ = old_state;
        HandleStateChanged(last_old_state_, new_state);
    });

    audio_processor_ = std::make_unique<WasmAudioProcessor>();
    audio_processor_->Initialize(nullptr, 60, nullptr);
    audio_processor_->OnVadStateChange([this](bool speaking) {
        printf("[WASM_APP] OnVadStateChange(%s) — VAD voice activity signaled.\n", speaking ? "true" : "false");
        Schedule([this, speaking]() {
            if (GetDeviceState() != kDeviceStateListening) return;
            GetAvatarDisplay().SetStatus(speaking ? "RECORDING" : Lang::Strings::LISTENING);

            if (speaking) {
                // Speech start: flush pre-roll ring buffer as backfill, open gate
                vad_gate_open_ = true;
                post_roll_remaining_ = 0;  // cancel any pending post-roll from previous segment

                printf("[WASM_APP:VAD_GATE] Speech start. Backfilling %zu pre-roll frames (~%dms)\n",
                       pre_roll_buffer_.size(), (int)(pre_roll_buffer_.size() * 60));
                for (const auto& frame : pre_roll_buffer_) {
                    SendPcmFrame(frame);
                }
                pre_roll_buffer_.clear();
            } else {
                // Speech end: close gate, start post-roll countdown
                vad_gate_open_ = false;
                post_roll_remaining_ = kPostRollFrames;
                printf("[WASM_APP:VAD_GATE] Speech end. Starting %d-frame post-roll.\n", kPostRollFrames);
                // detect_end will be sent when post_roll_remaining_ hits 0 in OnOutput
            }
        });
    });
    audio_processor_->OnOutput([this](std::vector<int16_t>&& pcm) {
        if (GetDeviceState() != kDeviceStateListening || !protocol_) return;

        if (vad_gate_open_) {
            // Speech active: stream live
            SendPcmFrame(pcm);
        } else if (post_roll_remaining_ > 0) {
            // Post-roll: send trailing frames after speech_end
            SendPcmFrame(pcm);
            post_roll_remaining_--;
            if (post_roll_remaining_ == 0) {
                // Post-roll complete: NOW send detect_end
                protocol_->SendText("{\"type\":\"listen\",\"state\":\"detect_end\"}");
                printf("[WASM_APP:VAD_GATE] Post-roll complete. detect_end sent.\n");
            }
        } else {
            // Silence: accumulate into pre-roll ring buffer
            pre_roll_buffer_.push_back(std::move(pcm));
            while (pre_roll_buffer_.size() > (size_t)kPreRollFrames) {
                pre_roll_buffer_.pop_front();
            }
        }
    });

    protocol_ = std::make_unique<WasmProtocol>("ws://localhost:8081/ws");
    WireProtocolCallbacks();
    protocol_->Start();
    protocol_->OpenAudioChannel();

    EM_ASM({
        Module._isAssistantActive = true;
        Module._isAssistantAppRunning = true;
    });

    // Honor legitimate DeviceStateMachine transitions: unknown -> starting -> activating -> idle
    SetDeviceState(kDeviceStateStarting);
    SetDeviceState(kDeviceStateActivating);
    SetDeviceState(kDeviceStateIdle);
    printf("[WASM_APP] ✓ WasmApplication initialized & transitioned through boot states to STANDBY.\n");
}

void WasmApplication::Update() {
    ProcessScheduledTasks();
}

void WasmApplication::NotifySchedule() {
    // Cooperative update loop in WasmApplication::Update() executes scheduled tasks every frame (60fps)
}

void WasmApplication::HandleStateChanged(DeviceState old_state, DeviceState new_state) {
    ApplicationCore::HandleStateChanged(old_state, new_state);
    const char* state_str = "unknown";
    switch (new_state) {
        case kDeviceStateIdle:
            state_str = "idle";
            break;
        case kDeviceStateListening:
            state_str = "listening";
            break;
        case kDeviceStateSpeaking:
            state_str = "speaking";
            break;
        case kDeviceStateConnecting:
            state_str = "connecting";
            break;
        case kDeviceStateStarting:
            state_str = "starting";
            break;
        case kDeviceStateActivating:
            state_str = "activating";
            break;
        default:
            state_str = "idle";
            break;
    }
    printf("[WASM_APP] Authoritative state machine transition: %d -> %d (%s)\n", old_state, new_state, state_str);
    EM_ASM({
        if (typeof window._onFirmwareStateChanged === 'function') {
            window._onFirmwareStateChanged(UTF8ToString($0));
        }
    }, state_str);
}

void WasmApplication::OnAudioVoiceProcessing(bool enable) {
    printf("[WASM_APP] OnAudioVoiceProcessing(enable=%d)\n", enable ? 1 : 0);
    if (enable) {
        GetAudioProcessor().Start();
    } else {
        GetAudioProcessor().Stop();
        // Reset VAD gate state when leaving listening
        vad_gate_open_ = false;
        post_roll_remaining_ = 0;
        pre_roll_buffer_.clear();
    }
    EM_ASM({
        Module._audioProcessingEnabled = $0 ? true : false;
        if (typeof Module.onAudioVoiceProcessing === 'function') {
            Module.onAudioVoiceProcessing($0);
        }
    }, enable ? 1 : 0);
}

void WasmApplication::OnAudioResetDecoder() {
    printf("[WASM_APP] OnAudioResetDecoder() invoked.\n");
}

void WasmApplication::OnAudioIncoming(std::unique_ptr<AudioStreamPacket> packet) {
    if (!packet || packet->payload.empty()) return;
    printf("[WASM_APP] OnAudioIncoming(%zu bytes, rate=%d)\n", packet->payload.size(), packet->sample_rate);
    EM_ASM({
        if (typeof window._streamPlaybackChunk === 'function') {
            var ptr = $0;
            var len = $1;
            var pcmBytes = Module.HEAPU8.slice(ptr, ptr + len);
            window._streamPlaybackChunk(pcmBytes.buffer);
        }
    }, packet->payload.data(), packet->payload.size());
}

void WasmApplication::Reboot() {
    printf("[WASM_APP] Reboot requested — reloading page in 500ms...\n");
    EM_ASM({ setTimeout(function() { location.reload(); }, 500); });
}

extern "C" EMSCRIPTEN_KEEPALIVE void wasm_app_toggle_chat() {
    auto& app = WasmApplication::GetInstance();
    if (app.GetDeviceState() == kDeviceStateUnknown) app.Initialize();
    app.ToggleChat();
}

extern "C" EMSCRIPTEN_KEEPALIVE void wasm_app_start_listening() {
    auto& app = WasmApplication::GetInstance();
    if (app.GetDeviceState() == kDeviceStateUnknown) app.Initialize();
    app.SetListeningMode(kListeningModeAutoStop);
}
