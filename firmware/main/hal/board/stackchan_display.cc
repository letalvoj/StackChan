/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "stackchan_display.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <vector>
#include <cstring>
#include <src/misc/cache/lv_cache.h>
#include <settings.h>
#include "hal_bridge.h"
#include "font_awesome.h"
#include <cstdio>
#include <lvgl.h>
#include <lvgl_theme.h>
#include <stackchan/stackchan.h>
#include <assets/lang_config.h>
#include <hal/hal.h>

using namespace stackchan;
using namespace stackchan::avatar;

#define TAG "StackChanAvatarDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_4);
// 20px variant for the camera button and the privacy indicators -- the 30px one is for
// the emotion display and is far too heavy for a corner glyph.
LV_FONT_DECLARE(font_awesome_20_4);

// Have to register themes, so the asset apply can update the text font
void StackChanAvatarDisplay::InitializeLcdThemes()
{
    auto text_font       = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font       = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));        // rgb(255, 255, 255)
    light_theme->set_text_color(lv_color_hex(0x000000));              // rgb(0, 0, 0)
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));   // rgb(224, 224, 224)
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));       // rgb(0, 128, 0)
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));  // rgb(221, 221, 221)
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));     // rgb(255, 255, 255)
    light_theme->set_system_text_color(lv_color_hex(0x000000));       // rgb(0, 0, 0)
    light_theme->set_border_color(lv_color_hex(0x000000));            // rgb(0, 0, 0)
    light_theme->set_low_battery_color(lv_color_hex(0x000000));       // rgb(0, 0, 0)
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));        // rgb(0, 0, 0)
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));              // rgb(255, 255, 255)
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));   // rgb(31, 31, 31)
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));       // rgb(0, 128, 0)
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));  // rgb(34, 34, 34)
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));     // rgb(0, 0, 0)
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));       // rgb(255, 255, 255)
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));            // rgb(255, 255, 255)
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));       // rgb(255, 0, 0)
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

StackChanAvatarDisplay::StackChanAvatarDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                                               bool mirror_y, bool swap_xy)
    : LvglDisplay(), panel_io_(panel_io), panel_(panel)
{
    width_  = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_         = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Draw white screen
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // port_cfg.task_priority   = 20;
    port_cfg.task_priority = 3;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle      = panel_io_,
        .panel_handle   = panel_,
        .control_handle = nullptr,
        .buffer_size    = static_cast<uint32_t>(width_ * 20),
        .double_buffer  = false,
        .trans_size     = 0,
        .hres           = static_cast<uint32_t>(width_),
        .vres           = static_cast<uint32_t>(height_),
        .monochrome     = false,
        .rotation =
            {
                .swap_xy  = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma     = 1,
                .buff_spiram  = 0,
                .sw_rotate    = 0,
                .swap_bytes   = 1,
                .full_refresh = 0,
                .direct_mode  = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                StackChanAvatarDisplay* display = static_cast<StackChanAvatarDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg                   = this,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);

    // Create boot logo label if not warm boot
    if (GetHAL().getWarmRebootTarget() < 0) {
        ESP_LOGI(TAG, "Create boot logo label");
        Lock();
        {
            uitk::lvgl_cpp::ScreenActive screen;
            screen.setBgColor(lv_color_hex(0x000000));
        }
        GetHAL().bootLogo = std::make_unique<BootLogo>();
        Unlock();
    }

    // Robot will be created later in SetupXiaoZhiUI()
}

StackChanAvatarDisplay::~StackChanAvatarDisplay()
{
    ESP_LOGI(TAG, "Destroying StackChanAvatarDisplay");

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }

    auto& stackchan = GetStackChan();
    if (stackchan.hasAvatar()) {
        stackchan.resetAvatar();
    }
}

bool StackChanAvatarDisplay::Lock(int timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void StackChanAvatarDisplay::Unlock()
{
    lvgl_port_unlock();
}

lv_disp_t* StackChanAvatarDisplay::GetLvglDisplay()
{
    return display_;
}

#include <hal/board/hal_bridge.h>

void StackChanAvatarDisplay::SetupUI()
{
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called

    auto& stackchan = GetStackChan();

    if (stackchan.hasAvatar()) {
        ESP_LOGW(TAG, "Avatar already created");
        return;
    }

    DisplayLockGuard lock(this);

    ESP_LOGI(TAG, "Creating Stack-chan Avatar...");

    // CuteAvatar is the shipping face. DefaultAvatar is still built and still works --
    // swap the type here to go back, nothing else changes, because both satisfy the same
    // Feature contract and every modifier drives them identically.
    //
    // Preview either without flashing: tools/facelab/grid.sh <label> {cute|default}
    auto avatar = std::make_unique<CuteAvatar>();
    avatar->init(lv_screen_active());
    avatar->getPanel()->onClick().connect([]() {
        static uint32_t last_toggle_tick = 0;
        const uint32_t now               = GetHAL().millis();
        if (last_toggle_tick != 0 && now - last_toggle_tick < 2000) {
            return;
        }

        if (hal_bridge::is_xiaozhi_ready()) {
            last_toggle_tick = now;
            hal_bridge::toggle_xiaozhi_chat_state();
        }
    });

    // Created here, inside SetupUI's DisplayLockGuard, so widget creation is serialised
    // against the render task like everything else in this function.
    CreateCameraButton();
    CreatePrivacyIndicators();

    stackchan.attachAvatar(std::move(avatar));
    stackchan.addModifier(std::make_unique<BreathModifier>());
    controller_.SetBlinkModifierId(stackchan.addModifier(std::make_unique<BlinkModifier>()));
    stackchan.addModifier(std::make_unique<HeadPetModifier>());
    stackchan.addModifier(std::make_unique<ImuEventModifier>());

    preview_image_ = lv_image_create(lv_screen_active());
    lv_obj_set_size(preview_image_, 320, 240);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    // GetHAL().startStackChanAutoUpdate(24);

    auto config = hal_bridge::get_xiaozhi_config();
    controller_.SetIdleMotionLevel(config.idleRandomMovementLevel);

    ESP_LOGI(TAG, "Avatar created and started");
}

void StackChanAvatarDisplay::LvglLock()
{
    if (!Lock(30000)) {
        ESP_LOGE("Display", "Failed to lock display");
    }
}

void StackChanAvatarDisplay::LvglUnlock()
{
    Unlock();
}

void StackChanAvatarDisplay::SetEmotion(const char* emotion)
{
    DisplayLockGuard lock(this);
    controller_.SetEmotion(emotion);
}

void StackChanAvatarDisplay::SetChatMessage(const char* role, const char* content)
{
    DisplayLockGuard lock(this);
    controller_.SetChatMessage(role, content, setup_ui_called_);
}

void StackChanAvatarDisplay::ClearChatMessages()
{
    DisplayLockGuard lock(this);
    controller_.ClearChatMessages();
}

void StackChanAvatarDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image)
{
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc          = preview_image_cached_->image_dsc();
    // Set image source and show preview image
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // Scale to fit width
        lv_image_set_scale(preview_image_, 256 * width_ / img_dsc->header.w);
    }

    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(preview_image_);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, 6000 * 1000));
}

void StackChanAvatarDisplay::CreateCameraButton()
{
    // Bottom-right, deliberately understated: a camera that is easy to hit by accident is
    // a privacy problem, and one that shouts is ugly on a face. Subtle grey glyph, but the
    // touch target is much larger than the glyph -- a small icon does not have to mean a
    // small button, and on a 320x240 panel with fingers it must not.
    camera_btn_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(camera_btn_);
    lv_obj_set_size(camera_btn_, kCameraBtnTouch, kCameraBtnTouch);
    lv_obj_align(camera_btn_, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    lv_obj_set_style_radius(camera_btn_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(camera_btn_, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(camera_btn_, LV_OBJ_FLAG_CLICKABLE);

    camera_icon_ = lv_label_create(camera_btn_);
    lv_label_set_text(camera_icon_, FONT_AWESOME_CAMERA);
    lv_obj_set_style_text_font(camera_icon_, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(camera_icon_, lv_color_hex(kCameraIdleColor), 0);
    lv_obj_center(camera_icon_);

    lv_obj_add_event_cb(
        camera_btn_,
        [](lv_event_t* e) {
            static uint32_t last_tick = 0;
            const uint32_t now        = GetHAL().millis();
            // Same debounce as the face: capture takes a moment to spin up and a double
            // tap would toggle straight back off.
            if (last_tick != 0 && now - last_tick < 2000) {
                return;
            }
            if (hal_bridge::is_xiaozhi_ready()) {
                last_tick = now;
                hal_bridge::toggle_xiaozhi_chat_state_with_video();
            }
        },
        LV_EVENT_CLICKED, nullptr);
}

void StackChanAvatarDisplay::CreatePrivacyIndicators()
{
    // Where the status bar sits, but always visible -- the status bar auto-hides, and an
    // indicator that tells you a microphone is live only when you swipe for it is not an
    // indicator. Tiny on purpose: present, not shouting.
    //
    // Colours follow the platform convention people already read without thinking:
    // orange = microphone, blue = camera.
    mic_icon_ = lv_label_create(lv_layer_top());
    lv_label_set_text(mic_icon_, FONT_AWESOME_MICROPHONE);
    lv_obj_set_style_text_font(mic_icon_, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(mic_icon_, lv_color_hex(kMicColor), 0);
    lv_obj_align(mic_icon_, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_add_flag(mic_icon_, LV_OBJ_FLAG_HIDDEN);

    cam_icon_ = lv_label_create(lv_layer_top());
    lv_label_set_text(cam_icon_, FONT_AWESOME_CAMERA);
    lv_obj_set_style_text_font(cam_icon_, &font_awesome_20_4, 0);
    lv_obj_set_style_text_color(cam_icon_, lv_color_hex(kCamColor), 0);
    lv_obj_align(cam_icon_, LV_ALIGN_TOP_RIGHT, -30, 4);
    lv_obj_add_flag(cam_icon_, LV_OBJ_FLAG_HIDDEN);

    // Capture blink.
    //
    // An LVGL timer, not the 1 Hz application tick: this runs in LVGL's own context so
    // it needs no external lock (the bug that froze boot came from touching widgets from
    // the app task without one), and 1 Hz cannot render a blink against a 1 fps capture
    // -- it would alias into either a solid light or nothing at all.
    //
    // Shows red for kBlinkMs after each frame is grabbed. With VAD gating, "a session is
    // open" and "a frame just left" are very different facts, and only this one tells you
    // the gate actually opened.
    lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<StackChanAvatarDisplay*>(lv_timer_get_user_data(t));
            if (self->cam_icon_ == nullptr) {
                return;
            }
            const bool firing = hal_bridge::ms_since_camera_capture() < kBlinkMs;
            const uint32_t want = firing ? kCaptureBlinkColor : kCamColor;
            if (want != self->cam_blink_color_) {
                self->cam_blink_color_ = want;
                lv_obj_set_style_text_color(self->cam_icon_, lv_color_hex(want), 0);
                if (self->camera_icon_ != nullptr && hal_bridge::is_camera_live()) {
                    // The button in the corner blinks with it, so the control you pressed
                    // is also the thing that reports back.
                    lv_obj_set_style_text_color(self->camera_icon_, lv_color_hex(want), 0);
                }
            }
        },
        kBlinkPollMs, this);
}

void StackChanAvatarDisplay::UpdatePrivacyIndicators()
{
    // MUST hold the LVGL lock. This runs on the application task once a second while the
    // LVGL port task is rendering; touching objects without the lock is an unsynchronised
    // mutation of live widget state, and it hung the device on the very first clock tick
    // -- boot froze at "Logging in...", the main loop never produced another line, and
    // the screen stopped responding to touch entirely. Every other method here that
    // touches widgets takes this guard; this one was written without it.
    DisplayLockGuard lock(this);

    const bool mic = hal_bridge::is_mic_live();
    const bool cam = hal_bridge::is_camera_live();

    if (mic_icon_ != nullptr) {
        mic ? lv_obj_remove_flag(mic_icon_, LV_OBJ_FLAG_HIDDEN)
            : lv_obj_add_flag(mic_icon_, LV_OBJ_FLAG_HIDDEN);
    }
    if (cam_icon_ != nullptr) {
        cam ? lv_obj_remove_flag(cam_icon_, LV_OBJ_FLAG_HIDDEN)
            : lv_obj_add_flag(cam_icon_, LV_OBJ_FLAG_HIDDEN);
    }
    // The button itself lights up while streaming, so the control and the indicator agree.
    if (camera_icon_ != nullptr) {
        lv_obj_set_style_text_color(camera_icon_,
                                    lv_color_hex(cam ? kCamColor : kCameraIdleColor), 0);
    }
}

void StackChanAvatarDisplay::UpdateStatusBar(bool update_all)
{
    UpdatePrivacyIndicators();

    // The avatar owns the whole screen, so there is no bar here to redraw -- the visible
    // indicator lives in the pull-down status bar (status_bar.cpp, the Link widget).
    //
    // What this tick is for is the LED. The idle colour is chosen in SetStatus(), which
    // only fires on a *status change*, so a device that connects or disconnects while
    // already sitting idle would keep whatever colour it had -- stuck pale red after the
    // host arrives, or misleadingly dark after it leaves. Once a second is plenty.
    //
    // Only while idle: during a conversation the listening/speaking colours must win,
    // and connection state is self-evident anyway when something is talking to you.
    const bool connected = hal_bridge::is_host_connected();
    const int state = connected ? 1 : 0;
    if (state == conn_last_state_ && !update_all) {
        return;
    }
    conn_last_state_ = state;

    if (hal_bridge::is_xiaozhi_idle()) {
        // Pale red (24/255) rather than a hard red: waiting for a host is a normal
        // resting state, not a fault, and an unattended robot should not look alarmed.
        GetHAL().setRgbColor(0, connected ? 0 : 24, 0, 0);
        GetHAL().refreshRgb();
    }
}

void StackChanAvatarDisplay::SetTheme(Theme* theme)
{
    ESP_LOGI(TAG, "SetTheme: %s", theme->name().c_str());

    auto& stackchan = GetStackChan();
    if (!stackchan.hasAvatar()) {
        ESP_LOGE(TAG, "Avatar is invalid");
        return;
    }

    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font  = lvgl_theme->text_font()->font();

    stackchan.avatar().setSpeechTextFont((void*)text_font);
}

void StackChanAvatarDisplay::SetStatus(const char* status)
{
    DisplayLockGuard lock(this);
    controller_.SetStatus(status);
}

void StackChanAvatarDisplay::ShowNotification(const char* notification, int duration_ms)
{
}
