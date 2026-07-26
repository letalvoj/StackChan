#ifndef WASM_AUDIO_PROCESSOR_H
#define WASM_AUDIO_PROCESSOR_H

#include <vector>
#include <functional>
#include <atomic>
#include <audio/audio_processor.h>

class WasmAudioProcessor : public AudioProcessor {
public:
    WasmAudioProcessor();
    ~WasmAudioProcessor() override;

    void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;

    void TriggerVad(bool speaking);

private:
    int frame_samples_ = 960; // 60ms of 16kHz mono PCM by default
    std::vector<int16_t> output_buffer_;
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    std::atomic<bool> is_running_{false};
    std::atomic<bool> is_speaking_{false};
};

#endif // WASM_AUDIO_PROCESSOR_H
