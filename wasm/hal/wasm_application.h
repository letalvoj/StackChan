#ifndef WASM_APPLICATION_H
#define WASM_APPLICATION_H

#include <application_core.h>
#include "wasm_display.h"
#include "wasm_protocol.h"
#include "wasm_audio_processor.h"
#include <memory>
#include <deque>
#include <vector>

class WasmApplication : public ApplicationCore {
public:
    static constexpr int kPreRollFrames = 5;   // ~300ms at 60ms/frame
    static constexpr int kPostRollFrames = 5;  // ~300ms at 60ms/frame

    static WasmApplication& GetInstance() {
        static WasmApplication instance;
        return instance;
    }

    WasmApplication(const WasmApplication&) = delete;
    WasmApplication& operator=(const WasmApplication&) = delete;

    void Initialize();
    void Update();
    void ToggleChat() { HandleToggleChat(); }

    WasmDisplay& GetAvatarDisplay() { return display_; }
    WasmAudioProcessor& GetAudioProcessor() {
        if (!audio_processor_) {
            audio_processor_ = std::make_unique<WasmAudioProcessor>();
        }
        return *audio_processor_;
    }

protected:
    Display* GetDisplay() override { return &display_; }
    void HandleStateChanged(DeviceState old_state, DeviceState new_state) override;
    void OnAudioVoiceProcessing(bool enable) override;
    void OnAudioResetDecoder() override;
    void OnAudioIncoming(std::unique_ptr<AudioStreamPacket> packet) override;
    void Reboot() override;
    void NotifySchedule() override;

private:
    WasmApplication();
    ~WasmApplication() override;

    void SendPcmFrame(const std::vector<int16_t>& frame);

    WasmDisplay display_;
    std::unique_ptr<WasmAudioProcessor> audio_processor_;
    DeviceState last_old_state_ = kDeviceStateUnknown;

    // VAD-gated audio transmission state
    std::deque<std::vector<int16_t>> pre_roll_buffer_;  // ring buffer, max 5 frames (~300ms)
    bool vad_gate_open_ = false;                        // true when Silero says speech active
    int post_roll_remaining_ = 0;                       // frames left to send after speech_end
};

#endif // WASM_APPLICATION_H
