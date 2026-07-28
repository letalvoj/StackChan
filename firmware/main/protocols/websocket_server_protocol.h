/*
 * Inverted transport: the device LISTENS, the host connects to it.
 *
 * WebsocketProtocol dials out to an address it was told about -- upstream gets that
 * address from a remote provisioning service, which is the call-home shape this project
 * exists to remove. Over USB that inversion is also just wrong practically: dialling out
 * means the firmware has to know the host's IP, so a host that takes a different address
 * leaves the device connecting to nothing.
 *
 * Here the device serves on its own fixed address and the host connects whenever it
 * likes. Nothing has to be discovered or configured, and unplug/replug needs no retry
 * logic -- the host simply reconnects.
 *
 * The *wire protocol is unchanged*: same WebSocket, same JSON, same binary audio frames,
 * same hello exchange in the same order (device hello first, host replies with a
 * session_id). Only who opens the TCP connection differs, so servers and tools need a
 * connect-instead-of-listen switch and nothing more.
 */
#pragma once

#include "protocol.h"

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <atomic>
#include <mutex>
#include <string>

#define WS_SERVER_SERVER_HELLO_EVENT (1 << 0)

class WebsocketServerProtocol : public Protocol {
public:
    WebsocketServerProtocol();
    ~WebsocketServerProtocol() override;

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendText(const std::string& text) override;

    // True once a host has completed the WebSocket handshake.
    bool HasClient() const { return client_fd_ >= 0; }

private:
    httpd_handle_t server_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;

    // Exactly one host at a time. A second connection replaces the first rather than
    // being queued: two controllers driving one robot is never what anyone wanted.
    std::atomic<int> client_fd_{-1};
    std::atomic<bool> audio_channel_opened_{false};

    // httpd sends from its own task; SendAudio/SendText arrive from the main loop.
    std::mutex tx_mutex_;

    static esp_err_t WsHandler(httpd_req_t* req);
    static void OnClientClosed(httpd_handle_t hd, int sockfd);

    void HandleTextFrame(const std::string& text);
    void HandleBinaryFrame(const uint8_t* data, size_t len);
    void ParseServerHello(const cJSON* root);
    std::string GetHelloMessage();
    bool SendFrame(httpd_ws_type_t type, const uint8_t* data, size_t len);
    void DropClient(bool notify);

    // httpd callbacks are plain C with no user pointer we control.
    static WebsocketServerProtocol* instance_;
};
