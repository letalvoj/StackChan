#ifndef WASM_PROTOCOL_H
#define WASM_PROTOCOL_H

#include <protocols/protocol.h>
#include <string>
#include <memory>

class WasmProtocol : public Protocol {
public:
    WasmProtocol(std::string_view ws_url);
    ~WasmProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    // Real incoming WebSocket handlers
    void HandleIncomingJsonString(const char* json_str);
    void DispatchIncomingAudio(const uint8_t* data, int len);
    void NotifyConnected();
    bool SendText(const std::string& text) override;

private:
    std::string _ws_url;
    bool _is_started;
    bool _is_audio_opened;
    int _audio_tx_counter;
};

// Singleton accessor for interactive test bindings
WasmProtocol* GetWasmProtocolInstance();

#endif // WASM_PROTOCOL_H
