/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <display/lvgl_display/lvgl_display.h>
#include <stackchan/avatar_controller.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>
#include <memory>

class StackChanAvatarDisplay : public LvglDisplay {
private:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_       = nullptr;
    AvatarController controller_;

    // Last connection state the LED was painted for; -1 = never, so the first tick
    // always paints. See UpdateStatusBar().
    int conn_last_state_ = -1;

    // Camera button (bottom-right) and the always-on privacy indicators (top-right).
    lv_obj_t* camera_btn_  = nullptr;
    lv_obj_t* camera_icon_ = nullptr;
    lv_obj_t* mic_icon_    = nullptr;
    lv_obj_t* cam_icon_    = nullptr;

    // Touch target is far larger than the glyph: a small icon must not mean a small
    // button on a panel this size.
    static constexpr int kCameraBtnTouch  = 52;
    static constexpr uint32_t kCameraIdleColor = 0x6A6A6A;   // subtle grey when off
    static constexpr uint32_t kMicColor        = 0xFF9500;   // orange, the usual "mic live"
    static constexpr uint32_t kCamColor        = 0x2F9BFF;   // blue, the usual "camera live"

    // Per-frame capture blink: red for kBlinkMs whenever a frame is actually grabbed.
    // Polled faster than it blinks so the flash has clean edges rather than aliasing.
    static constexpr uint32_t kCaptureBlinkColor = 0xFF3B30;
    static constexpr uint32_t kBlinkMs           = 220;
    static constexpr uint32_t kBlinkPollMs       = 60;

    uint32_t cam_blink_color_ = 0;

    void CreateCameraButton();
    void CreatePrivacyIndicators();
    void UpdatePrivacyIndicators();

    lv_obj_t* preview_image_                         = nullptr;
    esp_timer_handle_t preview_timer_                = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;

protected:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

public:
    StackChanAvatarDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height,
                           int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy);
    virtual ~StackChanAvatarDisplay();

    void InitializeLcdThemes();

    // Override Display methods to control Robot
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetupUI() override;
    virtual void SetTheme(Theme* theme) override;
    virtual void SetStatus(const char* status) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;

    void LvglLock();
    void LvglUnlock();
    lv_disp_t* GetLvglDisplay();
};
