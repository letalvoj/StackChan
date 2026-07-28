#include "usb_net_board.h"

// This file is globbed into the build unconditionally, but TinyUSB's network headers
// compile to nothing unless a net class is selected. Guard the whole implementation so
// the SLIP and plain-network configurations still build.
#if CONFIG_CONNECTION_TYPE_USB_NCM

#include "esp_network.h"
#include "display.h"
#include "assets/lang_config.h"
#include "font_awesome.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_event.h>
#include <cJSON.h>
#include <cstring>
#include <cinttypes>
#include <esp_timer.h>

#include <tinyusb.h>
#include <tinyusb_net.h>

#define TAG "UsbNetBoard"

UsbNetBoard* UsbNetBoard::instance_ = nullptr;

// A link-local /24 that does not collide with the usual home-router ranges
// (192.168.0/1.x) so plugging the device in cannot shadow the host's real network.
static constexpr uint32_t kDeviceIp = 0xC0A80701;  // 192.168.7.1
static constexpr uint32_t kNetmask  = 0xFFFFFF00;  // 255.255.255.0

UsbNetBoard::UsbNetBoard() {
    instance_ = this;
}

std::string UsbNetBoard::GetBoardType() {
    return "usb-ncm";
}

NetworkInterface* UsbNetBoard::GetNetwork() {
    // Identical to WifiBoard: EspNetwork speaks lwIP, which does not care which netif
    // carries the packets. This is what lets the stock WebsocketProtocol run over USB.
    static EspNetwork network;
    return &network;
}

void UsbNetBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = callback;
}

void UsbNetBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

esp_err_t UsbNetBoard::NetifTransmit(void* handle, void* buffer, size_t len) {
    (void)handle;
    // Synchronous: lwIP owns the pbuf and reuses it once we return, so the packet has
    // to be handed to TinyUSB before this call completes.
    if (tinyusb_net_send_sync(buffer, len, nullptr, pdMS_TO_TICKS(100)) != ESP_OK) {
        // Rate-limited: a wedged host would otherwise flood the console at line rate
        // and hide whatever else is going wrong.
        static uint32_t dropped = 0;
        if ((++dropped % 100) == 1) {
            ESP_LOGW(TAG, "TX drop: host not draining USB (%" PRIu32 " total)", dropped);
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

void UsbNetBoard::NetifFreeRxBuffer(void* handle, void* buffer) {
    (void)handle;
    free(buffer);
}

esp_err_t UsbNetBoard::OnUsbPacketReceived(void* buffer, uint16_t len, void* ctx) {
    (void)ctx;
    auto self = instance_;
    if (self == nullptr || self->netif_ == nullptr) {
        return ESP_OK;
    }

    // Copy before handing the packet to lwIP. esp_netif_receive wraps the pointer in a
    // zero-copy PBUF_REF and posts it asynchronously to tcpip_thread, while TinyUSB
    // calls tud_network_recv_renew() the instant this callback returns -- re-arming the
    // very same static NTB buffer for the next USB transfer. Passing the raw pointer
    // through would let the next packet overwrite one lwIP has not read yet.
    void* owned = malloc(len);
    if (owned == nullptr) {
        return ESP_ERR_NO_MEM;   // drop; the peer will retransmit
    }
    memcpy(owned, buffer, len);

    esp_err_t err = esp_netif_receive(self->netif_, owned, len, nullptr);
    if (err != ESP_OK) {
        free(owned);   // not handed over, so still ours
    }
    return err;
}

void UsbNetBoard::LogWaitingState(void* arg) {
    auto self = static_cast<UsbNetBoard*>(arg);
    if (self == nullptr || self->host_attached_) {
        return;
    }
    // The single most useful line when nothing happens: it says exactly how far the
    // link got, so "cable not detected" and "host never took a lease" are different
    // symptoms rather than the same silence.
    ESP_LOGW(TAG, "waiting for host: usb_mounted=%s dhcp_lease=%s "
                  "(host should show a new network interface; check cable is data-capable)",
             self->usb_mounted_ ? "yes" : "NO",
             self->host_attached_ ? "yes" : "NO");
}

void UsbNetBoard::HandleHostAttached(bool attached) {
    auto self = instance_;
    if (self == nullptr || self->usb_mounted_ == attached) {
        return;
    }
    self->usb_mounted_ = attached;

    if (attached) {
        ESP_LOGI(TAG, "USB cable attached; waiting for the host to take a DHCP lease");
        return;   // not "connected" until the host actually has an address
    }

    ESP_LOGW(TAG, "USB cable detached");
    self->host_attached_ = false;
    self->OnNetworkEvent(NetworkEvent::Disconnected, "USB");
}

void UsbNetBoard::OnDhcpLease(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)base;
    (void)id;
    auto self = static_cast<UsbNetBoard*>(arg);
    auto evt = static_cast<ip_event_ap_staipassigned_t*>(data);
    if (self == nullptr || evt == nullptr || evt->esp_netif != self->netif_) {
        return;   // some other netif's DHCP server
    }
    if (self->host_attached_) {
        return;   // lease renewal, not a new host
    }

    self->host_attached_ = true;
    if (self->wait_timer_ != nullptr) {
        esp_timer_stop(self->wait_timer_);
    }
    ESP_LOGI(TAG, "Host took DHCP lease " IPSTR "; USB link is usable", IP2STR(&evt->ip));

    // Put the address on the avatar's speech bubble. With no console over USB this is
    // the only way to read it off the device, and it replaces the boot-time user-agent
    // string that otherwise sits there untouched until the first conversation.
    char bubble[64];
    snprintf(bubble, sizeof(bubble), "connect to ws://%d.%d.%d.%d:%d/ws",
             (int)((kDeviceIp >> 24) & 0xFF), (int)((kDeviceIp >> 16) & 0xFF),
             (int)((kDeviceIp >> 8) & 0xFF), (int)(kDeviceIp & 0xFF),
             CONFIG_USB_NET_LISTEN_PORT);
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetChatMessage("system", bubble);
    }

    // Only now can the host actually be reached, so this -- not USB mount -- is the
    // right moment to let the application open the protocol.
    self->OnNetworkEvent(NetworkEvent::Connected, "USB");
}

// TinyUSB weak hooks. Deliberately not tud_network_init_cb(): that is declared by
// net_device.h and forwarded by esp_tinyusb, but nothing in the NCM class driver ever
// calls it -- it is an ECM/RNDIS-only hook, so relying on it meant the link never came
// up at all. Mount/unmount are driven by SET_CONFIGURATION and bus reset.
extern "C" void tud_mount_cb(void) {
    UsbNetBoard::HandleHostAttached(true);
}

extern "C" void tud_umount_cb(void) {
    UsbNetBoard::HandleHostAttached(false);
}

bool UsbNetBoard::StartUsbNetwork() {
    // Both are idempotent-ish: they return ESP_ERR_INVALID_STATE if something else
    // already initialised them, which is fine.
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return false;
    }

    static esp_netif_ip_info_t ip_info = {};
    ip_info.ip.addr      = esp_netif_htonl(kDeviceIp);
    ip_info.netmask.addr = esp_netif_htonl(kNetmask);
    // gw deliberately left at 0.0.0.0. Setting it to our own address makes the DHCP
    // server offer the device as the host's default route, and the host then tries to
    // reach the whole internet through a desk robot. This link is a private /24 between
    // exactly two peers and leads nowhere -- it must never look like a way out.
    ip_info.gw.addr = 0;

    // DHCP_SERVER, not DHCP_CLIENT: there is no router on this link, so the device
    // hands the host its address. AUTOUP brings the interface up as soon as it exists.
    esp_netif_inherent_config_t base_cfg = {
        .flags       = static_cast<esp_netif_flags_t>(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
        .mac         = {0},
        .ip_info     = &ip_info,
        .get_ip_event = 0,
        .lost_ip_event = 0,
        .if_key      = "usb",
        .if_desc     = "usb-ncm",
        .route_prio  = 10,
        .bridge_info = nullptr,
    };
    esp_netif_config_t cfg = {
        .base   = &base_cfg,
        .driver = nullptr,
        .stack  = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    netif_ = esp_netif_new(&cfg);
    if (netif_ == nullptr) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return false;
    }

    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle                 = reinterpret_cast<void*>(1),  // must be non-null
        .transmit               = NetifTransmit,
        .transmit_wrap          = nullptr,
        .driver_free_rx_buffer  = NetifFreeRxBuffer,
    };
    ESP_ERROR_CHECK(esp_netif_set_driver_config(netif_, &driver_cfg));

    // Derive the device's USB MAC from the factory MAC so the host sees a stable
    // interface across replugs rather than a new adapter each time.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    mac[0] |= 0x02;   // locally administered
    mac[0] &= 0xFE;   // unicast

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor        = nullptr,
        .string_descriptor        = nullptr,
        .external_phy             = false,
        .configuration_descriptor = nullptr,
    };
    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    tinyusb_net_config_t net_cfg = {};
    memcpy(net_cfg.mac_addr, mac, sizeof(mac));
    net_cfg.on_recv_callback = OnUsbPacketReceived;
    net_cfg.free_tx_buffer   = nullptr;
    net_cfg.user_context     = this;

    ESP_LOGI(TAG, "USB adapter MAC %02x:%02x:%02x:%02x:%02x:%02x (host sees this as its peer)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Composite: bring up a CDC-ACM interface alongside NCM and move the log console
    // onto it. Otherwise taking the PHY for networking silences the device completely,
    // and every bring-up problem has to be diagnosed by inference from the host side.
    const tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 256,
        .callback_rx = nullptr,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = nullptr,
        .callback_line_coding_changed = nullptr,
    };
    if (tusb_cdc_acm_init(&acm_cfg) == ESP_OK) {
        esp_tusb_init_console(TINYUSB_CDC_ACM_0);
        ESP_LOGI(TAG, "log console moved to USB CDC-ACM (composite with NCM)");
    } else {
        ESP_LOGE(TAG, "CDC-ACM console init failed; device will be silent over USB");
    }

    err = tinyusb_net_init(TINYUSB_USBDEV_0, &net_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_net_init failed: %s", esp_err_to_name(err));
        return false;
    }

    // Belt and braces alongside the zeroed gw: tell the DHCP server explicitly not to
    // hand out a router or a DNS server. Without this the host installs a default route
    // (and a resolver) pointing at the robot and its internet stops working -- which is
    // exactly what happened the first time this ran on a real Mac. Options must be set
    // before the server starts.
    uint8_t offer_off = 0;
    esp_netif_dhcps_option(netif_, ESP_NETIF_OP_SET,
                           ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &offer_off, sizeof(offer_off));
    esp_netif_dhcps_option(netif_, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER, &offer_off, sizeof(offer_off));

    // The DHCP lease, not USB enumeration, is what tells us the host can be reached.
    // esp_netif registers this for any netif running a DHCP server, not just SoftAP.
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                               OnDhcpLease, this));

    esp_netif_action_start(netif_, nullptr, 0, nullptr);
    ESP_LOGI(TAG, "USB network up: device 192.168.7.1, host will be offered 192.168.7.2");
    return true;
}

void UsbNetBoard::StartNetwork() {
    OnNetworkEvent(NetworkEvent::Connecting, "USB");

    if (!StartUsbNetwork()) {
        OnNetworkEvent(NetworkEvent::Disconnected, "USB");
        return;
    }

    // Nothing to provision: the device listens rather than dialling out, so it never
    // needs the host's address. The host connects to us whenever it likes.
    ESP_LOGI(TAG, "waiting for a host to connect to ws://192.168.7.1:%d/ws",
             CONFIG_USB_NET_LISTEN_PORT);

    const esp_timer_create_args_t args = {
        .callback = LogWaitingState,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "usbnet_wait",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &wait_timer_) == ESP_OK) {
        esp_timer_start_periodic(wait_timer_, 5 * 1000 * 1000);
    }
}

const char* UsbNetBoard::GetNetworkStateIcon() {
    return host_attached_ ? FONT_AWESOME_WIFI : FONT_AWESOME_WIFI_SLASH;
}

void UsbNetBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    // No radio to idle, and the USB link must stay enumerated or the host drops the
    // interface, so there is nothing to do here.
    (void)level;
}

std::string UsbNetBoard::GetBoardJson() {
    std::string json = "{\"type\":\"" + GetBoardType() + "\",";
    json += "\"name\":\"" + std::string(BOARD_NAME) + "\",";
    json += "\"transport\":\"usb-ncm\"}";
    return json;
}

std::string UsbNetBoard::GetDeviceStatusJson() {
    cJSON* root = cJSON_CreateObject();
    cJSON* network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "usb-ncm");
    cJSON_AddBoolToObject(network, "connected", host_attached_);
    cJSON_AddItemToObject(root, "network", network);

    char* str = cJSON_PrintUnformatted(root);
    std::string result(str ? str : "{}");
    if (str) {
        cJSON_free(str);
    }
    cJSON_Delete(root);
    return result;
}

#endif // CONFIG_CONNECTION_TYPE_USB_NCM
