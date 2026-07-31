/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "network_link.h"
#include <mooncake_log.h>

#if CONFIG_STACKCHAN_WIFI_ENABLE
// Every system/vendor include lives OUT HERE, before `namespace stackchan` opens.
// FreeRTOS.h declares `struct _reent` at GLOBAL scope for its TLS block; pulling it
// in from inside the namespace silently resolves that to stackchan::_reent instead,
// which fails to compile with an unhelpful "incomplete type" error pointing at
// FreeRTOS's own header, nowhere near the actual mistake.
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/utils/wifi_connect/wifi_station.h>
#if CONFIG_STACKCHAN_TAILSCALE_ENABLE
#include <microlink.h>
#endif
#endif

static const std::string_view _tag = "NetLink";

namespace stackchan {

#if CONFIG_STACKCHAN_WIFI_ENABLE

namespace {

// Outlives the task that creates it -- this link is meant to run for the life of
// the device, same as the USB server it sits alongside.
StackChanWifiStation* g_wifi = nullptr;

// The one number that actually explains a task-creation failure: FreeRTOS task
// stacks land in internal SRAM even with CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
// and CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM both on (both are, in this build) --
// those make PSRAM a fallback the ALLOCATOR may reach for, not a place FreeRTOS
// prefers by default, and a fragmented few KB of internal RAM can still refuse a
// contiguous 14 KB request even while total heap (SRAM+PSRAM combined) reads in the
// megabytes.
//
// Both free-size AND largest-free-block, because logging only the first disproved
// nothing: a task creation failed on an 8 KB stack request with 14.8 KB of internal
// RAM reported free at that exact moment. A request smaller than the total available
// memory still failed. That gap between "total free" and "one contiguous block big
// enough" is what fragmentation looks like, and largest-free-block is the number
// that settles it instead of leaving it a guess.
void LogInternalHeap(const char* when)
{
    mclog::tagInfo(_tag, "internal RAM: {} bytes free, {} bytes largest block ({})",
                   heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), when);
}

#if CONFIG_STACKCHAN_TAILSCALE_ENABLE

microlink_t* g_ml = nullptr;

const char* StateName(microlink_state_t state)
{
    switch (state) {
        case ML_STATE_IDLE:         return "IDLE";
        case ML_STATE_WIFI_WAIT:    return "LINK_WAIT";
        case ML_STATE_CONNECTING:   return "CONNECTING";
        case ML_STATE_REGISTERING:  return "REGISTERING";
        case ML_STATE_CONNECTED:    return "CONNECTED";
        case ML_STATE_RECONNECTING: return "RECONNECTING";
        case ML_STATE_ERROR:        return "ERROR";
        default:                    return "UNKNOWN";
    }
}

void OnMicrolinkState(microlink_t* ml, microlink_state_t state, void* user_data)
{
    mclog::tagInfo(_tag, "tailnet state: {}", StateName(state));
}

void OnMicrolinkPeer(microlink_t* ml, const microlink_peer_info_t* peer, void* user_data)
{
    char ip[16];
    microlink_ip_to_str(peer->vpn_ip, ip);
    mclog::tagInfo(_tag, "peer {} {} {} path={}", ip, peer->hostname,
                   peer->online ? "online" : "offline", peer->direct_path ? "direct" : "derp");
}

// One attempt: init, wire callbacks, start. On failure, tear down completely --
// microlink_destroy() signals shutdown and waits for every task that DID start to
// self-delete, then frees the handle -- so the caller always retries from a clean
// ML_STATE_IDLE, never on top of a half-started mess. That mess is real and was
// observed on hardware: wg_mgr failed its stack allocation with only 7680 contiguous
// bytes free while net_io/derp_tx/coord kept running orphaned, emitting heartbeats
// and DISCO packets that made the system LOOK alive while coord sat stuck forever at
// "Waiting for WiFi..." and nothing ever retried.
bool TryStartMicrolink()
{
    microlink_config_t cfg = {};
    cfg.auth_key            = CONFIG_STACKCHAN_TAILSCALE_AUTH_KEY;
    cfg.device_name         = CONFIG_STACKCHAN_TAILSCALE_DEVICE_NAME;
    cfg.enable_derp         = true;
    cfg.enable_stun         = true;
    cfg.enable_disco        = true;
    cfg.max_peers           = 8;

    microlink_t* ml = microlink_init(&cfg);
    if (ml == nullptr) {
        mclog::tagError(_tag, "microlink_init failed");
        return false;
    }
    microlink_set_state_callback(ml, OnMicrolinkState, nullptr);
    microlink_set_peer_callback(ml, OnMicrolinkPeer, nullptr);

    LogInternalHeap("just before microlink_start");
    esp_err_t err = microlink_start(ml);
    if (err != ESP_OK) {
        // "failed" alone was useless the first time this actually failed on real
        // hardware -- esp_err_to_name is the difference between guessing and reading.
        mclog::tagError(_tag, "microlink_start failed: {} ({})", esp_err_to_name(err),
                        static_cast<int>(err));
        LogInternalHeap("immediately after the failure");
        microlink_destroy(ml);
        return false;
    }

    g_ml = ml;
    return true;
}

// Retries forever with a fixed backoff, matching this project's own established
// idiom for a background link meant to outlive the session -- a fragmentation
// failure at one moment in boot says nothing about the next attempt 15s later, once
// other allocations elsewhere have shifted.
void MicrolinkTask(void*)
{
    LogInternalHeap("at WiFi-up");
    while (!TryStartMicrolink()) {
        mclog::tagWarn(_tag, "retrying microlink_start in 15s");
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
    vTaskDelete(nullptr);
}

// Called on every WiFi-up, which is NOT always a first connect. A fresh connect
// needs microlink_init()+start(); a reconnect on an already-registered session
// should microlink_rebind() instead, keeping the WireGuard peer state, the VPN IP
// and the DISCO table intact and coming back in ~5-10s instead of paying for a full
// re-registration and MapResponse re-download.
void StartOrRebindTailnet()
{
    if (g_ml != nullptr) {
        mclog::tagInfo(_tag, "rebinding existing tailnet session to the new link");
        esp_err_t err = microlink_rebind(g_ml);
        if (err != ESP_OK) {
            mclog::tagWarn(_tag, "rebind failed: {}", esp_err_to_name(err));
        }
        return;
    }

    // Its own task rather than inline: this runs on the shared default event loop
    // task, and blocking that for however long microlink_init()+start() take would
    // stall every other WiFi/IP event behind it.
    xTaskCreate(MicrolinkTask, "ml_start", 3072, nullptr, 3, nullptr);
}

#else  // !CONFIG_STACKCHAN_TAILSCALE_ENABLE

void StartOrRebindTailnet() {}

#endif

void OnWifiUp(const std::string& ssid)
{
    // Counted, not assumed: a fast disconnect/reconnect during association is a real
    // thing, and this firing twice before the tailnet handle is assigned would let
    // two microlink_init() calls both proceed, the second failing on a UDP port the
    // first already holds.
    static int call_count = 0;
    mclog::tagInfo(_tag, "WiFi up on '{}' at {} (call #{})", ssid, g_wifi->GetIpAddress(),
                   ++call_count);

    // Power save OFF, and the reason is latency, not throughput. The ESP32 station
    // default is WIFI_PS_MIN_MODEM: the radio sleeps between DTIM beacons and only
    // wakes to collect buffered traffic, so anything inbound waits for the next
    // beacon. Measured here as 100.7 ms and 119.1 ms round-trip against 1.1 ms over
    // USB -- not "WiFi is slow" but ping quantised to the AP's 100 ms beacon period.
    //
    // This link exists to debug the device. A debug channel with a random 0-100 ms
    // penalty on every packet is worth less than the power it saves, on hardware
    // that is plugged into USB anyway.
    g_wifi->SetPowerSaveMode(false);
    LogInternalHeap("at WiFi-up");
    StartOrRebindTailnet();
}

// WifiStation's own built-in retry is one attempt (MAX_RECONNECT_COUNT = 1 in
// wifi_station.cc) before it calls this and gives up -- fine for a foreground
// provisioning flow, wrong for a background link meant to outlive the session.
// Keep trying with a fixed backoff instead; there is nobody to show a "wrong
// password" dialog to, and the wrong move here is ever stopping.
void OnWifiFailed(const std::string& ssid)
{
    mclog::tagWarn(_tag, "join to '{}' failed, retrying in 5s", ssid);
    vTaskDelay(pdMS_TO_TICKS(5000));
    g_wifi->AddAuth(CONFIG_STACKCHAN_WIFI_SSID, CONFIG_STACKCHAN_WIFI_PASSWORD);
}

void NetworkLinkTask(void*)
{
    g_wifi = new StackChanWifiStation();
    g_wifi->OnConnected(OnWifiUp);
    g_wifi->OnConnectFailed(OnWifiFailed);
    g_wifi->Start();
    g_wifi->AddAuth(CONFIG_STACKCHAN_WIFI_SSID, CONFIG_STACKCHAN_WIFI_PASSWORD);

    // Everything from here is event-driven (WiFi events, and microlink's own tasks
    // if it is enabled); nothing left for this task to do.
    vTaskDelete(nullptr);
}

}  // namespace

void StartNetworkLink()
{
    // #if, not a ternary on the symbol: an unset Kconfig bool is UNDEFINED, not 0,
    // so referring to it in a plain C++ expression is a compile error rather than a
    // false.
#if CONFIG_STACKCHAN_TAILSCALE_ENABLE
    constexpr const char* kTailnet = " (tailnet enabled)";
#else
    constexpr const char* kTailnet = "";
#endif
    mclog::tagInfo(_tag, "starting WiFi link to '{}'{}", CONFIG_STACKCHAN_WIFI_SSID, kTailnet);
    xTaskCreate(NetworkLinkTask, "network_link", 4096, nullptr, 3, nullptr);
}

std::string WifiIpAddress()
{
    return g_wifi != nullptr ? g_wifi->GetIpAddress() : std::string();
}

#else  // !CONFIG_STACKCHAN_WIFI_ENABLE

void StartNetworkLink()
{
    // Deliberately silent: this runs on every boot of every build, and this feature
    // is off by default (see Kconfig.projbuild). A log line here would be noise for
    // everyone who has not opted in.
}

std::string WifiIpAddress()
{
    return std::string();
}

#endif

}  // namespace stackchan
