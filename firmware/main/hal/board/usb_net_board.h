/*
 * Board base class for devices whose only network is a USB CDC-NCM link.
 *
 * The device enumerates as a USB *network adapter* rather than a serial port, so the
 * host gets an ordinary network interface (usb0 on Linux) and everything above the
 * link -- WebsocketProtocol, TLS, MCP -- runs unmodified. That is the whole point:
 * there is no bespoke wire format to implement, and therefore none to get wrong.
 *
 * Addressing is deliberately fixed and self-contained: the device runs a DHCP server
 * on the link, takes 192.168.7.1, and the host lands on 192.168.7.2. No provisioning
 * step, no router, no internet.
 */
#pragma once

#include "board.h"
#include <esp_netif.h>
#include <string>

class UsbNetBoard : public Board {
public:
    UsbNetBoard();
    virtual ~UsbNetBoard() = default;

    std::string GetBoardType() override;
    void StartNetwork() override;
    NetworkInterface* GetNetwork() override;
    void SetNetworkEventCallback(NetworkEventCallback callback) override;
    const char* GetNetworkStateIcon() override;
    void SetPowerSaveLevel(PowerSaveLevel level) override;
    std::string GetBoardJson() override;
    std::string GetDeviceStatusJson() override;

    // Called from TinyUSB's tud_mount_cb/tud_umount_cb weak hooks, which are plain C
    // globals. USB attach/detach is the only reliable cable-presence signal available.
    static void HandleHostAttached(bool attached);

protected:
    NetworkEventCallback network_event_callback_ = nullptr;
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

private:
    esp_netif_t* netif_ = nullptr;
    bool usb_mounted_ = false;
    bool host_attached_ = false;

    bool StartUsbNetwork();

    // TinyUSB and esp_netif both take plain C callbacks with no user pointer we can
    // rely on, so the active board is resolved through this.
    static UsbNetBoard* instance_;

    static esp_err_t NetifTransmit(void* handle, void* buffer, size_t len);
    static void NetifFreeRxBuffer(void* handle, void* buffer);
    static esp_err_t OnUsbPacketReceived(void* buffer, uint16_t len, void* ctx);
    static void OnDhcpLease(void* arg, esp_event_base_t base, int32_t id, void* data);
};
