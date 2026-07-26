#include <emscripten.h>
#include <lvgl.h>
#include <stdio.h>
#include <memory>
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <apps/apps.h>
#include "hal/wasm_application.h"

using namespace mooncake;
using namespace smooth_ui_toolkit;

#include <stackchan/stackchan.h>
#include <hal/board/hal_bridge.h>
#include <apps/common/home_indicator/home_indicator.h>
#include <apps/common/status_bar/status_bar.h>
#include <apps/common/reminder/reminder.h>

static void main_loop_iteration() {
    lv_tick_inc(16);
    lv_timer_handler();
    GetHAL().feedTheDog();
    GetMooncake().update();

    static bool s_xiaozhi_started = false;
    if (!s_xiaozhi_started && (GetHAL().isXiaozhiStartRequested() || WasmApplication::GetInstance().GetDeviceState() != kDeviceStateUnknown)) {
        s_xiaozhi_started = true;
        printf("[WASM_AI_AGENT] AI.AGENT active! Activating interactive StackChan Assistant via authoritative GetHAL().startXiaozhi()...\n");
        GetHAL().startXiaozhi();
    }

    if (s_xiaozhi_started) {
        static bool s_xiaozhi_setup_done = false;
        tools::update_reminders();
        GetStackChan().update();
        WasmApplication::GetInstance().Update();
        
        if (!s_xiaozhi_setup_done && hal_bridge::is_xiaozhi_ready()) {
            printf("[WASM_LOOP] Xiaozhi ready -> Creating authoritative Home Indicator & Status Bar views.\n");
            view::create_home_indicator([]() { GetHAL().requestWarmReboot(0); }, 0x81DBBD, 0x134233);
            view::create_status_bar(0x81DBBD, 0x134233);
            s_xiaozhi_setup_done = true;
        }
        if (s_xiaozhi_setup_done) {
            view::update_home_indicator();
            view::update_status_bar();
        }
    }
}

extern "C" void app_main() {
    printf("[WASM_BOOT] Initializing StackChan Gateway WebAssembly runtime (Phase 1)...\n");
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    GetHAL().init();
    
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    printf("[WASM_BOOT] Installing full application suite into Mooncake framework...\n");
    GetMooncake().installApp(std::make_unique<AppLauncher>());
    GetMooncake().installApp(std::make_unique<AppAiAgent>());
    GetMooncake().installApp(std::make_unique<AppAvatar>());
    GetMooncake().installApp(std::make_unique<AppEspnowControl>());
    GetMooncake().installApp(std::make_unique<AppAppCenter>());
    GetMooncake().installApp(std::make_unique<AppEzdata>());
    GetMooncake().installApp(std::make_unique<AppDance>());
    GetMooncake().installApp(std::make_unique<AppSetup>());
    
    printf("[WASM_BOOT] Setup complete! Transitioning execution to emscripten_set_main_loop (60 fps)...\n");
    emscripten_set_main_loop(main_loop_iteration, 0, 1);
}

int main() {
    app_main();
    return 0;
}
