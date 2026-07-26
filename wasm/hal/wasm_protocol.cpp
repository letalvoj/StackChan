#include "wasm_protocol.h"
#include "system_info.h"
#include "wasm_board.h"
#include <settings.h>
#include <cJSON.h>
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static WasmProtocol* s_wasm_protocol_instance = nullptr;

WasmProtocol* GetWasmProtocolInstance() {
    if (!s_wasm_protocol_instance) {
        s_wasm_protocol_instance = new WasmProtocol("ws://localhost:8081/ws");
    }
    return s_wasm_protocol_instance;
}

WasmProtocol::WasmProtocol(std::string_view ws_url)
    : _ws_url(ws_url), _is_started(false), _is_audio_opened(false), _audio_tx_counter(0)
{
    s_wasm_protocol_instance = this;
    printf("[WASM_PROTOCOL] WasmProtocol instantiated with target URL: '%s'\n", _ws_url.c_str());
}

WasmProtocol::~WasmProtocol() {
    if (s_wasm_protocol_instance == this) {
        s_wasm_protocol_instance = nullptr;
    }
}

bool WasmProtocol::Start() {
    printf("[WASM_PROTOCOL] Start() invoked — connecting browser WebSocket to '%s'\n", _ws_url.c_str());
    _is_started = true;
    
    EM_ASM({
        var proto = (window.location && window.location.protocol === 'https:') ? 'wss://' : 'ws://';
        var wsUrl = (window.location && window.location.host) ? (proto + window.location.host + '/ws') : UTF8ToString($0);
        if (window._wasmProtocolWs) {
            try { window._wasmProtocolWs.close(); } catch(e) {}
        }
        console.log('[WASM_PROTOCOL:WS] Connecting dynamic WebSocket to ' + wsUrl);
        var ws = new WebSocket(wsUrl);
        ws.binaryType = 'arraybuffer';
        ws.onopen = function() {
            console.log('[WASM_PROTOCOL:WS] ✓ WebSocket connected to ' + wsUrl);
            if (typeof Module.onWasmProtocolState === 'function') {
                Module.onWasmProtocolState('Connected to ' + wsUrl);
            }
            if (Module.ccall) {
                Module.ccall('wasm_protocol_notify_connected', null, [], []);
            }
        };
        ws.onmessage = function(evt) {
            if (typeof evt.data === 'string') {
                if (typeof window.onProtocolRxJson === 'function') {
                    window.onProtocolRxJson(evt.data);
                }
                if (Module.ccall) {
                    Module.ccall('wasm_protocol_on_incoming_json', null, ['string'], [evt.data]);
                }
            } else if (evt.data instanceof ArrayBuffer) {
                if (Module.ccall && Module.HEAPU8) {
                    var byteLen = evt.data.byteLength;
                    if (!window._rxBridgePtr || window._rxBridgeCap < byteLen) {
                        if (window._rxBridgePtr) Module._free(window._rxBridgePtr);
                        window._rxBridgePtr = Module._malloc(byteLen * 2);
                        window._rxBridgeCap = byteLen * 2;
                    }
                    var srcView = new Uint8Array(evt.data);
                    Module.HEAPU8.set(srcView, window._rxBridgePtr);
                    Module.ccall('wasm_protocol_on_incoming_audio', null, ['number', 'number'], [window._rxBridgePtr, byteLen]);
                }
            }
        };
        ws.onclose = function() {
            console.log('[WASM_PROTOCOL:WS] WebSocket closed.');
            if (typeof Module.onWasmProtocolState === 'function') {
                Module.onWasmProtocolState('Disconnected');
            }
        };
        window._wasmProtocolWs = ws;
    }, _ws_url.c_str());

    return true;
}

void WasmProtocol::NotifyConnected() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON_AddStringToObject(root, "device_id", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "client_id", Board::GetInstance().GetUuid().c_str());
    cJSON_AddStringToObject(root, "token", Settings("app_config", false).GetString("token", "dev_token_8081").c_str());

    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "pcm");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", 60);
    cJSON_AddItemToObject(root, "audio_params", audio_params);

    auto json_str = cJSON_PrintUnformatted(root);
    SendText(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);

    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    if (on_connected_) {
        on_connected_();
    }
}

bool WasmProtocol::OpenAudioChannel() {
    _is_audio_opened = true;
    printf("[WASM_PROTOCOL] OpenAudioChannel() — duplex JSON/Opus stream opened on '%s'\n", _ws_url.c_str());
    
    EM_ASM({
        if (typeof Module.onWasmProtocolState === 'function') {
            Module.onWasmProtocolState('Duplex Audio/JSON Channel Open');
        }
    });

    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    return true;
}

void WasmProtocol::CloseAudioChannel(bool send_goodbye) {
    _is_audio_opened = false;
    printf("[WASM_PROTOCOL] CloseAudioChannel(send_goodbye=%s)\n", send_goodbye ? "true" : "false");
    
    EM_ASM({
        if (typeof Module.onWasmProtocolState === 'function') {
            Module.onWasmProtocolState('Audio Channel Closed');
        }
    });

    if (on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

bool WasmProtocol::IsAudioChannelOpened() const {
    return _is_audio_opened;
}

bool WasmProtocol::SendText(const std::string& text) {
    printf("[WASM_PROTOCOL:TX_JSON] Sending %zu bytes -> %s\n", text.size(), text.c_str());
    EM_ASM({
        var txt = UTF8ToString($0);
        if (window._wasmProtocolWs && window._wasmProtocolWs.readyState === 1) {
            window._wasmProtocolWs.send(txt);
        }
        if (typeof Module.onWasmProtocolTxJson === 'function') {
            Module.onWasmProtocolTxJson(txt);
        }
    }, text.c_str());
    return true;
}

bool WasmProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!packet) return false;
    _audio_tx_counter++;
    if (_audio_tx_counter % 30 == 1) {
        printf("[WASM_PROTOCOL:TX_OPUS] Transmitted audio packet #%d (%zu bytes payload, sample_rate=%d)\n",
               _audio_tx_counter, packet->payload.size(), packet->sample_rate);
    }
    EM_ASM({
        var ptr = $0;
        var len = $1;
        if (window._wasmProtocolWs && window._wasmProtocolWs.readyState === 1 && Module.HEAPU8) {
            var buf = Module.HEAPU8.slice(ptr, ptr + len).buffer;
            window._wasmProtocolWs.send(buf);
        }
    }, packet->payload.data(), packet->payload.size());
    return true;
}

void WasmProtocol::HandleIncomingJsonString(const char* json_str) {
    printf("[WASM_PROTOCOL:RX_JSON] Incoming server JSON-RPC -> %s\n", json_str);
    cJSON* root = cJSON_Parse(json_str);
    if (!root) {
        printf("[WASM_PROTOCOL] Failed to parse incoming JSON payload.\n");
        return;
    }

    auto type = cJSON_GetObjectItem(root, "type");
    if (type && type->valuestring && strcmp(type->valuestring, "hello") == 0) {
        auto sess_id = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(sess_id) && sess_id->valuestring) {
            session_id_ = sess_id->valuestring;
            printf("[WASM_PROTOCOL] Authenticated! Assigned Server Session ID: %.8s...\n", session_id_.c_str());
            EM_ASM({
                if (typeof window._onServerSessionAssigned === 'function') {
                    window._onServerSessionAssigned(UTF8ToString($0));
                }
            }, session_id_.c_str());
        }
        cJSON_Delete(root);
        return;
    }

    if (on_incoming_json_) {
        on_incoming_json_(root);
    } else {
        printf("[WASM_PROTOCOL] Incoming JSON parsed cleanly (type=%s), but no on_incoming_json_ listener registered yet.\n",
               type ? type->valuestring : "unknown");
    }
    cJSON_Delete(root);
}

extern "C" EMSCRIPTEN_KEEPALIVE void wasm_protocol_on_incoming_json(const char* json_str) {
    WasmProtocol* proto = GetWasmProtocolInstance();
    proto->HandleIncomingJsonString(json_str);
}

void WasmProtocol::DispatchIncomingAudio(const uint8_t* data, int len) {
    if (!data || len <= 0) return;
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = 16000;
    packet->payload.assign(data, data + len);
    if (on_incoming_audio_) {
        on_incoming_audio_(std::move(packet));
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void wasm_protocol_on_incoming_audio(const uint8_t* data, int len) {
    WasmProtocol* proto = GetWasmProtocolInstance();
    proto->DispatchIncomingAudio(data, len);
}

extern "C" EMSCRIPTEN_KEEPALIVE void wasm_protocol_notify_connected() {
    WasmProtocol* proto = GetWasmProtocolInstance();
    proto->NotifyConnected();
}
