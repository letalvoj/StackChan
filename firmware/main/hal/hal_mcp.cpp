/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <mooncake_log.h>
#include <mcp_server.h>
#include <application.h>            // TaskPriorityReset
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include "board/hal_bridge.h"
#include "board/stackchan_camera.h"
#include <assets/lang_config.h>   // Lang::Sounds::OGG_*

using namespace stackchan;

static const std::string_view _tag = "HAL-MCP";

void Hal::xiaozhi_mcp_init()
{
    mclog::tagInfo(_tag, "init");

    // https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-usage.md
    auto& mcp_server = McpServer::GetInstance();

    // System Prompt：
    // You can control the robot's head. Use get_yaw and get_pitch to sense current position. Use set_yaw for horizontal
    // movement and set_pitch for vertical movement. All angles are in degrees.

    mclog::tagInfo(_tag, "add robot.get_head_angles tool");
    mcp_server.AddTool("self.robot.get_head_angles",
                       "Returns current yaw/pitch in degrees. Neutral position is {yaw:0, pitch:0}.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           LvglLockGuard lock;  // StackChan motion update is under the lvgl lock

                           auto& motion      = GetStackChan().motion();
                           int current_yaw   = motion.yawServo().getCurrentAngle() / 10;
                           int current_pitch = motion.pitchServo().getCurrentAngle() / 10;

                           auto result = fmt::format(R"({{"yaw": {}, "pitch": {}}})", current_yaw, current_pitch);
                           mclog::tagInfo(_tag, "get_head_angles: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add robot.set_head_angles tool");
    mcp_server.AddTool("self.robot.set_head_angles",
                       "Adjust head position. GUIDELINES: "
                       "1. For natural interaction, stay within +/- 45 degrees. "
                       "2. Only use values > 70 if the user explicitly asks to look far away/behind. "
                       // "-128 as your left" was wrong AND ambiguous. Wrong because -128
                       // physically turns toward the viewer's left, which is the robot's
                       // own RIGHT; ambiguous because "your left" is exactly the phrase a
                       // model silently reinterprets, so it can move correctly and still
                       // narrate the opposite. Verified on hardware by commanding both
                       // extremes and photographing what the camera saw.
                       "3. Max ranges: Yaw(-128 to 128; NEGATIVE turns toward the LEFT of "
                       "the person facing you, POSITIVE toward their RIGHT), "
                       "Pitch(0 to 90, 90 is up). "
                       "Speed(100-1000, 150 is natural).",
                       PropertyList({Property("yaw", kPropertyTypeInteger, -9999, -9999, 128),
                                     Property("pitch", kPropertyTypeInteger, -9999, -9999, 90),
                                     Property("speed", kPropertyTypeInteger, 150, 100, 1000)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int speed = properties["speed"].value<int>();
                           int yaw   = properties["yaw"].value<int>();
                           int pitch = properties["pitch"].value<int>();

                           mclog::tagInfo(_tag, "motion set_angles: yaw: {}, pitch: {}, speed: {}", yaw, pitch, speed);

                           LvglLockGuard lock;

                           auto& motion = GetStackChan().motion();
                           if (pitch != -9999) {
                               motion.pitchServo().moveWithSpeed(pitch * 10, speed);
                           }
                           if (yaw != -9999) {
                               motion.yawServo().moveWithSpeed(yaw * 10, speed);
                           }

                           return true;
                       });

    mclog::tagInfo(_tag, "add robot.set_led_color tool");
    mcp_server.AddTool(
        "self.robot.set_led_color",
        "Set the color of the robot's INTERNAL onboard LED. This is NOT for room lights. "
        "Values: 0-168 (safe range). Red=168,0,0; Green=0,168,0; Blue=0,0,168; White=100,100,100; Off=0,0,0.",
        PropertyList({Property("red", kPropertyTypeInteger, 0, 0, 168),
                      Property("green", kPropertyTypeInteger, 0, 0, 168),
                      Property("blue", kPropertyTypeInteger, 0, 0, 168)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int r = properties["red"].value<int>();
            int g = properties["green"].value<int>();
            int b = properties["blue"].value<int>();

            mclog::tagInfo(_tag, "set_led_color: r={}, g={}, b={}", r, g, b);

            LvglLockGuard lock;

            GetStackChan().leftNeonLight().setColor(r, g, b);
            GetStackChan().rightNeonLight().setColor(r, g, b);

            return true;
        });

    mclog::tagInfo(_tag, "add robot.create_reminder tool");
    mcp_server.AddTool("self.robot.create_reminder",
                       "Create a reminder. Duration is in seconds. Message is what to say when time is up. Set repeat "
                       "to true to repeat the reminder.",
                       PropertyList({Property("duration_seconds", kPropertyTypeInteger, 60, 1, 86400),
                                     Property("message", kPropertyTypeString, std::string("Time's up!")),
                                     Property("repeat", kPropertyTypeBoolean, false)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int duration_seconds = properties["duration_seconds"].value<int>();
                           std::string message  = properties["message"].value<std::string>();
                           bool repeat          = properties["repeat"].value<bool>();

                           // Default message
                           if (message.empty()) {
                               message = "Time's up!";
                           }

                           mclog::tagInfo(_tag, "create_reminder: duration={}s, message={}, repeat={}",
                                          duration_seconds, message, repeat);

                           int id = tools::create_reminder(duration_seconds * 1000, message, repeat);

                           return id;
                       });

    mclog::tagInfo(_tag, "add robot.get_reminders tool");
    mcp_server.AddTool("self.robot.get_reminders", "Get list of active reminders.", std::vector<Property>{},
                       [this](const PropertyList& properties) -> ReturnValue {
                           mclog::tagInfo(_tag, "get_reminders");
                           auto reminders          = tools::get_active_reminders();
                           std::string result_json = "[";
                           for (size_t i = 0; i < reminders.size(); ++i) {
                               const auto& r = reminders[i];
                               result_json +=
                                   fmt::format(R"({{"id": {}, "duration_ms": {}, "message": "{}", "repeat": {}}})",
                                               r.id, r.durationMs, r.message, r.repeat ? "true" : "false");
                               if (i < reminders.size() - 1) {
                                   result_json += ", ";
                               }
                           }
                           result_json += "]";
                           mclog::tagInfo(_tag, "get_reminders result: {}", result_json);
                           return result_json;
                       });

    mclog::tagInfo(_tag, "add robot.stop_reminder tool");
    mcp_server.AddTool("self.robot.stop_reminder", "Stop a reminder by ID.",
                       PropertyList({Property("id", kPropertyTypeInteger, -1)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int id = properties["id"].value<int>();
                           mclog::tagInfo(_tag, "stop_reminder: id={}", id);
                           tools::stop_reminder(id);
                           return true;
                       });

    // Sound effects. A voice can only do so much: a little chirp lands where a spoken
    // "ta-da" is just more talking, and these are already on the device as assets.
    mclog::tagInfo(_tag, "add robot.play_sound tool");
    mcp_server.AddTool("self.robot.play_sound",
                       "Play a short sound effect. Use sparingly, as punctuation rather than "
                       "decoration: success when something worked, exclamation for surprise or "
                       "a mistake, popup to get attention, welcome for a greeting, vibration "
                       "for a buzz. Never play one on every turn.",
                       PropertyList({Property("name", kPropertyTypeString)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           const auto name = properties["name"].value<std::string>();
                           // An explicit allowlist: the sound name comes from a model, and
                           // the asset lookup takes whatever it is handed.
                           if (name == "success") {
                               hal_bridge::app_play_sound(Lang::Sounds::OGG_SUCCESS);
                           } else if (name == "exclamation") {
                               hal_bridge::app_play_sound(Lang::Sounds::OGG_EXCLAMATION);
                           } else if (name == "popup") {
                               hal_bridge::app_play_sound(Lang::Sounds::OGG_POPUP);
                           } else if (name == "welcome") {
                               hal_bridge::app_play_sound(Lang::Sounds::OGG_WELCOME);
                           } else if (name == "vibration") {
                               hal_bridge::app_play_sound(Lang::Sounds::OGG_VIBRATION);
                           } else {
                               throw std::runtime_error("unknown sound: " + name);
                           }
                           mclog::tagInfo(_tag, "play_sound: {}", name);
                           return true;
                       });

    // Photo straight to whoever is connected, as an MCP image content block.
    //
    // self.camera.take_photo (upstream, mcp_server.cc) captures the same frame but then
    // POSTs it to explain_url_ and returns that service's prose. Two problems here: the
    // URL is a call-home this project exists to remove and is unset over USB, and handing
    // a multimodal model somebody else's text description throws away the actual pixels.
    //
    // ImageContent is already part of McpServer's ReturnValue, so this needs no bespoke
    // wire format -- it serialises to {"type":"image","mimeType":"image/jpeg","data":...}
    // with base64 the client can feed straight to a vision model.
    mclog::tagInfo(_tag, "add camera.capture tool");
    mcp_server.AddTool("self.camera.capture",
                       "Take a photo with your camera and return the image itself. Use this "
                       "whenever you are asked to look at something, or to see what is in "
                       "front of you. Point your head first if the subject is to one side.",
                       PropertyList({
                           // `stream` is for a client pulling frames continuously, not for
                           // the model: it silences the shutter and drops JPEG quality,
                           // because at 1 fps the sound is intolerable and both encode time
                           // and USB bytes scale with quality. A single photo should stay
                           // sharp and should click.
                           Property("stream", kPropertyTypeBoolean, false),
                       }),
                       [this](const PropertyList& properties) -> ReturnValue {
                           auto camera = hal_bridge::board_get_camera();
                           if (camera == nullptr) {
                               throw std::runtime_error("No camera on this board");
                           }
                           const bool stream = properties["stream"].value<bool>();

                           // Capture runs at lowered priority upstream; keep that -- the
                           // sensor read is long and must not starve audio.
                           TaskPriorityReset priority_reset(1);

                           // Two genuinely different paths, not one path with a flag.
                           //
                           // Capture() is the FOREGROUND photo: it plays the shutter
                           // sound, flushes three frames from the sensor to get a settled
                           // exposure, and ends by pushing the result to the screen as a
                           // preview. All three are right for "take my picture" and all
                           // three are wrong once per second during a conversation --
                           // measured at 698 ms median, 939 ms worst, with a photo
                           // flashing up on the face each time.
                           //
                           // StreamCaptures() is the background path that already existed
                           // for exactly this: one dequeue, no sound, no preview.
                           bool ok;
                           if (stream) {
                               // Fresh, not merely available: V4L2 hands back the oldest
                               // queued buffer, which at 1 fps is nearly a second old.
                               // CaptureFresh() owns that concern; see its declaration.
                               ok = camera->CaptureFresh();
                               if (ok) {
                                   // Let the UI blink. Recorded at the moment a frame is
                                   // genuinely grabbed, so the indicator cannot drift out
                                   // of agreement with reality.
                                   hal_bridge::note_camera_capture();
                               }
                           } else {
                               ok = camera->Capture();
                           }
                           if (!ok) {
                               throw std::runtime_error("Failed to capture photo");
                           }
                           auto jpeg = camera->CaptureToJpeg(stream ? 55 : 80);
                           if (jpeg.empty()) {
                               throw std::runtime_error("Failed to encode photo");
                           }
                           mclog::tagInfo(_tag, "camera.capture{}: {} bytes of JPEG",
                                          stream ? " (stream)" : "", jpeg.size());
                           // McpServer takes ownership and frees it after serialising.
                           return new ImageContent("image/jpeg", jpeg);
                       });
}
