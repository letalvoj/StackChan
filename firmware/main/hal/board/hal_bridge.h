/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#ifndef __EMSCRIPTEN__
#include <driver/i2c_master.h>
// Forward-declared rather than including stackchan_camera.h: that header pulls in
// esp_video's <linux/videodev2.h>, whose _IO/_IOR/_IOW macros collide with lwip's
// incompatible BSD-encoded ones. Only a pointer to the type is needed here.
class StackChanCamera;
#endif
#include <cstdint>
#include <lvgl.h>
#include <string_view>

namespace hal_bridge {

struct TouchPoint_t {
    int num = 0;
    int x   = -1;
    int y   = -1;
};

struct Data_t {
    TouchPoint_t touchPoint;
    bool isXiaozhiMode              = false;
    bool isXiaozhiModeToggleEnabled = false;
};

struct XiaozhiConfig_t {
    uint32_t idleShutdownTimeSeconds = 600;
    bool allowShutdownWhenCharging   = false;
    uint8_t idleRandomMovementLevel  = 2;
    bool startAiAgentOnBoot          = false;
};

void lock();
void unlock();
Data_t& get_data();

void set_touch_point(int num, int x, int y);
TouchPoint_t get_touch_point();

bool is_xiaozhi_mode();
void set_xiaozhi_mode(bool mode);
void toggle_xiaozhi_chat_state();

// Same, but asks the client to stream camera frames alongside audio. Bound to the camera
// button rather than the face, so a tap never silently switches the camera on.
void toggle_xiaozhi_chat_state_with_video();

// What is currently being captured, for the privacy indicators. Microphone is live
// whenever the device is listening; camera only during a video session.
bool is_mic_live();
bool is_camera_live();

// Called on every frame actually captured for streaming, so the UI can blink. This is
// the difference between "the camera session is open" and "a frame just left the
// device" -- with VAD gating, those are very different things and only the second one
// tells you the gate opened.
void note_camera_capture();
uint32_t ms_since_camera_capture();

// Report a physical thing that happened TO the robot -- being petted, being shaken.
// These already drive local reactions (hearts, blush, dizzy eyes); forwarding them lets
// a connected agent know it was touched, so it can react to it in conversation rather
// than the reaction being purely cosmetic. Rate-limited and dropped when nobody is
// listening; this is flavour, not telemetry.
void report_sensor_event(const char* event);

void disply_lvgl_lock();
void disply_lvgl_unlock();
lv_disp_t* display_get_lvgl_display();

void xiaozhi_board_init();
void start_xiaozhi_app();
bool is_xiaozhi_ready();
bool is_xiaozhi_idle();

// Is a host currently connected over the active transport? Used by the LED and the
// connection badge, so both read the same source of truth rather than each keeping
// their own idea of "connected" and drifting apart.
bool is_host_connected();

// Short name of the transport this build listens on: "USB", "WiFi", ... Shown on the
// badge, so it answers "not connected *to what*" without needing a serial log.
const char* transport_label();
XiaozhiConfig_t get_xiaozhi_config();
void set_xiaozhi_config(const XiaozhiConfig_t& config);

#ifndef __EMSCRIPTEN__
i2c_master_bus_handle_t board_get_i2c_bus();
StackChanCamera* board_get_camera();
#endif
int board_get_battery_level();
bool board_is_battery_charging();
void board_set_backlight_brightness(uint8_t brightness, bool permanent = false);
uint8_t board_get_backlight_brightness();
void board_set_speaker_volume(uint8_t volume, bool permanent = false);
uint8_t board_get_speaker_volume();

void app_play_sound(const std::string_view& sound);

}  // namespace hal_bridge
