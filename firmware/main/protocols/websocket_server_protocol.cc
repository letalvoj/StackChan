// Globbed into the build unconditionally, but this transport only exists under USB
// networking -- and esp_http_server's WebSocket API is compiled out unless
// CONFIG_HTTPD_WS_SUPPORT is set, which only that configuration enables. Guard the
// whole file so the SLIP and plain-network builds still compile.
#include "sdkconfig.h"
#if CONFIG_CONNECTION_TYPE_USB_NCM

#include "websocket_server_protocol.h"

#include "board.h"
#include "system_info.h"
#include "audio/audio_service.h"
#include "assets/lang_config.h"
#include "application.h"
#include "device_state_machine.h"
#include "hal/board/hal_bridge.h"
#include "hal/board/network_link.h"

#include <esp_log.h>
#include <esp_system.h>
#include <cstdio>
#include <cstring>
#include <chrono>

#define TAG "WsServerProto"

// A frame larger than this is not something this device produces or consumes; refusing
// it keeps a hostile or confused host from driving us into an allocation failure.
static constexpr size_t kMaxFrameBytes = 64 * 1024;

WebsocketServerProtocol* WebsocketServerProtocol::instance_ = nullptr;

WebsocketServerProtocol::WebsocketServerProtocol() {
    event_group_ = xEventGroupCreate();
    instance_ = this;
}

WebsocketServerProtocol::~WebsocketServerProtocol() {
    instance_ = nullptr;
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool WebsocketServerProtocol::Start() {
    if (server_ != nullptr) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = CONFIG_USB_NET_LISTEN_PORT;
    config.ctrl_port        = CONFIG_USB_NET_LISTEN_PORT + 1;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    // Audio frames arrive continuously once a turn starts; the default 5 s recv timeout
    // would tear down an idle-but-healthy connection between turns.
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 10;
    config.close_fn          = OnClientClosed;
    config.stack_size        = 8192;   // cJSON + the app's JSON handler run on this task

    // TCP keepalive: the device's own detector for peers that vanished without a FIN.
    //
    // This is the unplugged-laptop and dead-SSH-tunnel case. Neither end sees a close,
    // so without this the device holds client_fd_ forever, believing someone is there --
    // and since we allow exactly one client, that ghost blocks nothing but confuses
    // everything, right up until a reconnecting client evicts a descriptor nobody owns.
    //
    // Done at the TCP layer rather than by timing WebSocket traffic, because
    // handle_ws_control_frames is false: httpd answers pings itself without invoking our
    // handler, so ping activity never refreshes last_incoming_time_ and any silence
    // timer we wrote would reap perfectly healthy idle clients.
    //
    // ~35 s to notice: 15 s idle, then 3 probes 5 s apart. Slow enough to be free,
    // fast enough that a reconnect is not left waiting.
    config.keep_alive_enable   = true;
    config.keep_alive_idle     = 15;
    config.keep_alive_interval = 5;
    config.keep_alive_count    = 3;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return false;
    }

    const httpd_uri_t ws_uri = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = WsHandler,
        .user_ctx     = this,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol    = nullptr,
    };
    err = httpd_register_uri_handler(server_, &ws_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler failed: %s", esp_err_to_name(err));
        httpd_stop(server_);
        server_ = nullptr;
        return false;
    }

    const httpd_uri_t debug_uri = {
        .uri          = "/debug",
        .method       = HTTP_GET,
        .handler      = DebugHandler,
        .user_ctx     = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol    = nullptr,
    };
    if (httpd_register_uri_handler(server_, &debug_uri) != ESP_OK) {
        // Not fatal: losing the status page must never cost us the protocol.
        ESP_LOGW(TAG, "could not register /debug");
    }

    const httpd_uri_t reset_uri = {
        .uri          = "/debug/reset",
        .method       = HTTP_POST,
        .handler      = DebugResetHandler,
        .user_ctx     = this,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol    = nullptr,
    };
    if (httpd_register_uri_handler(server_, &reset_uri) != ESP_OK) {
        ESP_LOGW(TAG, "could not register /debug/reset");
    }

    const esp_timer_create_args_t hello_args = {
        .callback = SendHelloWork,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ws_hello",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&hello_args, &hello_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "could not create the hello timer");
        httpd_stop(server_);
        server_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "listening for a host on ws://<device>:%d/ws", CONFIG_USB_NET_LISTEN_PORT);
    return true;
}

void WebsocketServerProtocol::OnClientClosed(httpd_handle_t hd, int sockfd) {
    auto self = instance_;
    // httpd owns the socket lifecycle; we must still close it or the slot leaks.
    close(sockfd);
    if (self == nullptr || self->client_fd_.load() != sockfd) {
        return;
    }
    ESP_LOGW(TAG, "host disconnected (fd=%d)", sockfd);
    self->DropClient(true);
}

void WebsocketServerProtocol::SendHelloWork(void* arg) {
    // The fd is captured at queue time rather than re-read from client_fd_: httpd
    // recycles socket numbers, so a close_fn for a *previous* session can arrive on the
    // same fd between the handshake and this work item running, clear client_fd_, and
    // leave the freshly connected host greeted by silence.
    auto self = static_cast<WebsocketServerProtocol*>(arg);
    if (self == nullptr) {
        return;
    }
    int fd = self->pending_hello_fd_.load();
    if (fd < 0) {
        return;
    }
    std::string hello = self->GetHelloMessage();

    httpd_ws_frame_t frame = {};
    frame.final   = true;
    frame.type    = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(hello.data()));
    frame.len     = hello.size();

    esp_err_t err;
    {
        std::lock_guard<std::mutex> lock(self->tx_mutex_);
        err = httpd_ws_send_frame_async(self->server_, fd, &frame);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to greet host on fd=%d: %s", fd, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "greeted host on fd=%d", fd);
    }
}

void WebsocketServerProtocol::DropClient(bool notify) {
    client_fd_ = -1;
    audio_channel_opened_.exchange(false);

    // Always restore idle, NOT only when an audio channel had been opened.
    //
    // The device's state is driven by protocol messages, and a client can move it to
    // listening or speaking with `tts`/`listen` frames alone -- no audio channel required.
    // Gating cleanup on was_open meant such a session was never unwound: disconnect left
    // the robot parked in listening with a green LED, and because the launcher gates the
    // home indicator and status bar on is_xiaozhi_idle(), the screen also stopped
    // responding to touch. It reads exactly like a freeze, and the only way out was a
    // power cycle.
    //
    // The handler is idempotent (it sets idle and clears the chat message), so calling it
    // for a session that never opened audio costs nothing and closes the hole for every
    // way a client can leave: clean close, eviction, timeout, or a yanked cable.
    if (notify && on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
    if (notify && on_disconnected_) {
        on_disconnected_();
    }
}

uint32_t WebsocketServerProtocol::HostPeerAddressV4() {
    if (instance_ == nullptr) {
        return 0;
    }
    int fd = instance_->client_fd_.load();
    if (fd < 0) {
        return 0;
    }
    // Asked of the socket rather than remembered from the handshake: the answer cannot
    // go stale, and there is no second copy of the truth to drift.
    struct sockaddr_in6 addr = {};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    if (addr.sin6_family == AF_INET) {
        auto* v4 = reinterpret_cast<struct sockaddr_in*>(&addr);
        return ntohl(v4->sin_addr.s_addr);
    }
    if (addr.sin6_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&addr.sin6_addr)) {
        // lwIP hands back v4-mapped addresses (::ffff:a.b.c.d) on a dual-stack socket,
        // so a client that is plainly IPv4 still arrives as AF_INET6. Missing this is
        // how the indicator would silently read "unknown" for every real connection.
        uint32_t mapped;
        memcpy(&mapped, &addr.sin6_addr.un.u32_addr[3], sizeof(mapped));
        return ntohl(mapped);
    }
    return 0;
}

void WebsocketServerProtocol::DropRemoteClient() {
    if (instance_ == nullptr) {
        return;
    }
    const int fd = instance_->client_fd_.load();
    if (fd < 0) {
        return;
    }
    const uint32_t peer = HostPeerAddressV4();
    if ((peer & 0xFFFFFF00u) == 0xC0A80700u) {
        return;   // 192.168.7.0/24 -- came in over the cable, untouched by a radio drop
    }
    ESP_LOGW(TAG, "network went away; dropping remote host on fd=%d", fd);
    // Close the SOCKET, not just the bookkeeping. Clearing client_fd_ alone would let
    // httpd keep a dead session in its table and, worse, leave the slot occupied so a
    // reconnect after the radio returns is refused.
    instance_->DropClient(true);
    esp_err_t closed = httpd_sess_trigger_close(instance_->server_, fd);
    if (closed != ESP_OK) {
        ESP_LOGW(TAG, "could not close fd=%d: %s", fd, esp_err_to_name(closed));
    }
}

namespace {

// lwIP names its task "tiT". Looked up by name rather than cached at startup
// because this transport does not create it and has no handle to it; the lookup is
// cheap and /debug is polled by hand, not in a hot path. Returns 0 if the task
// cannot be found, which reads as "unknown", not as "no stack left".
unsigned long TcpipTaskStackFreeMin() {
    TaskHandle_t tit = xTaskGetHandle("tiT");
    return tit != nullptr ? (unsigned long)uxTaskGetStackHighWaterMark(tit) : 0;
}

}  // namespace

esp_err_t WebsocketServerProtocol::DebugHandler(httpd_req_t* req) {
    auto self = static_cast<WebsocketServerProtocol*>(req->user_ctx);

    // Everything here is read-only and none of it touches client_fd_, so a monitor can
    // poll this endpoint continuously while a gateway holds the session. That property
    // is the whole point: the tooling that tells you why the session is broken must not
    // be the thing that breaks it.
    int fd = (self != nullptr) ? self->client_fd_.load() : -1;
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    char body[640];
    int n = snprintf(body, sizeof(body),
        "{"
        // No %llu here on purpose: CONFIG_LIBC_NEWLIB_NANO_FORMAT drops long long
        // support, so the conversion consumes the wrong number of vararg bytes and every
        // later %s reads a misaligned pointer -- which dereferences garbage and panics.
        // Seconds as a plain long covers 68 years of uptime.
        "\"uptime_s\":%lu,"
        "\"version\":\"%s\","
        "\"device_state\":\"%s\","
        "\"xiaozhi_ready\":%s,"
        "\"client_fd\":%d,"
        "\"has_client\":%s,"
        "\"audio_channel_open\":%s,"
        "\"frames_rx\":%lu,"
        "\"frames_tx\":%lu,"
        "\"send_failures\":%lu,"
        "\"last_send_err\":\"%s\","
        "\"heap_free\":%lu,"
        "\"heap_min\":%lu,"
        // Empty until WiFi associates, and empty forever if WiFi is not enabled.
        // Reported here because there is nowhere else to read it: once the app is
        // running TinyUSB owns the USB pins, so the serial console this would
        // otherwise be logged to does not exist. Without it, finding the device on
        // the LAN means sweeping the subnet.
        "\"wifi_ip\":\"%s\","
        // Lowest the lwIP task's free stack has EVER been, in bytes -- not its
        // current depth. This exists because 3072 (ESP-IDF's stock default) silently
        // overflowed once WiFi joined USB-NCM on the same task, and the only signal
        // was an intermittent post-boot crash. Sizing this stack by argument is
        // guesswork; sizing it by watching this number approach zero is not.
        //
        // Watch it in particular when adding a netif: WireGuard runs its handshake
        // crypto (X25519, blake2s HMAC) in lwIP's context, on this stack.
        "\"tcpip_stack_free_min\":%lu"
        "}",
        (unsigned long)(esp_timer_get_time() / 1000000),
        FIRMWARE_VERSION,
        DeviceStateMachine::GetStateName(state),
        // The flag behind tap-to-talk. If this is false, face taps are being dropped
        // before they ever reach the application -- which cost a debugging session to
        // work out precisely because nothing surfaced it.
        hal_bridge::is_xiaozhi_ready() ? "true" : "false",
        fd,
        fd >= 0 ? "true" : "false",
        (self != nullptr && self->audio_channel_opened_) ? "true" : "false",
        (unsigned long)(self ? self->frames_rx_.load() : 0),
        (unsigned long)(self ? self->frames_tx_.load() : 0),
        (unsigned long)(self ? self->send_failures_.load() : 0),
        esp_err_to_name(self ? self->last_send_err_.load() : 0),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)esp_get_minimum_free_heap_size(),
        stackchan::WifiIpAddress().c_str(),
        (unsigned long)TcpipTaskStackFreeMin());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, body, n > 0 ? n : 0);
}

esp_err_t WebsocketServerProtocol::DebugResetHandler(httpd_req_t* req) {
    // Recovery of last resort, over HTTP rather than by hand. When the device is
    // mounted somewhere awkward and reached through an SSH tunnel, "just power-cycle
    // it" is an expensive instruction; this makes an unresponsive robot fixable from
    // the same shell that noticed the problem.
    //
    // Drops any client and forces the state machine back to idle. Deliberately does
    // NOT reboot: rebooting loses the log and the uptime, which are usually the only
    // evidence of whatever went wrong.
    auto self = static_cast<WebsocketServerProtocol*>(req->user_ctx);
    auto& app = Application::GetInstance();
    auto before = app.GetDeviceState();

    ESP_LOGW(TAG, "/debug/reset while %s", DeviceStateMachine::GetStateName(before));
    if (self != nullptr) {
        int fd = self->client_fd_.load();
        if (fd >= 0) {
            // Close the SOCKET, not just our bookkeeping. DropClient() alone clears
            // client_fd_ and notifies the app, but leaves the TCP connection open --
            // which manufactures precisely the ghost we are trying to eliminate: the
            // device believes nobody is connected while the client sees a healthy
            // socket and waits forever. A client that is being reset must be told.
            self->DropClient(true);
            esp_err_t closed = httpd_sess_trigger_close(self->server_, fd);
            if (closed != ESP_OK) {
                ESP_LOGW(TAG, "reset: could not close fd=%d: %s", fd,
                         esp_err_to_name(closed));
            }
        }
    }
    app.SetDeviceState(kDeviceStateIdle);

    char body[160];
    int n = snprintf(body, sizeof(body),
                     "{\"was\":\"%s\",\"now\":\"%s\"}",
                     DeviceStateMachine::GetStateName(before),
                     DeviceStateMachine::GetStateName(app.GetDeviceState()));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n > 0 ? n : 0);
}

esp_err_t WebsocketServerProtocol::WsHandler(httpd_req_t* req) {
    auto self = static_cast<WebsocketServerProtocol*>(req->user_ctx);
    if (self == nullptr) {
        return ESP_FAIL;
    }

    // ---- Adopting the client socket -------------------------------------------------
    //
    // Adopt on EVERY invocation, not just the handshake. The obvious place for this is an
    // `if (req->method == HTTP_GET)` branch -- that is where esp_http_server is documented
    // to hand you the completed WebSocket upgrade -- but on this IDF version that branch
    // never runs for our URI. Doing it there meant client_fd_ stayed -1 forever, every
    // SendFrame() bailed at its first check, and the device could receive perfectly while
    // being structurally unable to reply: no hello, no MCP results, no audio. Every
    // "the device ignores me" symptom traced back to this one line.
    //
    // Keying off "we have seen a frame on this socket" instead of "we saw the handshake"
    // makes the send path independent of handshake-dispatch semantics, which have already
    // proven to vary between IDF releases.
    int active_fd = httpd_req_to_sockfd(req);
    int previous_fd = (active_fd >= 0) ? self->client_fd_.exchange(active_fd) : -1;
    if (active_fd >= 0 && previous_fd != active_fd) {
        // ---- Why exactly one client, enforced rather than assumed --------------------
        //
        // esp_http_server happily holds max_open_sockets connections at once, and RX is
        // per-socket, so several hosts CAN talk to us simultaneously. We deliberately do
        // not allow it, and it is worth being precise about why -- because an earlier
        // version of this code left it to chance and appeared to work.
        //
        // The load-bearing problem is not request/response traffic. MCP replies happen to
        // land on the right socket under turn-taking load, simply because the requester is
        // whoever most recently touched this handler. The problem is device-ORIGINATED
        // output: audio frames, TTS state, emotion changes. Those have no requester to
        // reply to, so with a single fd they follow whoever spoke last -- an assistant
        // streaming Opus would have its frames silently redirected mid-utterance the
        // instant some other tool polled get_device_status. That is a data race dressed
        // up as a routing rule, and it fails in the least debuggable way possible.
        //
        // Real fan-out (broadcast to httpd_get_client_list(), replies routed per request)
        // is maybe two hours of work, but it does not belong here. Deciding WHO may drive
        // the servos and WHO hears the audio is policy, and policy baked into firmware
        // costs a reflash to change. Constrained devices conventionally stay single-homed
        // for exactly this reason -- MQTT is the canonical shape, and upstream xiaozhi is
        // already built that way, holding one connection to one endpoint.
        //
        // So: the newest connection wins and the previous one is actively CLOSED. Being
        // hung up on is a clean, observable failure that any client library reports.
        // Leaving the old socket open but deaf -- what the accidental version did -- is
        // the same outcome with none of the feedback.
        if (previous_fd >= 0) {
            // Evict before greeting the newcomer, so the logs read in the order things
            // actually happened. client_fd_ already points at active_fd, so the close_fn
            // this triggers takes OnClientClosed's "not the current client" early return
            // and cannot tear down the session we are in the middle of establishing.
            ESP_LOGW(TAG, "evicting host on fd=%d in favour of fd=%d (single client by design)",
                     previous_fd, active_fd);
            esp_err_t closed = httpd_sess_trigger_close(self->server_, previous_fd);
            if (closed != ESP_OK) {
                // Not fatal: the old socket stays open but stops receiving device output,
                // which is the pre-eviction behaviour. Worth logging loudly because it is
                // the difference between a client seeing a clean hangup and a silent one.
                ESP_LOGW(TAG, "could not close fd=%d: %s", previous_fd, esp_err_to_name(closed));
            }
        }
        ESP_LOGI(TAG, "adopted host socket fd=%d", active_fd);

        // A newly adopted socket is a new session: drop any audio channel state belonging
        // to the previous one, or a half-open channel leaks across clients.
        self->audio_channel_opened_ = false;
        xEventGroupClearBits(self->event_group_, WS_SERVER_SERVER_HELLO_EVENT);

        // Greet immediately, so a tool that attaches learns who we are without waiting for
        // someone to press talk.
        //
        // Deferred by a one-shot timer rather than sent inline: httpd still owns this
        // session inside the handler, so httpd_ws_send_frame_async() fails here, and
        // SendFrame's error path would then drop the very client we just accepted.
        // A timer rather than httpd_queue_work() because queue_work routes through httpd's
        // control socket on the loopback netif -- a dependency this path does not need,
        // and one that can be starved by the server's own socket budget. The esp_timer
        // task is always there.
        self->pending_hello_fd_ = active_fd;
        esp_timer_stop(self->hello_timer_);
        esp_timer_start_once(self->hello_timer_, 20 * 1000);   // 20 ms
        if (self->on_connected_) {
            self->on_connected_();
        }
    }

    if (req->method == HTTP_GET) {
        // The WebSocket upgrade itself: no payload to read, so returning here is required
        // -- falling through would call httpd_ws_recv_frame() on a handshake request.
        //
        // On this IDF version this branch is never reached (see above); it is kept because
        // it is the documented contract, and a future IDF that does dispatch the handshake
        // here must not land in the receive path. The socket has already been adopted
        // above either way, so nothing session-related lives in here anymore.
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;

    // Two-step receive: first with len 0 to learn the size, then with a buffer.
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_ws_recv_frame(size) failed: %s", esp_err_to_name(err));
        return err;
    }
    if (frame.len == 0) {
        return ESP_OK;
    }
    if (frame.len > kMaxFrameBytes) {
        ESP_LOGE(TAG, "refusing oversized frame (%u bytes)", (unsigned)frame.len);
        return ESP_FAIL;
    }

    // +1 so a text payload can be NUL-terminated in place.
    auto buf = static_cast<uint8_t*>(calloc(1, frame.len + 1));
    if (buf == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_ws_recv_frame(payload) failed: %s", esp_err_to_name(err));
        free(buf);
        return err;
    }

    self->last_incoming_time_ = std::chrono::steady_clock::now();
    self->frames_rx_.fetch_add(1, std::memory_order_relaxed);
    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        self->HandleTextFrame(std::string(reinterpret_cast<char*>(buf), frame.len));
    } else if (frame.type == HTTPD_WS_TYPE_BINARY) {
        self->HandleBinaryFrame(buf, frame.len);
    }
    free(buf);
    return ESP_OK;
}

void WebsocketServerProtocol::HandleTextFrame(const std::string& text) {
    // Explicit length: a stray NUL must not silently truncate the message.
    cJSON* root = cJSON_ParseWithLength(text.data(), text.size());
    if (root == nullptr) {
        ESP_LOGW(TAG, "dropped unparseable JSON frame (%zu bytes)", text.size());
        return;
    }

    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (type != nullptr && cJSON_IsString(type) && strcmp(type->valuestring, "hello") == 0) {
        ParseServerHello(root);
    } else if (on_incoming_json_) {
        on_incoming_json_(root);
    }
    cJSON_Delete(root);
}

void WebsocketServerProtocol::HandleBinaryFrame(const uint8_t* data, size_t len) {
    if (!on_incoming_audio_) {
        return;
    }
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate    = server_sample_rate_;
    packet->frame_duration = server_frame_duration_;
    packet->payload.resize(len);
    memcpy(packet->payload.data(), data, len);
    on_incoming_audio_(std::move(packet));
}

void WebsocketServerProtocol::ParseServerHello(const cJSON* root) {
    cJSON* transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || !cJSON_IsString(transport) ||
        strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "host hello has wrong transport; ignoring");
        return;
    }

    cJSON* session_id = cJSON_GetObjectItem(root, "session_id");
    if (session_id != nullptr && cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
    }

    cJSON* audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        cJSON* sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (sample_rate != nullptr && cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        cJSON* frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (frame_duration != nullptr && cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    ESP_LOGI(TAG, "session %s established (%d Hz / %d ms)",
             session_id_.c_str(), server_sample_rate_, server_frame_duration_);
    xEventGroupSetBits(event_group_, WS_SERVER_SERVER_HELLO_EVENT);
}

bool WebsocketServerProtocol::SendFrame(httpd_ws_type_t type, const uint8_t* data, size_t len) {
    int fd = client_fd_.load();
    if (server_ == nullptr || fd < 0) {
        return false;
    }

    httpd_ws_frame_t frame = {};
    frame.final   = true;
    frame.type    = type;
    frame.payload = const_cast<uint8_t*>(data);
    frame.len     = len;

    esp_err_t err;
    {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        err = httpd_ws_send_frame_async(server_, fd, &frame);
    }
    if (err == ESP_OK) {
        frames_tx_.fetch_add(1, std::memory_order_relaxed);
    } else {
        send_failures_.fetch_add(1, std::memory_order_relaxed);
        last_send_err_.store(err, std::memory_order_relaxed);
    }
    if (err != ESP_OK) {
        // A send failure on an established socket means the host is gone; httpd's
        // close_fn does not always fire promptly enough to notice on its own.
        //
        // Notify *outside* the lock: DropClient runs application callbacks, and
        // tx_mutex_ is not recursive, so any handler that sends would deadlock.
        ESP_LOGW(TAG, "send failed (%s); dropping host", esp_err_to_name(err));
        DropClient(true);
        return false;
    }
    return true;
}

bool WebsocketServerProtocol::SendText(const std::string& text) {
    if (!SendFrame(HTTPD_WS_TYPE_TEXT,
                   reinterpret_cast<const uint8_t*>(text.data()), text.size())) {
        return false;
    }
    return true;
}

bool WebsocketServerProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!IsAudioChannelOpened()) {
        return false;
    }
    return SendFrame(HTTPD_WS_TYPE_BINARY, packet->payload.data(), packet->payload.size());
}

bool WebsocketServerProtocol::OpenAudioChannel() {
    error_occurred_ = false;
    if (audio_channel_opened_) {
        return true;
    }
    if (client_fd_.load() < 0) {
        // Not an error worth alerting on: it just means nobody has connected yet.
        ESP_LOGW(TAG, "no host connected; cannot open the audio channel");
        return false;
    }

    // The hello was already sent when the host connected; normally its reply has long
    // since arrived and this returns at once. Do not consume the bit -- a second turn
    // on the same connection must not have to re-handshake.
    EventBits_t bits = xEventGroupWaitBits(event_group_, WS_SERVER_SERVER_HELLO_EVENT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WS_SERVER_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "host did not answer the hello within 10s");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    audio_channel_opened_ = true;
    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    return true;
}

void WebsocketServerProtocol::CloseAudioChannel(bool send_goodbye) {
    if (!audio_channel_opened_) {
        return;
    }
    if (send_goodbye && client_fd_.load() >= 0) {
        SendText("{\"session_id\":\"" + session_id_ + "\",\"type\":\"goodbye\"}");
    }
    audio_channel_opened_ = false;
    if (on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
    // The socket deliberately stays open: the host owns its lifetime here, and holding
    // it lets the next turn start without another handshake.
}

bool WebsocketServerProtocol::IsAudioChannelOpened() const {
    // IsTimeout() is overridden to false for this transport -- see the header. Kept in
    // the expression so this line still matches every other protocol implementation.
    return client_fd_.load() >= 0 && audio_channel_opened_ && !error_occurred_ && !IsTimeout();
}

std::string WebsocketServerProtocol::GetHelloMessage() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 1);

    cJSON* features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);

    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON_AddStringToObject(root, "device_id", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "client_id", Board::GetInstance().GetUuid().c_str());

    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);

    char* str = cJSON_PrintUnformatted(root);
    std::string result(str != nullptr
                       ? str
                       : "{\"type\":\"hello\",\"transport\":\"websocket\",\"version\":1}");
    if (str != nullptr) {
        cJSON_free(str);
    }
    cJSON_Delete(root);
    return result;
}

#endif // CONFIG_CONNECTION_TYPE_USB_NCM
