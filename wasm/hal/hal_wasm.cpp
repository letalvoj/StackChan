#include <sdkconfig.h>
#include <hal/hal.h>
#include <emscripten.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <stackchan/motion/servo.h>
#include <stackchan/motion/motion.h>
#include <stackchan/stackchan.h>
#include <ArduinoJson.hpp>
#include <settings.h>
#include <stackchan/avatar/avatar.h>
#include <stackchan/modifiers/modifiers.h>
#include "wasm_protocol.h"
#include <mooncake.h>
#include "wasm_board.h"
#include <camera.h>
#include <hal/board/hal_bridge.h>
#include "wasm_log.h"
#include "wasm_application.h"
#include <apps/common/reminder/reminder.h>
#include <apps/common/reminder/reminder_view.hpp>

namespace hal_bridge {
    static bool s_xiaozhi_mode_enabled = false;
    XiaozhiConfig_t get_xiaozhi_config() {
        XiaozhiConfig_t cfg;
        cfg.idleRandomMovementLevel = 2;
        return cfg;
    }
    void set_xiaozhi_config(const XiaozhiConfig_t& config) {}
    bool is_xiaozhi_mode() { return s_xiaozhi_mode_enabled; }
    void set_xiaozhi_mode(bool mode) { s_xiaozhi_mode_enabled = mode; }
    bool is_xiaozhi_ready() {
        DeviceState s = WasmApplication::GetInstance().GetDeviceState();
        return s == kDeviceStateIdle || s == kDeviceStateListening || s == kDeviceStateSpeaking || s == kDeviceStateActivating;
    }
    bool is_xiaozhi_idle() {
        return WasmApplication::GetInstance().GetDeviceState() == kDeviceStateIdle;
    }
    void toggle_xiaozhi_chat_state() {
        printf("[WASM_BRIDGE] toggle_xiaozhi_chat_state() -> delegating to WasmApplication::ToggleChat().\n");
        WasmApplication::GetInstance().ToggleChat();
    }
    void start_xiaozhi_app() {
        printf("[WASM_BRIDGE] start_xiaozhi_app() -> closing launcher & uninstalling Mooncake apps prior to WasmApplication initialization.\n");
        mooncake::GetMooncake().closeApp(0);
        mooncake::GetMooncake().uninstallAllApps();
        if (WasmApplication::GetInstance().GetDeviceState() == kDeviceStateUnknown) {
            WasmApplication::GetInstance().Initialize();
        }
    }
}

// Embedded audio SFX linker symbol stubs exported with external linkage
extern "C" {
    char _binary_camera_shutter_ogg_start[1] = { 0 };
    char _binary_camera_shutter_ogg_end[1]   = { 0 };
    char _binary_new_notification_ogg_start[1] = { 0 };
    char _binary_new_notification_ogg_end[1]   = { 0 };
}

static std::unique_ptr<Hal> s_hal_instance;
static lv_color_t s_lvgl_buf1[320 * 240];
static int s_frame_counter = 0;
static uint8_t s_brightness = 128;
static uint8_t s_volume = 70;
static int s_current_yaw_raw = 0;
static int s_current_pitch_raw = 0;
static bool s_is_app_configed = false;

class WasmServo : public stackchan::motion::Servo {
public:
    WasmServo(std::string_view label, int minAngle, int maxAngle, bool isYaw)
        : _label(label), _is_yaw(isYaw), _current_angle(0), _last_logged_angle(-9999)
    {
        set_angle_limit(uitk::Vector2i(minAngle, maxAngle));
        printf("[WASM_SERVO] Motor [%s] instantiated with physical limits [%d..%d] (0.1° step)\n", _label.c_str(), minAngle, maxAngle);
    }

    void init() override {
        printf("[WASM_SERVO] Motor [%s] init() executed — simulated PWM control ready.\n", _label.c_str());
    }

    void set_angle_impl(int angle) override {
        angle = uitk::clamp(angle, _angle_limit.x, _angle_limit.y);
        _current_angle = angle;
        
        if (_is_yaw) {
            s_current_yaw_raw = angle;
        } else {
            s_current_pitch_raw = angle;
        }

        if (abs(_last_logged_angle - angle) >= 50 || angle == 0 || angle == _angle_limit.x || angle == _angle_limit.y) {
            WASM_LOG_FIRST_N(3, "[WASM_SERVO:%s] ⚙️ Target angle moved -> %.1f° (raw: %d)\n",
                             _label.c_str(), angle / 10.0f, angle);
            _last_logged_angle = angle;
        }

        EM_ASM({
            if (typeof Module.updateServoRadar === 'function') {
                Module.updateServoRadar($0 / 10.0, $1 / 10.0);
            }
        }, s_current_yaw_raw, s_current_pitch_raw);
    }

    int getCurrentAngle() override {
        return _current_angle;
    }

    bool is_moving_impl() override {
        return false;
    }

    void setTorqueEnabled(bool enabled) override {
        printf("[WASM_SERVO:%s] Torque %s\n", _label.c_str(), enabled ? "ENABLED (Locked)" : "DISABLED (Released)");
    }

    void setCurrentAngleAsZero() override {
        printf("[WASM_SERVO:%s] Zero position calibration locked at current angle.\n", _label.c_str());
    }

private:
    std::string _label;
    bool _is_yaw;
    int _current_angle;
    int _last_logged_angle;
};

// Extern C API callable from JavaScript Web Bluetooth deck
extern "C" EMSCRIPTEN_KEEPALIVE void wasm_ble_send_config_json(const char* json_str) {
    printf("[WASM_BLE] Received GATT Write on Config Characteristic (e2e5e5e3): %s\n", json_str);
    
    ArduinoJson::JsonDocument doc;
    auto error = ArduinoJson::deserializeJson(doc, json_str);
    if (error) {
        printf("[WASM_BLE] JSON parse failure: %s\n", error.c_str());
        return;
    }

    std::string cmd = doc["cmd"] | "";
    if (cmd == "handshake") {
        printf("[WASM_BLE] 🤝 Processing phone handshake challenge — emitting AppConnected event.\n");
        GetHAL().onAppConfigEvent.emit(AppConfigEvent::AppConnected);
        const char* responseJson = "{\"cmd\":\"notifyState\",\"data\":{\"type\":4,\"state\":\"WASM_HMAC_TOKEN_7F3A9B\"}}";
        EM_ASM({
            if (typeof Module.onBleNotify === 'function') {
                Module.onBleNotify(UTF8ToString($0));
            }
        }, responseJson);
    } else if (cmd == "setWifi") {
        const char* ssid = doc["data"]["ssid"] | "unknown_ssid";
        printf("[WASM_BLE] 📶 Provisioning Wi-Fi credentials for SSID: '%s' over BLE GATT...\n", ssid);
        const char* notifyConn = "{\"cmd\":\"notifyState\",\"data\":{\"type\":0,\"state\":\"wifiConnecting\"}}";
        EM_ASM({ if (typeof Module.onBleNotify === 'function') Module.onBleNotify(UTF8ToString($0)); }, notifyConn);
        
        { Settings s("app_config", true); s.SetBool("is_configed", true); }
        GetHAL().onAppConfigEvent.emit(AppConfigEvent::TryWifiConnect);
        printf("[WASM_BLE] ⏳ TryWifiConnect emitted... simulating successful connection.\n");
        GetHAL().onAppConfigEvent.emit(AppConfigEvent::WifiConnected);
        printf("[WASM_BLE] ✅ WifiConnected emitted — setup state machine should transition to Done.\n");
        
        const char* notifyOk = "{\"cmd\":\"notifyState\",\"data\":{\"type\":1,\"state\":\"wifiConnected\"}}";
        EM_ASM({ if (typeof Module.onBleNotify === 'function') Module.onBleNotify(UTF8ToString($0)); }, notifyOk);
        printf("[WASM_BLE] ✓ Wi-Fi provisioning completed. App is now configured and paired.\n");
    } else if (cmd == "getWifiStatus") {
        const char* statusJson = s_is_app_configed ?
            "{\"cmd\":\"notifyState\",\"data\":{\"type\":1,\"state\":\"wifiConnected\"}}" :
            "{\"cmd\":\"notifyState\",\"data\":{\"type\":3,\"state\":\"wifiDisconnected\"}}";
        EM_ASM({ if (typeof Module.onBleNotify === 'function') Module.onBleNotify(UTF8ToString($0)); }, statusJson);
    }
}

Hal& GetHAL() {
    if (!s_hal_instance) {
        s_hal_instance = std::make_unique<Hal>();
    }
    return *s_hal_instance;
}

static void wasm_lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    s_frame_counter++;
    if (s_frame_counter == 1) {
        printf("[WASM_HAL] Frame #1 rendering via LVGL software flush (area: %d,%d - %d,%d)\n",
               area->x1, area->y1, area->x2, area->y2);
    }
    
    EM_ASM({
        const canvas = document.getElementById('display');
        if (!canvas) return;
        const ctx = canvas.getContext('2d');
        const imgData = ctx.createImageData(320, 240);
        const src = new Uint16Array(HEAPU8.buffer, $0, 320 * 240);
        const data = imgData.data;
        for (let i = 0; i < 320 * 240; i++) {
            const p = src[i];
            data[i * 4 + 0] = ((p >> 11) & 0x1F) * 255 / 31; // R
            data[i * 4 + 1] = ((p >> 5) & 0x3F) * 255 / 63;  // G
            data[i * 4 + 2] = (p & 0x1F) * 255 / 31;          // B
            data[i * 4 + 3] = 255;                             // Alpha
        }
        ctx.putImageData(imgData, 0, 0);
    }, px_map);

    lv_display_flush_ready(disp);
}

static void wasm_lvgl_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    int pressed = EM_ASM_INT({ return (typeof Module._touchState !== 'undefined' && Module._touchState.pressed) ? 1 : 0; });
    if (pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = EM_ASM_INT({ return Module._touchState.x || 160; });
        data->point.y = EM_ASM_INT({ return Module._touchState.y || 120; });
        if (s_frame_counter % 30 == 0) {
            printf("[WASM_HAL] Touch press detected at (%d, %d)\n", data->point.x, data->point.y);
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void Hal::init() {
    printf("[WASM_HAL] Initializing Hardware Abstraction Layer for Emscripten browser sandbox...\n");
    lv_init();
    printf("[WASM_HAL] LVGL core initialized.\n");
    
    lv_display_t* disp = lv_display_create(320, 240);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_lvgl_buf1, nullptr, sizeof(s_lvgl_buf1), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, wasm_lvgl_flush_cb);
    printf("[WASM_HAL] 320x240 RGB565 display buffer and software render flush mounted.\n");

    lvTouchpad = lv_indev_create();
    lv_indev_set_type(lvTouchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvTouchpad, wasm_lvgl_read_cb);
    lv_indev_set_display(lvTouchpad, disp);
    lv_indev_enable(lvTouchpad, true);
    printf("[WASM_HAL] Pointer touchpad input device mounted.\n");
    
    servo_init();
}

void Hal::servo_init() {
    printf("[WASM_HAL] Initializing simulated dual-servo motion assembly via WasmServo drivers...\n");
    auto yaw_servo   = std::make_unique<WasmServo>("Yaw", -1280, 1280, true);
    auto pitch_servo = std::make_unique<WasmServo>("Pitch", -30, 950, false);
    auto motion      = std::make_unique<stackchan::motion::Motion>(std::move(yaw_servo), std::move(pitch_servo));
    motion->init();
    GetStackChan().attachMotion(std::move(motion));
    printf("[WASM_HAL] ✓ Simulated dual-servo motor controller successfully mounted into StackChan::motion()\n");
}

void Hal::delay(std::uint32_t ms) {}

std::uint32_t Hal::millis() {
    using namespace std::chrono;
    static auto start = steady_clock::now();
    return duration_cast<milliseconds>(steady_clock::now() - start).count();
}

void Hal::feedTheDog() {}
std::array<uint8_t, 6> Hal::getFactoryMac() { return {0x02, 0x00, 0x00, 0xAA, 0x54, 0x4D}; }
std::string Hal::getFactoryMacString(std::string divider) { return "02" + divider + "00" + divider + "00" + divider + "AA" + divider + "54" + divider + "4D"; }
void Hal::reboot() {
    printf("[WASM_HAL] Reboot requested — reloading page in 500ms...\n");
    EM_ASM({ setTimeout(function() { location.reload(); }, 500); });
}
void Hal::updateHeapStatusLog() {}
uint8_t Hal::getBatteryLevel() { return 95; }
bool Hal::isBatteryCharging() { return true; }
void Hal::factoryReset() { printf("[WASM_HAL] Factory reset requested.\n"); }
void Hal::lvglLock() {}
void Hal::lvglUnlock() {}

void Hal::setBackLightBrightness(uint8_t brightness, bool permanent) {
    s_brightness = brightness;
    printf("[WASM_HAL] Backlight brightness set to %d (permanent=%d)\n", brightness, permanent);
}
uint8_t Hal::getBackLightBrightness() { return s_brightness; }

void Hal::startXiaozhi() {
    printf("[WASM_HAL] startXiaozhi() -> executing authentic ESP32 architectural sequence.\n");
    mooncake::GetMooncake().uninstallAllApps();
    auto& motion = GetStackChan().motion();
    motion.setAutoAngleSyncEnabled(true);
    motion.setAutoTorqueReleaseEnabled(true);

    tools::on_reminder_triggered().clear();
    tools::on_reminder_triggered().connect([](int id, std::string_view msg) {
        printf("[WASM_HAL] reminder triggered: id: %d, msg: %s\n", id, std::string(msg).c_str());
        {
            auto& avatar = GetStackChan().avatar();
            avatar.addDecorator(std::make_unique<view::ReminderView>(lv_screen_active(), msg));
        }
    });

    hal_bridge::start_xiaozhi_app();
}

XiaozhiConfig_t Hal::getXiaozhiConfig() {
    XiaozhiConfig_t cfg;
    cfg.startAiAgentOnBoot = false;
    return cfg;
}
void Hal::setXiaozhiConfig(XiaozhiConfig_t config) {
    printf("[WASM_HAL] setXiaozhiConfig() updated.\n");
}

void Hal::startBleServer() {
    printf("[WASM_HAL] startBleServer() invoked — advertising GATT Service UUID e2e5e5e0-1234-5678-1234-56789abcdef0\n");
}
bool Hal::isBleConnected() { return false; }
void Hal::startAppConfigServer() {
    printf("[WASM_HAL] startAppConfigServer() invoked — Web BLE Provisioning Gateway mounted on Config Char e2e5e5e3\n");
}
bool Hal::isAppConfiged() {
    printf("[WASM_HAL] isAppConfiged() -> true (skipping WiFi setup wizard in WASM emulator)\n");
    return true;
}
void Hal::resetAppConfiged() {
    Settings settings("app_config", true);
    settings.SetBool("is_configed", false);
    printf("[WASM_HAL] resetAppConfiged() -> localStorage cleared\n");
}
void Hal::setRgbColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    EM_ASM({ if (typeof Module.onUpdateLed === 'function') Module.onUpdateLed($0, $1, $2); }, r, g, b);
}
void Hal::showRgbColor(uint8_t r, uint8_t g, uint8_t b) {
    printf("[WASM_HAL:LED] showRgbColor(%d, %d, %d)\n", r, g, b);
    EM_ASM({ if (typeof Module.onUpdateLed === 'function') Module.onUpdateLed($0, $1, $2); }, r, g, b);
}
void Hal::refreshRgb() {}
void Hal::setServoPowerEnabled(bool enabled) {}

class WasmAvatarWorker : public mooncake::BasicAbility {
public:
    WasmAvatarWorker() {
        printf("[WASM_HAL] WasmAvatarWorker instantiated for Mooncake AVATAR mode.\n");
    }

    void onCreate() override {
        printf("[WASM_HAL] WasmAvatarWorker onCreate -> Activating live webcam and video mirror signal.\n");
        GetHAL().onWsVideoModeChange.emit(true);
        EM_ASM({
            var video = document.getElementById('webcam-feed');
            if (typeof window.toggleWebcamStream === 'function' && (!video || !video.srcObject)) {
                window.toggleWebcamStream();
            }
        });
    }

    void onRunning() override {
        uint32_t now = GetHAL().millis();
        if (now - last_tick_ < 66) { // ~15 FPS rate limiting
            return;
        }
        last_tick_ = now;
        Camera* cam = Board::GetInstance().GetCamera();
        if (cam) {
            cam->Capture();
        }
    }

    void onDestroy() override {
        printf("[WASM_HAL] WasmAvatarWorker onDestroy -> Suspending webcam stream and disabling video mode.\n");
        GetHAL().onWsVideoModeChange.emit(false);
        EM_ASM({
            var video = document.getElementById('webcam-feed');
            if (typeof window.toggleWebcamStream === 'function' && video && video.srcObject) {
                window.toggleWebcamStream();
            }
        });
    }

private:
    uint32_t last_tick_ = 0;
};

static int s_avatar_worker_ability_id = -1;

void Hal::startWebSocketAvatarService(std::function<void(std::string_view)> onStartLog) {
    printf("[WASM_HAL] startWebSocketAvatarService -> Installing WasmAvatarWorker into Mooncake extensionManager.\n");
    if (onStartLog) onStartLog("Activating WASM\nAVATAR Camera...");
    if (s_avatar_worker_ability_id >= 0) {
        mooncake::GetMooncake().extensionManager()->destroyAbility(s_avatar_worker_ability_id);
        s_avatar_worker_ability_id = -1;
    }
    s_avatar_worker_ability_id = mooncake::GetMooncake().extensionManager()->createAbility(std::make_unique<WasmAvatarWorker>());
}

void Hal::syncRtcTimeToSystem() {}
void Hal::syncSystemTimeToRtc() {}
void Hal::setTimezone(std::string_view tz) {}
std::string Hal::getTimezone() { return "UTC"; }
void Hal::startEspNow(int channel) {}
bool Hal::espNowSend(const std::vector<uint8_t>& data, const uint8_t* destAddr) { return false; }
void Hal::setLaserEnabled(bool enabled) {}

void Hal::requestWarmReboot(int appIndex) {
    if (s_avatar_worker_ability_id >= 0) {
        printf("[WASM_HAL] Tearing down active WasmAvatarWorker (ID: %d) on warm reboot request.\n", s_avatar_worker_ability_id);
        mooncake::GetMooncake().extensionManager()->destroyAbility(s_avatar_worker_ability_id);
        s_avatar_worker_ability_id = -1;
    }
    printf("[WASM_HAL] Warm reboot requested to target app %d -> saving simulated NVS target and executing window.location.reload()...\n", appIndex);
    EM_ASM({
        try {
            sessionStorage.setItem('stackchan_warm_reboot_target', $0);
            window.location.reload();
        } catch(e) {
            console.error('[WASM_HAL] Warm reboot execution failed:', e);
        }
    }, appIndex);
}
int Hal::getWarmRebootTarget() {
    return EM_ASM_INT({
        try {
            var val = sessionStorage.getItem('stackchan_warm_reboot_target');
            return val !== null ? parseInt(val, 10) : -1;
        } catch(e) {
            return -1;
        }
    });
}
void Hal::clearWarmRebootRequest() {
    printf("[WASM_HAL] Clearing warm reboot NVS target.\n");
    EM_ASM({
        try {
            sessionStorage.removeItem('stackchan_warm_reboot_target');
        } catch(e) {}
    });
}

void Hal::startNetwork(std::function<void(std::string_view)> onLog) {
    printf("[WASM_HAL] startNetwork() simulated in browser sandbox.\n");
    if (onLog) onLog("WASM browser network ready");
}
WifiStatus Hal::getWifiStatus() { return WifiStatus::High; }
void Hal::startSntp() {}
app_center::AppInfoList_t Hal::fetchAppList() { return {}; }
void Hal::launchApp(std::string_view url, std::function<void(int)> onProgress) {}
void Hal::startEzDataService(std::function<void(std::string_view)> onStartLog) {}
UserAccountInfo_t Hal::getUserAccountInfo() { return {"wasm_user", "wasm_chan"}; }
bool Hal::updateAccountInfo(std::function<void(std::string_view)> onLog) { return true; }
bool Hal::unbindAccount(std::function<void(std::string_view)> onLog) { return true; }
bool Hal::updateFirmware(std::function<void(std::string_view)> onLog) { return false; }
void Hal::setSpeakerVolume(uint8_t volume, bool permanent) {
    s_volume = volume;
    printf("[WASM_HAL] Speaker volume set to %d\n", volume);
}
uint8_t Hal::getSpeakerVolume() { return s_volume; }
std::string Hal::startMicTest(std::function<void(MicTestStatus)> onStatusUpdate) { return "wasm_mic_stub"; }
void Hal::getMicWaveformFrame(std::vector<int16_t>& data) { data.clear(); }
void Hal::clearupMicTest() {}

// Private member stub implementations
void Hal::xiaozhi_board_init() {}
void Hal::lvgl_init() {}
void Hal::xiaozhi_mcp_init() {}
void Hal::ble_init(bool useAltUuid) {}
void Hal::head_touch_init() {}
void Hal::io_expander_init() {}
void Hal::imu_init() {}
void Hal::rtc_init() {}
