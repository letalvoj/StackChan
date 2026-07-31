/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <hal/board/log_ring.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // DISABLED while the Tailscale crash is being isolated. The ring buffer lives in
    // PSRAM and is written on every log line, which makes it a suspect for the silent
    // reboots (PSRAM is unreachable while a flash write has the cache off, and the
    // resulting reset cannot print anything -- exactly the symptom). Guarding the
    // write on spi_flash_cache_enabled() did not stop the loop, so it is being taken
    // out of the picture entirely rather than left in as a confounding variable.
    // Re-enable once the VPN is stable, and only then find out whether it survives.
    // stackchan::LogRing::Install();

    // HAL init
    GetHAL().init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    // LOCAL BENCH TWEAK (not for upstream): jump straight into protocol mode on boot so
    // a host can drive the device over USB without anyone touching the screen. The
    // launcher is skipped entirely; AI.AGENT is a one-way trapdoor out of Mooncake
    // anyway (see ARCHITECTURE.md §6), so nothing is lost by not going through it.
    const bool skip_mooncake =
        (CONFIG_CONNECTION_TYPE_USB_NCM || GetHAL().getXiaozhiConfig().startAiAgentOnBoot) &&
        GetHAL().getWarmRebootTarget() < 0;

    if (!skip_mooncake) {
        // Install apps
        GetMooncake().installApp(std::make_unique<AppLauncher>());
        GetMooncake().installApp(std::make_unique<AppAiAgent>());
        GetMooncake().installApp(std::make_unique<AppAvatar>());
        GetMooncake().installApp(std::make_unique<AppEspnowControl>());
        GetMooncake().installApp(std::make_unique<AppAppCenter>());
        GetMooncake().installApp(std::make_unique<AppEzdata>());
        GetMooncake().installApp(std::make_unique<AppDance>());
        GetMooncake().installApp(std::make_unique<AppSetup>());

        // Main loop
        while (1) {
            GetHAL().feedTheDog();
            GetHAL().updateHeapStatusLog();

            GetMooncake().update();

            if (GetHAL().isXiaozhiStartRequested()) {
                break;
            }
        }

        // Uninstall all apps and destroy mooncake
        GetMooncake().uninstallAllApps();
        DestroyMooncake();
    }

    // Start xiaozhi, never returns
    GetHAL().startXiaozhi();
}
