#include "wasm_audio_processor.h"
#include "wasm_application.h"
#include "wasm_log.h"
#include <emscripten.h>
#include <stdio.h>

WasmAudioProcessor::WasmAudioProcessor() {
    output_buffer_.reserve(frame_samples_);
    printf("[WASM_AUDIO] WasmAudioProcessor instantiated for continuous PCM feeding & Silero VAD signaling.\n");
}

WasmAudioProcessor::~WasmAudioProcessor() {}

void WasmAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    if (frame_duration_ms > 0) {
        frame_samples_ = frame_duration_ms * 16000 / 1000;
        output_buffer_.reserve(frame_samples_);
    }
    printf("[WASM_AUDIO] WasmAudioProcessor initialized with frame_duration=%dms (%d samples per chunk)\n", frame_duration_ms, frame_samples_);
}

void WasmAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (!is_running_ || !output_callback_) {
        return;
    }

    output_buffer_.insert(output_buffer_.end(), data.begin(), data.end());

    while (output_buffer_.size() >= (size_t)frame_samples_) {
        WASM_LOG_EVERY_N(30, "[WASM_AUDIO:FEED] Sent PCM frame (%zu samples)\n", (size_t)frame_samples_);
        if (output_buffer_.size() == (size_t)frame_samples_) {
            output_callback_(std::move(output_buffer_));
            output_buffer_.clear();
            output_buffer_.reserve(frame_samples_);
        } else {
            output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
            output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
        }
    }
}

void WasmAudioProcessor::TriggerVad(bool speaking) {
    bool current = is_speaking_.load();
    if (current != speaking) {
        is_speaking_.store(speaking);
        printf("[WASM_AUDIO:VAD] TriggerVad state change -> %s\n", speaking ? "SPEECH_START (1)" : "SPEECH_END (0)");
        if (vad_state_change_callback_) {
            vad_state_change_callback_(speaking);
        }
    }
}

void WasmAudioProcessor::Start() {
    is_running_ = true;
    printf("[WASM_AUDIO] WasmAudioProcessor Start() — Continuous audio streaming ENABLED.\n");
}

void WasmAudioProcessor::Stop() {
    is_running_ = false;
    is_speaking_ = false;
    output_buffer_.clear();
    printf("[WASM_AUDIO] WasmAudioProcessor Stop() — Audio streaming suspended & buffers flushed.\n");
}

bool WasmAudioProcessor::IsRunning() {
    return is_running_;
}

void WasmAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = std::move(callback);
}

void WasmAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = std::move(callback);
}

size_t WasmAudioProcessor::GetFeedSize() {
    return frame_samples_;
}

void WasmAudioProcessor::EnableDeviceAec(bool enable) {
}

#ifdef __EMSCRIPTEN__
extern "C" {

EMSCRIPTEN_KEEPALIVE void hal_on_vad_state_change(int speaking) {
    WasmApplication::GetInstance().GetAudioProcessor().TriggerVad(speaking != 0);
}

EMSCRIPTEN_KEEPALIVE void hal_feed_pcm_audio(const int16_t* pcm_data, int num_samples) {
    if (!pcm_data || num_samples <= 0) return;
    std::vector<int16_t> chunk(pcm_data, pcm_data + num_samples);
    WasmApplication::GetInstance().GetAudioProcessor().Feed(std::move(chunk));
}

}
#endif
