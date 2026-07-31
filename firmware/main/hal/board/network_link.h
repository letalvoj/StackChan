/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <string>

namespace stackchan {

/**
 * @brief Bring up the optional WiFi link, and optionally a tailnet on top of it.
 *
 * TWO independent features behind TWO independent Kconfig symbols, deliberately
 * not one:
 *
 *   CONFIG_STACKCHAN_WIFI_ENABLE      joins the configured SSID. That alone is
 *                                     useful -- the WebSocket server already binds
 *                                     INADDR_ANY, so /ws, /debug and /debug/reset
 *                                     become reachable at the device's LAN address
 *                                     with no protocol code changes at all.
 *
 *   CONFIG_STACKCHAN_TAILSCALE_ENABLE additionally registers with a Tailscale
 *                                     tailnet via MicroLink, extending that same
 *                                     reach beyond the LAN.
 *
 * They were a single symbol until a boot loop made the cost obvious: with WiFi and
 * MicroLink welded together there is no build that answers "does WiFi work?"
 * separately from "does the VPN work?", so a crash in either one indicts both. The
 * split exists so a failure can be attributed.
 *
 * Deliberately additive, not a replacement: USB-NCM stays the trusted, no-setup
 * path it always was.
 *
 * Runs its own task and returns immediately -- association and tailnet
 * registration are both asynchronous and must not block the rest of boot.
 */
void StartNetworkLink();

/**
 * @brief State of the WiFi station this module owns.
 *
 * Deliberately separate from Hal::getWifiStatus(), which reads xiaozhi's
 * WifiManager -- a DIFFERENT object that knows nothing about StackChanWifiStation.
 * Asking it about this link returns None even while the radio is associated and
 * serving traffic, which is exactly the kind of indicator that is worse than no
 * indicator at all.
 */
enum class WifiLink {
    Disabled,      ///< Not built in, or CONFIG_STACKCHAN_WIFI_ENABLE is off.
    Disconnected,  ///< Enabled and trying, but not associated.
    Connected,     ///< Associated and holding an address.
};

WifiLink WifiLinkState();

/**
 * @brief Signal strength in dBm, or 0 when not connected. Only meaningful when
 *        WifiLinkState() == Connected.
 */
int8_t WifiRssiDbm();

/**
 * @brief The device's WiFi address, or "" if WiFi is disabled or not yet up.
 *
 * Exposed for /debug. Reporting it over the USB link is the only practical way to
 * learn the address the device is reachable at: once the app is running, TinyUSB
 * owns the USB pins and the serial console -- where this would otherwise just be
 * logged -- does not exist.
 */
std::string WifiIpAddress();

}  // namespace stackchan
