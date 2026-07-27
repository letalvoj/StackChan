#include "usb_net_board.h"

// This file is globbed into the build unconditionally, but TinyUSB's network headers
// compile to nothing unless a net class is selected. Guard the whole implementation so
// the SLIP and plain-network configurations still build.
#if CONFIG_CONNECTION_TYPE_USB_NCM

#include "esp_network.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "font_awesome.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_event.h>
#include <cJSON.h>
#include <cstring>

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
        return ESP_FAIL;
    }
    return ESP_OK;
}

void UsbNetBoard::NetifFreeRxBuffer(void* handle, void* buffer) {
    (void)handle;
    (void)buffer;
    // TinyUSB owns the RX buffer and recycles it after OnUsbPacketReceived returns.
}

esp_err_t UsbNetBoard::OnUsbPacketReceived(void* buffer, uint16_t len, void* ctx) {
    (void)ctx;
    auto self = instance_;
    if (self == nullptr || self->netif_ == nullptr) {
        return ESP_OK;
    }
    return esp_netif_receive(self->netif_, buffer, len, nullptr);
}

void UsbNetBoard::OnUsbNetInit(void* ctx) {
    (void)ctx;
    auto self = instance_;
    if (self == nullptr || self->host_attached_) {
        return;
    }
    self->host_attached_ = true;
    ESP_LOGI(TAG, "Host configured the USB network interface");
    // This is the closest signal TinyUSB gives us to "the cable is in and the host has
    // claimed the interface". The application waits on it before opening the protocol.
    self->OnNetworkEvent(NetworkEvent::Connected, "USB");
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
    ip_info.gw.addr      = esp_netif_htonl(kDeviceIp);
    ip_info.netmask.addr = esp_netif_htonl(kNetmask);

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
    net_cfg.on_init_callback = OnUsbNetInit;
    net_cfg.user_context     = this;

    err = tinyusb_net_init(TINYUSB_USBDEV_0, &net_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_net_init failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_netif_action_start(netif_, nullptr, 0, nullptr);
    ESP_LOGI(TAG, "USB network up: device 192.168.7.1, host gets 192.168.7.2");
    return true;
}

void UsbNetBoard::StartNetwork() {
    OnNetworkEvent(NetworkEvent::Connecting, "USB");

    if (!StartUsbNetwork()) {
        OnNetworkEvent(NetworkEvent::Disconnected, "USB");
        return;
    }

    // Force the WebSocket endpoint to the host side of the link. USB networking is a
    // build-time decision, so there is nothing to provision and a stale stored URL
    // from a previous WiFi setup would otherwise send the device nowhere.
    Settings settings("websocket", true);
    settings.SetString("url", CONFIG_USB_NET_WEBSOCKET_URL);
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
