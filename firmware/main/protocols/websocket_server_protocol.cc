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

#include <esp_log.h>
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
    bool was_open = audio_channel_opened_.exchange(false);
    if (notify && was_open && on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
    if (notify && on_disconnected_) {
        on_disconnected_();
    }
}

esp_err_t WebsocketServerProtocol::WsHandler(httpd_req_t* req) {
    auto self = static_cast<WebsocketServerProtocol*>(req->user_ctx);
    if (self == nullptr) {
        return ESP_FAIL;
    }

    // Adopt the socket on EVERY invocation, not just the handshake. The HTTP_GET branch
    // below turned out never to run on this IDF version, so client_fd_ stayed -1, every
    // SendFrame() bailed at its first check, and the device could receive but never
    // reply -- no hello, no MCP results. Capturing it here makes the send path depend on
    // having seen any frame at all rather than on handshake-dispatch semantics.
    int active_fd = httpd_req_to_sockfd(req);
    if (active_fd >= 0 && self->client_fd_.exchange(active_fd) != active_fd) {
        ESP_LOGI(TAG, "adopted host socket fd=%d", active_fd);
        // Greet from here too, for the same reason: the handshake branch never runs, so
        // arming the timer there meant the hello was never sent either. Deferred rather
        // than sent inline because httpd still owns the session inside the handler.
        self->audio_channel_opened_ = false;
        xEventGroupClearBits(self->event_group_, WS_SERVER_SERVER_HELLO_EVENT);
        self->pending_hello_fd_ = active_fd;
        esp_timer_stop(self->hello_timer_);
        esp_timer_start_once(self->hello_timer_, 20 * 1000);
        if (self->on_connected_) {
            self->on_connected_();
        }
    }

    if (req->method == HTTP_GET) {
        // The WebSocket handshake just completed; no payload yet.
        int fd = httpd_req_to_sockfd(req);
        self->audio_channel_opened_ = false;
        int previous = self->client_fd_.exchange(fd);
        if (previous >= 0 && previous != fd) {
            ESP_LOGW(TAG, "replacing host on fd=%d with fd=%d", previous, fd);
        }
        ESP_LOGI(TAG, "host connected (fd=%d)", fd);
        if (self->on_connected_) {
            self->on_connected_();
        }

        // Greet as soon as the host connects, so a tool that attaches has something to
        // identify without waiting for someone to press talk.
        //
        // Queued, NOT sent inline: httpd still owns this session inside the handshake
        // handler, so httpd_ws_send_frame_async() fails here -- and SendFrame's error
        // path would then drop the very client we just accepted. httpd_queue_work runs
        // it on the server task once the handler has returned.
        xEventGroupClearBits(self->event_group_, WS_SERVER_SERVER_HELLO_EVENT);

        // Deferred by a one-shot timer rather than httpd_queue_work(). Both avoid
        // sending inline (httpd still owns the session here, so the async send fails),
        // but queue_work routes through httpd's control socket on the loopback netif,
        // which adds a dependency this path does not need. The esp_timer task is always
        // there and cannot be starved by the server's own socket budget.
        self->pending_hello_fd_ = fd;
        esp_timer_start_once(self->hello_timer_, 20 * 1000);   // 20 ms
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
