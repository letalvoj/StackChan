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
#include <esp_timer.h>
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

    // Is a host connected right now? Static so UI code can ask without owning a
    // reference. Returns false when the protocol does not exist yet (early boot) or
    // when this build has no USB transport, which is the honest answer in both cases.
    static bool IsHostConnected() {
        return instance_ != nullptr && instance_->HasClient();
    }

protected:
    // Upstream declares any channel dead after 120 s without an inbound frame, and
    // IsAudioChannelOpened() is gated on it. That rule belongs to the CLIENT role: when
    // the device dials out to a cloud server, silence really does mean the session died.
    //
    // Inverted here, it is a bug with a nasty shape. The device is the server; a
    // connected agent that is simply waiting for someone to tap the robot sends
    // NOTHING, legitimately, for hours. httpd answers pings itself
    // (handle_ws_control_frames is false), so they never refresh last_incoming_time_
    // either -- see the keepalive note in Start(). After two quiet minutes the channel
    // is declared timed out, IsAudioChannelOpened() returns false forever, and a face
    // tap can no longer open the microphone. The robot lights up green and hears
    // nothing, which looks exactly like broken audio and is not.
    //
    // Liveness is already handled, and handled better, at the TCP layer: keepalive
    // probes reap a genuinely dead peer in ~35 s without punishing a healthy quiet one.
    // So this returns false and lets that mechanism own the question.
    bool IsTimeout() const override { return false; }

private:
    httpd_handle_t server_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;

    // Exactly one host at a time, enforced: adopting a new socket actively closes the
    // previous one (see WsHandler for the full reasoning). Note that this is a policy we
    // impose, not a limit httpd imposes -- esp_http_server holds max_open_sockets
    // connections happily and delivers RX from all of them. The reason to refuse is
    // device-originated output: audio and state frames have no requester to route back
    // to, so a single fd would silently redirect a live audio stream to whichever client
    // spoke most recently.
    //
    // Fan-out belongs in a host-side client, not here; that keeps "who may drive the
    // robot" out of firmware that costs a reflash to change. Run one client at a time.
    std::atomic<int> client_fd_{-1};
    std::atomic<bool> audio_channel_opened_{false};

    // httpd sends from its own task; SendAudio/SendText arrive from the main loop.
    std::mutex tx_mutex_;

    // Captured at handshake time rather than re-read from client_fd_ when the greeting
    // fires: httpd recycles socket numbers, so a close for a *previous* session can
    // clear client_fd_ in between and leave a fresh host greeted by silence.
    std::atomic<int> pending_hello_fd_{-1};
    esp_timer_handle_t hello_timer_ = nullptr;

    // Cheap traffic counters for /debug. Relaxed atomics: they are diagnostics, and
    // paying for ordering on the audio path to make a status page prettier is a bad
    // trade.
    std::atomic<uint32_t> frames_rx_{0};
    std::atomic<uint32_t> frames_tx_{0};
    std::atomic<uint32_t> send_failures_{0};
    std::atomic<int> last_send_err_{0};

    static esp_err_t WsHandler(httpd_req_t* req);

    // GET /debug -- read-only status as JSON. Registered on the same server as /ws but
    // deliberately independent of it: it never touches client_fd_, so polling it cannot
    // evict whichever client currently owns the session.
    static esp_err_t DebugHandler(httpd_req_t* req);

    // POST /debug/reset -- drop the client and force the state machine back to idle,
    // for when the device is unreachable by hand. Does not reboot; the log is evidence.
    static esp_err_t DebugResetHandler(httpd_req_t* req);
    static void OnClientClosed(httpd_handle_t hd, int sockfd);
    static void SendHelloWork(void* arg);

    void HandleTextFrame(const std::string& text);
    void HandleBinaryFrame(const uint8_t* data, size_t len);
    void ParseServerHello(const cJSON* root);
    std::string GetHelloMessage();
    bool SendFrame(httpd_ws_type_t type, const uint8_t* data, size_t len);
    void DropClient(bool notify);

    // httpd callbacks are plain C with no user pointer we control.
    static WebsocketServerProtocol* instance_;
};
