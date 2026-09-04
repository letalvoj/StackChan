/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <mooncake.h>
#include <mooncake_log.h>
#include <cstdint>
#include <functional>
#include <smooth_ui_toolkit.hpp>
#include <smooth_lvgl.hpp>
#include <assets/assets.h>
#include <fmt/chrono.h>
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <hal/board/network_link.h>
#include <memory>
#include <vector>
#include <lvgl.h>
#include <src/draw/lv_image_dsc.h>

using namespace smooth_ui_toolkit;
using namespace smooth_ui_toolkit::lvgl_cpp;

/**
 * @brief
 *
 */
class StatuBarGesture {
public:
    std::function<void(void)> onGesture;

    StatuBarGesture() : _is_tracking(false), _last_state(LV_INDEV_STATE_REL)
    {
    }

    void init()
    {
        _is_tracking    = false;
        _last_state     = LV_INDEV_STATE_REL;
        _screen_height  = 240;
        _top_threshold  = 20;  // 距离顶部 20 像素内触发
        _swipe_min_dist = 50;  // 向下滑动至少 50 像素才触发
    }

    void update()
    {
        lv_indev_t* indev = GetHAL().lvTouchpad;
        if (!indev) {
            return;
        }

        lv_indev_state_t state = lv_indev_get_state(indev);
        lv_point_t curr_point;
        lv_indev_get_point(indev, &curr_point);

        // 1. 按下瞬间 (Transition: Released -> Pressed)
        if (state == LV_INDEV_STATE_PR && _last_state == LV_INDEV_STATE_REL) {
            // 只有在按下那一刻就在顶部，才标记为追踪开始
            if (curr_point.y <= _top_threshold && curr_point.y >= 0) {
                _start_point = curr_point;
                _is_tracking = true;
            } else {
                _is_tracking = false;  // 按下位置不对，此次滑动全程忽略
            }
        }
        // 2. 抬起瞬间 (Transition: Pressed -> Released)
        else if (state == LV_INDEV_STATE_REL && _last_state == LV_INDEV_STATE_PR) {
            if (_is_tracking) {
                int delta_y = curr_point.y - _start_point.y;  // 向下滑为正
                int delta_x = abs(curr_point.x - _start_point.x);

                // 判断标准：向下位移足够，且角度偏垂直
                if (delta_y > _swipe_min_dist && delta_y > delta_x) {
                    if (onGesture) {
                        onGesture();
                    }
                }
                _is_tracking = false;  // 重置追踪状态
            }
        }

        _last_state = state;  // 更新状态机
    }

private:
    bool _is_tracking;
    lv_indev_state_t _last_state;  // 记录上一帧的状态
    lv_point_t _start_point;
    int _screen_height;
    int _top_threshold;
    int _swipe_min_dist;
};

namespace status_bar_view {

class Widget {
public:
    virtual ~Widget()     = default;
    virtual void update() = 0;
};

class TimeLabel : public Widget {
public:
    TimeLabel(lv_obj_t* parent, uint32_t colorText)
    {
        _label = std::make_unique<Label>(parent);
        _label->setText("");
        _label->setTextColor(lv_color_hex(colorText));
        _label->setTextFont(&lv_font_montserrat_16);
        _label->align(LV_ALIGN_CENTER, 0, 0);

        update();
    }

    void update() override
    {
        auto now   = std::chrono::system_clock::now();
        auto now_t = std::chrono::system_clock::to_time_t(now);

        struct tm local_tm;
        localtime_r(&now_t, &local_tm);

        int hour12 = local_tm.tm_hour % 12;
        if (hour12 == 0) {
            hour12 = 12;
        }

        _label->setText(fmt::format("{}:{:02d} {}", hour12, local_tm.tm_min, local_tm.tm_hour >= 12 ? "PM" : "AM"));
    }

private:
    std::unique_ptr<Label> _label;
};

class BatteryIcon {
public:
    BatteryIcon(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary)
    {
        _color_primary = colorPrimary;

        _bat_top = std::make_unique<uitk::lvgl_cpp::Container>(parent);
        _bat_top->setBgColor(lv_color_hex(colorSecondary));
        _bat_top->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
        _bat_top->setBorderWidth(0);
        _bat_top->setSize(4, 4);
        _bat_top->setRadius(4);
        _bat_top->setOutlineWidth(1);
        _bat_top->setOutlineColor(lv_color_hex(colorPrimary));

        _bar = std::make_unique<uitk::lvgl_cpp::Bar>(parent);
        _bar->setSize(30, 12);
        _bar->setRadius(4);
        _bar->setRadius(0, LV_PART_INDICATOR);
        _bar->setBgColor(lv_color_hex(colorSecondary));
        _bar->setBgColor(lv_color_hex(colorPrimary), LV_PART_INDICATOR);
        _bar->setBgOpa(LV_OPA_COVER);
        _bar->setRange(0, 100);
        _bar->setValue(0);
        _bar->setPadding(1, 1, 1, 1);
        _bar->setOutlineWidth(1);
        _bar->setOutlineColor(lv_color_hex(colorPrimary));
        _bar->setRadius(3, LV_PART_INDICATOR);

        _lightning_icon     = std::make_unique<uitk::lvgl_cpp::Image>(parent);
        _icon_bat_lightning = assets::get_image("icon_bat_lightning.bin");
        _lightning_icon->setSrc(&_icon_bat_lightning);
        _lightning_icon->setImageRecolor(lv_color_hex(colorPrimary));
        _lightning_icon->setImageRecolorOpa(LV_OPA_COVER);
        _lightning_icon->setHidden(true);

        // "Powered but not charging" -- e.g. the charge-voltage cap has been reached,
        // so the AXP2101 reports no net current either way. No custom art exists for
        // this state, and none is needed: LVGL's default font ships LV_SYMBOL_USB as a
        // built-in glyph, styled to match the lightning icon it sits in place of.
        _plug_label = std::make_unique<Label>(parent);
        _plug_label->setText(LV_SYMBOL_USB);
        _plug_label->setTextColor(lv_color_hex(colorPrimary));
        _plug_label->setTextFont(&lv_font_montserrat_16);
        _plug_label->setHidden(true);
    }

    void align(lv_align_t align, int32_t x_ofs, int32_t y_ofs)
    {
        _bar->align(align, x_ofs, y_ofs);
        lv_obj_align_to(_bat_top->get(), _bar->get(), LV_ALIGN_CENTER, 15, 0);
        lv_obj_align_to(_lightning_icon->get(), _bar->get(), LV_ALIGN_CENTER, 0, 0);
        lv_obj_align_to(_plug_label->get(), _bar->get(), LV_ALIGN_CENTER, 0, 0);
    }

    void setLevel(uint8_t level)
    {
        _bar->setValue(level);
    }

    // Three states, matching how macOS reads a laptop battery: actively drawing charge
    // current gets the lightning bolt, external power present but not charging (already
    // full, or a voltage cap reached before 100%) gets the plug glyph, and running on
    // battery alone gets neither -- discharging is the assumed default and does not need
    // an icon to announce itself.
    //
    // While charging, the bar is additionally tinted by the AXP2101's own charger-phase
    // bits, because "charging" alone is the one thing that could not be diagnosed from
    // the outside: bulk-charging toward the cap and idling in the constant-voltage tail
    // just below it look identical on a percentage readout, and telling them apart is
    // the whole question when a charge-voltage cap may or may not be working. Costs no
    // space in a full status bar -- it re-uses a colour that was already there.
    //
    //   orange -- trickle/pre-charge (deeply discharged pack)
    //   green  -- constant current, i.e. genuinely bulk-charging
    //   amber  -- constant voltage, i.e. at the cap, current tapering off
    //
    // The AXP2101 phase field is 0..5, NOT 0..4: 4 is "charge done" and 5 is
    // "not charging" (XPOWERS_AXP2101_CHG_STOP_STATE). Both mean no current is going
    // in, so neither draws the lightning bolt.
    void setPowerState(bool charging, bool discharging, int phase = -1)
    {
        // Phase, where we have it, outranks the current-direction bits.
        //
        // Measured on this device: the direction bits reported "charging" continuously
        // for two solid minutes -- zero transitions -- while the phase field was moving
        // between constant-current and stopped the whole time. One of those two is
        // telling the truth about whether current is flowing, and it is not the one that
        // never changes. The direction bits stay as the fallback for when phase has not
        // been read yet.
        bool is_charging = charging;
        if (phase >= 0) {
            is_charging = (phase <= 3);   // trickle, pre-charge, CC, CV
        }

        if (is_charging) {
            uint32_t phase_color = 0x19C25F;   // green: constant-current, or unknown
            switch (phase) {
                case 0:
                case 1: phase_color = 0xF5A623; break;   // trickle / pre-charge
                case 2: phase_color = 0x19C25F; break;   // constant current
                case 3: phase_color = 0xF5D142; break;   // constant voltage (at the cap)
                default: break;
            }
            _bar->setBgColor(lv_color_hex(phase_color), LV_PART_INDICATOR);
            _lightning_icon->setHidden(false);
            _plug_label->setHidden(true);
        } else if (!discharging) {
            _bar->setBgColor(lv_color_hex(_color_primary), LV_PART_INDICATOR);
            _lightning_icon->setHidden(true);
            _plug_label->setHidden(false);
        } else {
            _bar->setBgColor(lv_color_hex(_color_primary), LV_PART_INDICATOR);
            _lightning_icon->setHidden(true);
            _plug_label->setHidden(true);
        }
    }

private:
    std::unique_ptr<Bar> _bar;
    std::unique_ptr<Container> _bat_top;
    std::unique_ptr<Image> _lightning_icon;
    lv_image_dsc_t _icon_bat_lightning;
    std::unique_ptr<Label> _plug_label;

    uint32_t _color_primary = 0;
};

class Battery : public Widget {
public:
    Battery(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary)
    {
        _label_level = std::make_unique<Label>(parent);
        _label_level->setText("");
        _label_level->setTextColor(lv_color_hex(colorPrimary));
        _label_level->setTextFont(&lv_font_montserrat_16);
        _label_level->align(LV_ALIGN_RIGHT_MID, -41, 0);

        _battery_icon = std::make_unique<BatteryIcon>(parent, colorSecondary, colorPrimary);
        _battery_icon->align(LV_ALIGN_RIGHT_MID, -7, 0);

        // Live pack voltage, in the gap between the centred clock and the percentage.
        // Fixed right offset rather than aligned to the percentage label: the latter
        // changes width as it crosses 100%/10%, and a one-shot lv_obj_align_to would
        // not track that. -85 clears the widest percentage ("100%" ends at -41) with a
        // few px to spare, and font 14 keeps the string clear of the clock.
        //
        // Worth having on the face of the device rather than only in /debug: the
        // percentage is a fuel-gauge estimate whose 100% moves with the charge-voltage
        // cap, so it cannot answer "what is the battery actually sitting at" -- which
        // is the only number that says whether the cap is doing anything.
        _label_voltage = std::make_unique<Label>(parent);
        _label_voltage->setText("");
        _label_voltage->setTextColor(lv_color_hex(colorPrimary));
        _label_voltage->setTextFont(&lv_font_montserrat_14);
        _label_voltage->align(LV_ALIGN_RIGHT_MID, -85, 0);

        update();
    }

    void update() override
    {
        auto level = GetHAL().getBatteryLevel();
        _label_level->setText(fmt::format("{}%", level));
        _battery_icon->setLevel(level);

        // Debounced, because the underlying state genuinely does flicker and showing
        // that faithfully is useless to a human.
        //
        // Near a charge-voltage cap the charger legitimately cycles: it tops the pack to
        // the target, terminates, the pack relaxes ~100-200 mV under load, drops below
        // the recharge threshold, and it starts again. That is correct behaviour, but
        // rendered live it strobes the icon and the bar colour several times a minute.
        // Requiring a few consecutive agreeing samples keeps the display readable while
        // leaving the raw, undebounced truth in /debug and /debug/history for diagnosis.
        const bool charging    = GetHAL().isBatteryCharging();
        const bool discharging = GetHAL().isBatteryDischarging();
        const int phase        = GetHAL().getChargePhase();

        if (charging == _pending_charging && discharging == _pending_discharging) {
            if (_pending_stable_count < kStateDebounceSamples) {
                _pending_stable_count++;
            }
        } else {
            _pending_charging    = charging;
            _pending_discharging = discharging;
            _pending_stable_count = 1;
        }

        if (_pending_stable_count >= kStateDebounceSamples) {
            _shown_charging    = _pending_charging;
            _shown_discharging = _pending_discharging;
            _shown_phase       = phase;
        }

        _battery_icon->setPowerState(_shown_charging, _shown_discharging, _shown_phase);

        // 0 mV means the ADC has not answered yet (or reports no battery). Blank rather
        // than "0.00V", which reads as a dead pack instead of a missing reading.
        auto mv = GetHAL().getBatteryVoltageMv();
        _label_voltage->setText(mv > 0 ? fmt::format("{:.2f}V", mv / 1000.0) : "");
    }

private:
    std::unique_ptr<Label> _label_level;
    std::unique_ptr<BatteryIcon> _battery_icon;
    std::unique_ptr<Label> _label_voltage;

    static constexpr int kStateDebounceSamples = 4;
    bool _pending_charging     = false;
    bool _pending_discharging  = false;
    int _pending_stable_count  = 0;
    bool _shown_charging       = false;
    bool _shown_discharging    = false;
    int _shown_phase           = -1;
};

class Wifi : public Widget {
public:
    Wifi(lv_obj_t* parent, uint32_t colorPrimary)
    {
        _color_primary = colorPrimary;
        _wifi_icon = std::make_unique<Image>(parent);
        _wifi_icon->align(LV_ALIGN_LEFT_MID, 58, 0);  // Gateway owns the far left
        _wifi_icon->setImageRecolor(lv_color_hex(colorPrimary));
        _wifi_icon->setImageRecolorOpa(LV_OPA_COVER);

        _icon_wifi_low    = assets::get_image("icon_wifi_low.bin");
        _icon_wifi_medium = assets::get_image("icon_wifi_medium.bin");
        _icon_wifi_high   = assets::get_image("icon_wifi_high.bin");
        _icon_wifi_slash  = assets::get_image("icon_wifi_slash.bin");

        update();
    }

    void update() override
    {
        // THREE states, not two, and the difference matters when something is wrong:
        //
        //   off          -- WiFi is not enabled in this build. Nothing is broken, so
        //                   show nothing rather than an alarming crossed-out icon.
        //   disconnected -- enabled and trying, but not associated. Crossed out, red.
        //                   This is the state worth noticing.
        //   connected    -- bars by signal strength.
        //
        // Collapsing "off" and "disconnected" into one glyph was the old behaviour and
        // it lies in both directions: a USB-only build looked permanently broken, and
        // a WiFi build that failed to associate looked identical to one that was never
        // asked to.
        switch (stackchan::WifiLinkState()) {
            case stackchan::WifiLink::Disabled:
                _wifi_icon->setSrc(NULL);
                return;

            case stackchan::WifiLink::Disconnected:
                _wifi_icon->setSrc(&_icon_wifi_slash);
                _wifi_icon->setImageRecolor(lv_palette_lighten(LV_PALETTE_RED, 1));
                return;

            case stackchan::WifiLink::Connected: {
                _wifi_icon->setImageRecolor(lv_color_hex(_color_primary));
                // Same thresholds Hal::getWifiStatus() uses, so the two agree rather
                // than each inventing its own idea of a good signal.
                const int8_t rssi = stackchan::WifiRssiDbm();
                _wifi_icon->setSrc(rssi >= -65   ? &_icon_wifi_high
                                   : rssi >= -75 ? &_icon_wifi_medium
                                                 : &_icon_wifi_low);
                return;
            }
        }
    }

private:
    std::unique_ptr<Image> _wifi_icon;
    lv_image_dsc_t _icon_wifi_low;
    lv_image_dsc_t _icon_wifi_medium;
    lv_image_dsc_t _icon_wifi_high;
    lv_image_dsc_t _icon_wifi_slash;
    uint32_t _color_primary = 0;
};

/**
 * @brief Gateway indicator: WHICH ROAD the connected host came in on.
 *
 * Sits at the far left, where the eye lands first. The Wifi widget beside it reports
 * the radio; this reports the thing that actually carries the conversation, and the
 * two are genuinely independent -- WiFi can be associated while the agent is attached
 * over the cable, and that combination used to be indistinguishable from any other.
 *
 * A pictogram rather than the old "USB ×" text, because the text was compiled from
 * CONNECTION_TYPE_* and said "USB" for a client that had arrived over WiFi. One
 * listener is bound to INADDR_ANY and serves every interface, so the label was not
 * merely terse, it was wrong. hal_bridge::gateway_link() reads the live socket's peer
 * address instead.
 *
 * The status bar auto-hides, so this is the *detail* view; the persistent at-a-glance
 * signal remains the idle LED going pale red (StackChanAvatarDisplay::UpdateStatusBar),
 * which is deliberately left alone.
 */
class Gateway : public Widget {
public:
    Gateway(lv_obj_t* parent, uint32_t colorPrimary)
    {
        _label = std::make_unique<Label>(parent);
        // Montserrat carries LVGL's built-in symbol range, so these need no extra font
        // and no new image asset in the mmap partition -- which matters, because the
        // asset pipeline is a build step and this is a status glyph.
        _label->setTextFont(&lv_font_montserrat_16);
        _label->align(LV_ALIGN_LEFT_MID, 11, 0);
        _colorPrimary = colorPrimary;
        update();
    }

    void update() override
    {
        const auto link = hal_bridge::gateway_link();
        if (link == _last_link && _drawn) {
            return;
        }
        _last_link = link;
        _drawn     = true;

        switch (link) {
            case hal_bridge::GatewayLink::None:
                // Pale red, not hard red: an unattended robot with no host is waiting,
                // not broken, and must not look like it is throwing an error.
                _label->setText(LV_SYMBOL_CLOSE);
                _label->setTextColor(lv_palette_lighten(LV_PALETTE_RED, 1));
                return;

            case hal_bridge::GatewayLink::Usb:
                _label->setText(LV_SYMBOL_USB);
                _label->setTextColor(lv_color_hex(_colorPrimary));
                return;

            case hal_bridge::GatewayLink::Wifi:
                _label->setText(LV_SYMBOL_WIFI);
                _label->setTextColor(lv_color_hex(_colorPrimary));
                return;

            case hal_bridge::GatewayLink::Vpn:
                // SHUFFLE is the least-bad glyph in LVGL's built-in set for "routed
                // through somewhere else", which is what a tailnet is. There is no
                // lock or shield in the built-in range, and pulling in a whole extra
                // font for one icon on a feature parked at P2 is not a trade worth
                // making. Revisit if the tailnet ships.
                _label->setText(LV_SYMBOL_SHUFFLE);
                _label->setTextColor(lv_color_hex(_colorPrimary));
                return;
        }
    }

private:
    std::unique_ptr<Label> _label;
    uint32_t _colorPrimary = 0;
    hal_bridge::GatewayLink _last_link = hal_bridge::GatewayLink::None;
    bool _drawn = false;
};

class StatusBarView {
public:
    StatusBarView(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary)
    {
        _panel = std::make_unique<uitk::lvgl_cpp::Container>(lv_screen_active());
        _panel->setBgColor(lv_color_hex(colorSecondary));
        _panel->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
        _panel->align(LV_ALIGN_TOP_MID, 0, 0);
        _panel->setBorderWidth(0);
        _panel->setSize(320, 28);
        _panel->setRadius(0);
        _panel->setPadding(0, 0, 0, 0);
        _panel->onClick().connect([this]() { hide(); });

        _widgets.push_back(std::make_unique<TimeLabel>(_panel->get(), colorPrimary));
        _widgets.push_back(std::make_unique<Battery>(_panel->get(), colorSecondary, colorPrimary));
        _widgets.push_back(std::make_unique<Wifi>(_panel->get(), colorPrimary));
        _widgets.push_back(std::make_unique<Gateway>(_panel->get(), colorPrimary));

        _panel->setPos(0, _pos_y_hide);
        _pos_y_anim.springOptions().bounce         = 0.1;
        _pos_y_anim.springOptions().visualDuration = 0.3;
        // _pos_y_anim.easingOptions().duration = 0.3;
        _pos_y_anim.teleport(_pos_y_hide);
    }

    void update()
    {
        _pos_y_anim.update();
        if (!_pos_y_anim.done()) {
            _panel->setPos(0, _pos_y_anim.directValue());
        } else {
            if (_hide_panel_flag) {
                _hide_panel_flag = false;
                _panel->setHidden(true);
            }
        }

        if (GetHAL().millis() - _last_update_tick > 1000) {
            _last_update_tick = GetHAL().millis();
            for (auto& widget : _widgets) {
                widget->update();
            }
        }
    }

    void show()
    {
        _panel->moveForeground();
        _panel->setHidden(false);
        _pos_y_anim = _pos_y_show;
        _is_hidden  = false;
    }

    void hide()
    {
        _pos_y_anim      = _pos_y_hide;
        _is_hidden       = true;
        _hide_panel_flag = true;
    }

    bool isHidden() const
    {
        return _is_hidden;
    }

private:
    const int _pos_y_show = 0;
    const int _pos_y_hide = -28;

    std::unique_ptr<Container> _panel;
    std::vector<std::unique_ptr<Widget>> _widgets;
    AnimateValue _pos_y_anim;
    bool _is_hidden            = false;
    bool _hide_panel_flag      = false;
    uint32_t _last_update_tick = 0;
};

}  // namespace status_bar_view

/**
 * @brief
 *
 */
class StatusBar {
public:
    void init(lv_obj_t* parent, uint32_t colorSecondary, uint32_t colorPrimary)
    {
        _status_bar_gesture            = std::make_unique<StatuBarGesture>();
        _status_bar_gesture->onGesture = [&]() { handle_gesture(); };
        _status_bar_gesture->init();

        _status_bar_view = std::make_unique<status_bar_view::StatusBarView>(parent, colorSecondary, colorPrimary);
        _status_bar_view->show();
        _status_bar_show_tick = GetHAL().millis();
        _is_first_show        = true;
    }

    void update()
    {
        _status_bar_gesture->update();
        _status_bar_view->update();
        update_visibility();
    }

private:
    std::unique_ptr<StatuBarGesture> _status_bar_gesture;
    std::unique_ptr<status_bar_view::StatusBarView> _status_bar_view;
    uint32_t _status_bar_show_tick = 0;
    bool _is_first_show            = true;

    void handle_gesture()
    {
        _status_bar_view->show();
        _status_bar_show_tick = GetHAL().millis();
    }

    void update_visibility()
    {
        if (!_status_bar_view->isHidden()) {
            if (GetHAL().millis() - _status_bar_show_tick > (_is_first_show ? 1800 : 6000)) {
                _is_first_show = false;
                _status_bar_view->hide();
            }
        }
    }
};

/**
 * @brief
 *
 */
namespace view {

static std::unique_ptr<StatusBar> _status_bar;

void create_status_bar(uint32_t colorSecondary, uint32_t colorPrimary, lv_obj_t* parent)
{
    _status_bar = std::make_unique<StatusBar>();
    _status_bar->init(parent, colorSecondary, colorPrimary);
}

void update_status_bar()
{
    if (_status_bar) {
        _status_bar->update();
    }
}

bool is_status_bar_created()
{
    return _status_bar != nullptr;
}

void destroy_status_bar()
{
    _status_bar.reset();
}

}  // namespace view
