#include "wifi_board.h"
#include "usb_net_board.h"

// The board's network base class is a build-time choice. Under USB networking there is
// no radio to provision, and the stock WebsocketProtocol runs over the USB link.
#if CONFIG_CONNECTION_TYPE_USB_NCM
using StackChanNetBoard = UsbNetBoard;
#else
using StackChanNetBoard = WifiBoard;
#endif
#include "cores3_audio_codec.h"
#include "display/lcd_display.h"
#include "stackchan_display.h"
#include "application.h"
#include "protocols/websocket_server_protocol.h"
#include "hal/hal.h"       // GetHAL().millis() for the capture-blink timestamp
#include <assets/lang_config.h>   // Lang::Sounds::OGG_*
#include <atomic>
#include "config.h"
#include "power_save_timer.h"
#include "i2c_device.h"
#include "axp2101.h"
#include "settings.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <wifi_station.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>
#include <esp_timer.h>
#include <algorithm>
// Drops lwip's BSD ioctl macros (pulled in above via application.h) so esp_video's
// V4L2 encoding below wins cleanly. Safe here: this TU uses neither ioctl family.
#include "ioctl_compat.h"
#include "stackchan_camera.h"
#include "hal_bridge.h"

#define TAG "M5Stack-StackChan-Board"

#define XPOWERS_AXP2101_ICC_CHG_SET      (0x62)
#define XPOWERS_AXP2101_CHG_V_SET        (0x64)
// Battery-voltage ADC result, verified against XPowersLib's XPowersAXP2101.hpp
// (getBattVoltage) rather than trusted from memory, after CHG_V_SET's enum values
// turned out to be off by one there. RESULT0 holds the top 5 bits, RESULT1 the low 8,
// combined as ((RESULT0 & 0x1F) << 8) | RESULT1 -- and that combined value is already
// millivolts, no further scaling.
#define XPOWERS_AXP2101_ADC_DATA_RESULT0 (0x34)
#define XPOWERS_AXP2101_ADC_DATA_RESULT1 (0x35)
// STATUS1 (VBUS/battery presence flags) and the charge-termination control register.
// Bit 4 of ITERM_CHG_SET_CTRL is the termination ENABLE: with it clear the charger
// never terminates on falling current at all, which looks identical from the outside
// to "the voltage cap is not working" -- both present as charging that never finishes.
// Bits [3:0] are the termination-current setting itself.
#define XPOWERS_AXP2101_STATUS1          (0x00)
#define XPOWERS_AXP2101_ITERM_CHG_SET    (0x63)
// VBUS input current limit (bits [2:0]): 0=100mA, 1=500mA, 2=900mA, 3=1000mA,
// 4=1500mA, 5=2000mA. This is the ceiling on everything drawn from the cable -- system
// load AND charging together. If it sits below what the board actually draws while
// running, charging gets whatever is left over, which can be nothing, and the charger
// oscillates instead of charging. Firmware never sets it, so whatever is here is the
// power-on default (or whatever the last thing to touch it left behind).
#define XPOWERS_AXP2101_INPUT_CUR_LIMIT  (0x16)

class Pmic : public Axp2101 {
public:
    /**
     * @brief axp2101 charge currnet voltage parameters.
     */
    typedef enum __xpowers_axp2101_chg_curr {
        XPOWERS_AXP2101_CHG_CUR_0MA,
        XPOWERS_AXP2101_CHG_CUR_100MA = 4,
        XPOWERS_AXP2101_CHG_CUR_125MA,
        XPOWERS_AXP2101_CHG_CUR_150MA,
        XPOWERS_AXP2101_CHG_CUR_175MA,
        XPOWERS_AXP2101_CHG_CUR_200MA,
        XPOWERS_AXP2101_CHG_CUR_300MA,
        XPOWERS_AXP2101_CHG_CUR_400MA,
        XPOWERS_AXP2101_CHG_CUR_500MA,
        XPOWERS_AXP2101_CHG_CUR_600MA,
        XPOWERS_AXP2101_CHG_CUR_700MA,
        XPOWERS_AXP2101_CHG_CUR_800MA,
        XPOWERS_AXP2101_CHG_CUR_900MA,
        XPOWERS_AXP2101_CHG_CUR_1000MA,
    } xpowers_axp2101_chg_curr_t;

    // Target voltage (bits [2:0] of XPOWERS_AXP2101_CHG_V_SET). Capping below the
    // chip's 4.2V default trades away the top of the capacity curve for far less time
    // spent sitting at 100% -- the device stays plugged in for hours at a stretch, and
    // that is the condition that ages a lithium cell fastest.
    //
    // Values start at 1, not 0, and there is no 4.36V option -- both confirmed against
    // XPowersLib's XPowersParams.hpp, the real source this enum is modeled on. An
    // earlier version of this enum started at 0 and invented a 4.36V entry from
    // (wrong) memory; register value 0 is not a defined voltage code at all, which is
    // exactly why that version measurably capped nothing -- the device kept charging
    // straight through to what looked like the old ~4.2V default.
    typedef enum __xpowers_axp2101_chg_vol {
        XPOWERS_AXP2101_CHG_VOL_4V0 = 1,
        XPOWERS_AXP2101_CHG_VOL_4V1,
        XPOWERS_AXP2101_CHG_VOL_4V2,
        XPOWERS_AXP2101_CHG_VOL_4V35,
        XPOWERS_AXP2101_CHG_VOL_4V4,
    } xpowers_axp2101_chg_vol_t;

    // Power Init
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr)
    {
        uint8_t data = ReadReg(0x90);
        data |= 0b10110100;
        WriteReg(0x90, data);
        // WriteReg(0x99, (0b11110 - 5));
        WriteReg(0x97, (0b11110 - 2));
        WriteReg(0x69, 0b00110101);
        WriteReg(0x30, 0b111111);
        WriteReg(0x90, 0xBF);
        WriteReg(0x94, 33 - 5);
        WriteReg(0x95, 33 - 5);
        WriteReg(0x27, 0x00);

        auto ret = setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_700MA);
        if (!ret) {
            ESP_LOGE(TAG, "Set charge current failed");
        } else {
            ESP_LOGI(TAG, "Set charge current success");
        }

        // Read BEFORE writing: once setChargeTargetVoltage() runs, the factory value is
        // gone for good. The PMIC keeps its registers across an MCU reset -- it is only
        // cleared by actually removing power from the chip -- so on a unit this firmware
        // has already run on, this reports what we last set, not what the factory did.
        // It is still worth having: it catches anything else moving this register, and
        // on a fresh unit it captures the real default exactly once.
        chg_v_reg_stock_ = ReadReg(XPOWERS_AXP2101_CHG_V_SET);
        ESP_LOGI(TAG, "CHG_V_SET (0x64) before we touch it: 0x%02x", chg_v_reg_stock_);

        if (!setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V0)) {
            ESP_LOGE(TAG, "Set charge voltage failed");
        } else {
            ESP_LOGI(TAG, "Set charge voltage success (4.0V cap)");
        }
        // Read back rather than trust the write: this is the only way to confirm the
        // register actually took the value, as opposed to the write silently landing on
        // a bit layout that does not mean what setChargeTargetVoltage() assumes it means.
        //
        // Cached rather than re-read on demand: this device's serial console is not a
        // reliable place to observe it. Board/PMIC init runs before the USB CDC console
        // finishes enumerating, so anything logged here can be sent to a listener that
        // does not exist yet -- confirmed the hard way, twice, chasing this exact line
        // across two reboots with nothing landing in the capture. GetChargeVoltageReg()
        // below exposes this same value over /debug instead, which has no such race.
        chg_v_reg_readback_ = ReadReg(XPOWERS_AXP2101_CHG_V_SET);
        ESP_LOGI(TAG, "CHG_V_SET (0x64) reads back as 0x%02x (want low 3 bits = 0x01 for 4.0V)",
                 chg_v_reg_readback_);

        SetBrightness(0);
    }

    // The boot-time readback of CHG_V_SET, cached because re-reading it live would put
    // one more raw (non-tolerant) I2C transaction on a bus this device already had to
    // learn to stop trusting -- see TryReadPowerStatus() below. This register does not
    // change after boot, so a cached snapshot is exactly as accurate as a fresh read.
    int GetChargeVoltageReg() const
    {
        return chg_v_reg_readback_;
    }

    int GetChargeVoltageRegStock() const
    {
        return chg_v_reg_stock_;
    }

    void SetBrightness(uint8_t brightness)
    {
        if (brightness == 0) {
            // DLDO1 off
            uint8_t val = ReadReg(0x90);
            WriteReg(0x90, val & 0x7F);
        } else {
            // 映射计算：将 1~100 映射到 寄存器值 20~28
            // 公式：MinReg + (input * (MaxReg - MinReg) / MaxInput)
            // 20 + (brightness * 8 / 100)
            if (brightness > 100) {
                brightness = 100;
            }
            uint8_t reg_val = 20 + ((uint16_t)brightness * 8 / 100);
            WriteReg(0x99, reg_val);

            // Make sure DLDO1 on
            uint8_t val = ReadReg(0x90);
            if (!(val & 0x80)) {
                WriteReg(0x90, val | 0x80);
            }
        }
    }

    /**
     * @brief Set charge current.
     * @param  opt: See xpowers_axp2101_chg_curr_t enum for details.
     * @retval
     */
    bool setChargerConstantCurr(uint8_t opt)
    {
        if (opt > XPOWERS_AXP2101_CHG_CUR_1000MA) {
            return false;
        }
        int val = ReadReg(XPOWERS_AXP2101_ICC_CHG_SET);
        if (val == -1) {
            return false;
        }
        val &= 0xE0;
        WriteReg(XPOWERS_AXP2101_ICC_CHG_SET, val | opt);
        return true;
    }

    /**
     * @brief Set charge target (constant-voltage) cutoff.
     * @param  opt: See xpowers_axp2101_chg_vol_t enum for details.
     * @retval
     */
    bool setChargeTargetVoltage(uint8_t opt)
    {
        if (opt > XPOWERS_AXP2101_CHG_VOL_4V4) {
            return false;
        }
        // No failure check on the read: ReadReg() returns uint8_t (never -1) and
        // ESP_ERROR_CHECKs internally on a real I2C fault, so there is no failure value
        // for this function to observe here -- a bus error aborts the device instead.
        // This is acceptable only because it runs once at boot, not on a recurring poll;
        // see the TryReadPowerStatus() comment below for why that distinction matters.
        uint8_t val = ReadReg(XPOWERS_AXP2101_CHG_V_SET);
        val &= 0xF8;
        WriteReg(XPOWERS_AXP2101_CHG_V_SET, val | opt);
        return true;
    }

    // A FAILED READ HERE MUST NOT BE FATAL.
    //
    // I2cDevice::ReadReg wraps the transfer in ESP_ERROR_CHECK, so a transient bus
    // timeout calls abort() and takes the whole device down. That is a reasonable
    // default for a one-off setup register and completely wrong for a poll that runs
    // every few seconds forever: this bus is shared with the touch panel, the IMU and
    // the IO expander, and under load -- audio, WiFi, a tailnet, someone shaking the
    // robot -- a 100 ms transaction occasionally loses the race.
    //
    // Observed exactly that: `ESP_ERR_TIMEOUT` (263) inside the esp_timer callback,
    // abort(), reboot, mid-conversation. Same shape as the FT6336 read above, which
    // already tolerates failure -- power state is a hint about a battery, not a
    // correctness invariant, and last known value is a perfectly good answer for one
    // more poll interval.
    bool TryReadPowerStatus(uint8_t* out)
    {
        esp_err_t err = TryReadRegs(0x01, out, 1);
        if (err == ESP_OK) {
            return true;
        }
        pmic_read_failures_++;
        int64_t now_us = esp_timer_get_time();
        if (last_pmic_error_log_us_ == 0 || (now_us - last_pmic_error_log_us_) >= 5000 * 1000) {
            ESP_LOGW(TAG, "AXP2101 read failed (%s), %lu total -- keeping last power state",
                     esp_err_to_name(err), static_cast<unsigned long>(pmic_read_failures_));
            last_pmic_error_log_us_ = now_us;
        }
        return false;
    }

    // ONE read of 0x01, every derived answer taken from that single byte.
    //
    // Every caller below used to do its own TryReadPowerStatus(), so a single sample of
    // "what is the battery doing" cost four reads of the same register -- four times the
    // traffic on a bus already shared with the touch panel, the IMU and the IO expander,
    // and four chances to time out instead of one.
    //
    // Worse than the waste: the four answers were taken at four different instants, so
    // they did not have to agree with each other. That is not hypothetical -- the
    // recorded history shows samples reporting charge phase "not charging" while the
    // current-direction bits in the very same sample said "charging", which is
    // impossible from one byte and entirely possible from two reads milliseconds apart.
    // Conclusions were drawn from that contradiction before its cause was understood.
    //
    // Short cache rather than an explicit snapshot object: it collapses a burst of
    // accessor calls into one transaction without changing any call site, and is still
    // far shorter than the 1 s poll that drives them.
    static constexpr int64_t kPowerStatusCacheUs = 250 * 1000;

    bool RefreshPowerStatus()
    {
        const int64_t now_us = esp_timer_get_time();
        if (power_status_valid_ && (now_us - last_power_status_us_) < kPowerStatusCacheUs) {
            return true;
        }

        uint8_t status = 0;
        if (!TryReadPowerStatus(&status)) {
            return power_status_valid_;   // keep the last known good byte
        }

        last_power_status_us_ = now_us;
        power_status_valid_   = true;

        const uint8_t current_direction = (status & 0b01100000) >> 5;
        const uint8_t phase             = status & 0b00000111;
        const bool is_charging_done     = phase == 0b100;

        last_charging_     = (current_direction == 1);
        last_discharging_  = (current_direction == 2);
        last_charge_phase_ = phase;
        // Treat any non-discharging state as externally powered so a plugged-in cable
        // still counts even after the battery is full.
        last_external_power_ = (current_direction != 2 || is_charging_done);
        return true;
    }

    bool IsExternalPowerConnected()
    {
        RefreshPowerStatus();
        return last_external_power_;
    }

    bool IsDischarging()
    {
        RefreshPowerStatus();
        return last_discharging_;
    }

    // Overrides Axp2101::IsCharging(), which goes straight through the raw,
    // ESP_ERROR_CHECK-wrapped ReadReg() and aborts the device on a transient I2C
    // timeout -- see TryReadPowerStatus() above. Now served from the shared snapshot.
    bool IsCharging()
    {
        RefreshPowerStatus();
        return last_charging_;
    }

    // Overrides Axp2101::GetBatteryLevel(), same reasoning: raw ReadReg(0xA4) on a poll
    // path aborts on a transient bus error instead of returning the last known level.
    int GetBatteryLevel()
    {
        uint8_t level = 0;
        esp_err_t err = TryReadRegs(0xA4, &level, 1);
        if (err != ESP_OK) {
            return last_battery_level_;
        }
        last_battery_level_ = level;
        return last_battery_level_;
    }

    // Bits [2:0] of the same power-status byte IsCharging()/IsDischarging() read, but
    // this is the detailed charger state machine (trickle/precharge/CC/CV/done), not
    // just current direction. Exists to answer "is it actually still bulk-charging past
    // the voltage cap, or just idling in the CV tail" without guessing from percentage
    // and a boolean -- percentage alone cannot distinguish those two, and got this
    // debugging session nowhere until this existed.
    int GetChargePhase()
    {
        RefreshPowerStatus();
        return last_charge_phase_;
    }

    // Raw battery-voltage ADC, in millivolts -- see the XPOWERS_AXP2101_ADC_DATA_RESULT0/1
    // comment above for where the formula came from. Two adjacent registers, read in one
    // transaction rather than two separate TryReadPowerStatus-style calls.
    // Deliberately reads the SAME quantity two different ways, because we do not yet
    // know whether the number is real.
    //
    // GetBatteryVoltageMv() does one transmit-receive for both bytes, which assumes the
    // AXP2101 auto-increments its register pointer across 0x34 -> 0x35. XPowersLib does
    // not assume that: it issues two separate single-byte reads. If this chip does not
    // auto-increment, the burst's second byte is garbage or a repeat of the first, which
    // would fabricate scatter around a value that is actually stable. Reporting both in
    // /debug turns that from an argument into a measurement -- if they disagree, the
    // burst read is the bug.
    int GetBatteryVoltageMvSplit()
    {
        uint8_t hi = 0, lo = 0;
        if (TryReadRegs(XPOWERS_AXP2101_ADC_DATA_RESULT0, &hi, 1) != ESP_OK ||
            TryReadRegs(XPOWERS_AXP2101_ADC_DATA_RESULT1, &lo, 1) != ESP_OK) {
            pmic_read_failures_++;
            return last_battery_mv_split_;
        }
        last_battery_mv_split_ = ((hi & 0x1F) << 8) | lo;
        return last_battery_mv_split_;
    }

    // Second consecutive split read, taken immediately after the first. A stable
    // quantity read twice in a row should agree; a large gap here means the reading
    // itself is unreliable, independent of which addressing style is used.
    int GetBatteryVoltageMvSplit2()
    {
        return GetBatteryVoltageMvSplit();
    }

    // The other thresholds this firmware never writes, so they sit at chip defaults.
    // Relevant to "it will not run without the cable": if min-system-voltage or the
    // power-off threshold sit high, the PMIC refuses to run on a pack a healthy cell
    // would drive fine -- a config problem, not a dead battery. Input voltage limit
    // (VINDPM) decides when the PMIC stops drawing from the cable and leans on the
    // battery instead.
    int GetRegRaw(uint8_t reg)
    {
        uint8_t val = 0;
        if (TryReadRegs(reg, &val, 1) != ESP_OK) {
            pmic_read_failures_++;
            return -1;
        }
        return val;
    }

    int GetBatteryVoltageMv()
    {
        uint8_t regs[2] = {0, 0};
        esp_err_t err = TryReadRegs(XPOWERS_AXP2101_ADC_DATA_RESULT0, regs, 2);
        if (err != ESP_OK) {
            pmic_read_failures_++;
            return last_battery_mv_;
        }
        last_battery_mv_ = ((regs[0] & 0x1F) << 8) | regs[1];
        return last_battery_mv_;
    }

    // Both cached the same way as everything else on this bus, and both read once per
    // /debug hit rather than on the UI poll path -- they are configuration, not state,
    // so they do not change between reads.
    int GetItermReg()
    {
        uint8_t val = 0;
        if (TryReadRegs(XPOWERS_AXP2101_ITERM_CHG_SET, &val, 1) != ESP_OK) {
            pmic_read_failures_++;
            return last_iterm_reg_;
        }
        last_iterm_reg_ = val;
        return last_iterm_reg_;
    }

    int GetStatus1Reg()
    {
        uint8_t val = 0;
        if (TryReadRegs(XPOWERS_AXP2101_STATUS1, &val, 1) != ESP_OK) {
            pmic_read_failures_++;
            return last_status1_reg_;
        }
        last_status1_reg_ = val;
        return last_status1_reg_;
    }

    int GetVbusIlimReg()
    {
        uint8_t val = 0;
        if (TryReadRegs(XPOWERS_AXP2101_INPUT_CUR_LIMIT, &val, 1) != ESP_OK) {
            pmic_read_failures_++;
            return last_vbus_ilim_reg_;
        }
        last_vbus_ilim_reg_ = val;
        return last_vbus_ilim_reg_;
    }

    uint32_t GetReadFailureCount() const
    {
        return pmic_read_failures_;
    }

private:
    // Defaults chosen so that a failure before the very first successful read reads as
    // "on external power, not discharging" -- the state in which nothing sleeps or
    // shuts down. Guessing wrong in the other direction could power the robot off.
    bool last_external_power_        = true;
    bool last_discharging_           = false;
    // Charging defaults to false rather than true: unlike the discharging default above,
    // there is no shutdown/sleep decision hanging off this one, so there is no "wrong
    // direction" to avoid -- false just means the plug/lightning icon starts in its
    // plainest state until the first real read comes in.
    bool last_charging_              = false;
    int last_battery_level_          = 100;
    int last_charge_phase_           = -1;   // -1 until the first successful read
    int last_battery_mv_             = 0;
    int last_battery_mv_split_       = 0;
    int last_iterm_reg_              = -1;
    int last_status1_reg_            = -1;
    int64_t last_power_status_us_    = 0;
    bool power_status_valid_         = false;
    int last_vbus_ilim_reg_          = -1;
    uint32_t pmic_read_failures_     = 0;
    int64_t last_pmic_error_log_us_  = 0;
    int chg_v_reg_readback_          = -1;   // -1 until the constructor's read completes
    int chg_v_reg_stock_             = -1;   // value found in 0x64 before we wrote it
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(Pmic* pmic) : pmic_(pmic)
    {
    }

    void SetBrightnessImpl(uint8_t brightness) override
    {
        pmic_->SetBrightness(target_brightness_);
        brightness_ = target_brightness_;
    }

private:
    Pmic* pmic_;
};

class Aw9523 : public I2cDevice {
public:
    // Exanpd IO Init
    Aw9523(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr)
    {
        WriteReg(0x02, 0b00000111);  // P0
        WriteReg(0x03, 0b10001111);  // P1
        WriteReg(0x04, 0b00011000);  // CONFIG_P0
        WriteReg(0x05, 0b00001100);  // CONFIG_P1
        WriteReg(0x11, 0b00010000);  // GCR P0 port is Push-Pull mode.
        WriteReg(0x12, 0b11111111);  // LEDMODE_P0
        WriteReg(0x13, 0b11111111);  // LEDMODE_P1
    }

    void ResetAw88298()
    {
        ESP_LOGI(TAG, "Reset AW88298");
        WriteReg(0x02, 0b00000011);
        vTaskDelay(pdMS_TO_TICKS(10));
        WriteReg(0x02, 0b00000111);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void ResetIli9342()
    {
        ESP_LOGI(TAG, "Reset IlI9342");
        WriteReg(0x03, 0b10000001);
        vTaskDelay(pdMS_TO_TICKS(20));
        WriteReg(0x03, 0b10000011);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
};

class Ft6336 : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x   = -1;
        int y   = -1;
    };

    Ft6336(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr)
    {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Ft6336()
    {
        delete[] read_buffer_;
    }

    bool UpdateTouchPoint()
    {
        auto err = TryReadRegs(0x02, read_buffer_, 6);
        if (err != ESP_OK) {
            tp_.num = 0;
            tp_.x   = -1;
            tp_.y   = -1;

            consecutive_failures_++;
            int64_t now_us = esp_timer_get_time();
            if (last_error_log_us_ == 0 || (now_us - last_error_log_us_) >= 1000 * 1000) {
                ESP_LOGW(TAG, "FT6336 read failed (%s), skipped %lu sample(s)", esp_err_to_name(err),
                         static_cast<unsigned long>(consecutive_failures_));
                last_error_log_us_ = now_us;
            }
            return false;
        }

        consecutive_failures_ = 0;
        tp_.num               = read_buffer_[0] & 0x0F;
        tp_.x                 = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y                 = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
        return true;
    }

    inline const TouchPoint_t& GetTouchPoint()
    {
        return tp_;
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
    int64_t last_error_log_us_     = 0;
    uint32_t consecutive_failures_ = 0;
};

class M5StackCoreS3Board : public StackChanNetBoard {
private:
    static constexpr int kPowerSaveSleepDelaySeconds = 300;
    static constexpr int kPowerStatePollIntervalMs   = 1000;

    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_;
    Aw9523* aw9523_;
    Ft6336* ft6336_;
    LvglDisplay* display_;
    StackChanCamera* camera_;
    esp_timer_handle_t touchpad_timer_;
    PowerSaveTimer* power_save_timer_;
    hal_bridge::XiaozhiConfig_t xiaozhi_config_;
    bool last_power_save_enabled_      = false;
    int64_t last_power_state_check_ms_ = 0;

    // ~2 minutes of history at the 1 s poll rate. Sized in internal RAM deliberately
    // small: this device has failed 8 KB allocations with 14.8 KB free, so a diagnostic
    // buffer has no business being generous. Oldest entry is evicted in place.
    static constexpr int kPowerSampleCount = 120;
    hal_bridge::PowerSample_t power_samples_[kPowerSampleCount] = {};
    int power_sample_head_  = 0;   ///< next slot to write
    int power_sample_count_ = 0;   ///< how many slots are populated (saturates at max)

    void RecordPowerSample()
    {
        auto& slot = power_samples_[power_sample_head_];
        slot.uptime_s    = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
        slot.mv          = static_cast<uint16_t>(pmic_->GetBatteryVoltageMv());
        slot.level       = static_cast<uint8_t>(pmic_->GetBatteryLevel());
        slot.phase       = static_cast<int8_t>(pmic_->GetChargePhase());
        slot.charging    = pmic_->IsCharging() ? 1 : 0;
        slot.discharging = pmic_->IsDischarging() ? 1 : 0;

        power_sample_head_ = (power_sample_head_ + 1) % kPowerSampleCount;
        if (power_sample_count_ < kPowerSampleCount) {
            power_sample_count_++;
        }
    }

    bool ShouldEnablePowerSave(bool has_external_power, bool is_discharging) const
    {
        return is_discharging || (has_external_power && xiaozhi_config_.allowShutdownWhenCharging);
    }

    void UpdatePowerSaveEnabled(bool has_external_power, bool is_discharging)
    {
        const bool should_enable_power_save = ShouldEnablePowerSave(has_external_power, is_discharging);
        if (should_enable_power_save == last_power_save_enabled_) {
            return;
        }

        ESP_LOGI(TAG, "Power save timer %s: external_power=%d, discharging=%d, allowShutdownWhenCharging=%d",
                 should_enable_power_save ? "enabled" : "disabled", has_external_power, is_discharging,
                 xiaozhi_config_.allowShutdownWhenCharging);
        power_save_timer_->SetEnabled(should_enable_power_save);
        last_power_save_enabled_ = should_enable_power_save;
    }

    void PollPowerSaveState()
    {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (last_power_state_check_ms_ != 0 && (now_ms - last_power_state_check_ms_) < kPowerStatePollIntervalMs) {
            return;
        }
        last_power_state_check_ms_ = now_ms;

        UpdatePowerSaveEnabled(pmic_->IsExternalPowerConnected(), pmic_->IsDischarging());
        RecordPowerSample();
    }

    void InitializePowerSaveTimer()
    {
        xiaozhi_config_ = hal_bridge::get_xiaozhi_config();

        const int seconds_to_shutdown = xiaozhi_config_.idleShutdownTimeSeconds > 0
                                            ? static_cast<int>(xiaozhi_config_.idleShutdownTimeSeconds)
                                            : -1;
        const int seconds_to_sleep    = seconds_to_shutdown == -1
                                            ? kPowerSaveSleepDelaySeconds
                                            : std::min(kPowerSaveSleepDelaySeconds, seconds_to_shutdown);

        ESP_LOGI(TAG, "Init power save timer: sleep=%d s, shutdown=%d s, allow_shutdown_when_charging=%d",
                 seconds_to_sleep, seconds_to_shutdown, xiaozhi_config_.allowShutdownWhenCharging);

        power_save_timer_ = new PowerSaveTimer(-1, seconds_to_sleep, seconds_to_shutdown);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            // GetBacklight()->SetBrightness(10);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() { pmic_->PowerOff(); });
        UpdatePowerSaveEnabled(pmic_->IsExternalPowerConnected(), pmic_->IsDischarging());
        RecordPowerSample();
    }

    void InitializeI2c()
    {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port          = (i2c_port_t)1,
            .sda_io_num        = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num        = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source        = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority     = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void I2cDetect()
    {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address       = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializeAxp2101()
    {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeAw9523()
    {
        ESP_LOGI(TAG, "Init AW9523");
        aw9523_ = new Aw9523(i2c_bus_, 0x58);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void PollTouchpad()
    {
        if (!ft6336_->UpdateTouchPoint()) {
            return;
        }
        auto& touch_point = ft6336_->GetTouchPoint();

        // Update hal touch point
        hal_bridge::set_touch_point(touch_point.num, touch_point.x, touch_point.y);
    }

    void InitializeFt6336TouchPad()
    {
        ESP_LOGI(TAG, "Init FT6336");
        ft6336_ = new Ft6336(i2c_bus_, 0x38);

        // 创建定时器，20ms 间隔
        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    M5StackCoreS3Board* board = (M5StackCoreS3Board*)arg;
                    board->PollTouchpad();
                    board->PollPowerSaveState();
                },
            .arg                   = this,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "touchpad_timer",
            .skip_unhandled_events = true,
        };

        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &touchpad_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(touchpad_timer_, 20 * 1000));
    }

    void InitializeSpi()
    {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num      = GPIO_NUM_37;
        buscfg.miso_io_num      = GPIO_NUM_NC;
        buscfg.sclk_io_num      = GPIO_NUM_36;
        buscfg.quadwp_io_num    = GPIO_NUM_NC;
        buscfg.quadhd_io_num    = GPIO_NUM_NC;
        buscfg.max_transfer_sz  = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeIli9342Display()
    {
        ESP_LOGI(TAG, "Init IlI9342");

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel       = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num                   = GPIO_NUM_3;
        io_config.dc_gpio_num                   = GPIO_NUM_35;
        io_config.spi_mode                      = 2;
        io_config.pclk_hz                       = 40 * 1000 * 1000;
        io_config.trans_queue_depth             = 10;
        io_config.lcd_cmd_bits                  = 8;
        io_config.lcd_param_bits                = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num             = GPIO_NUM_NC;
        panel_config.rgb_ele_order              = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel             = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        aw9523_->ResetIli9342();

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        // display_ = new StackChanLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
        //                                    DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        display_ = new StackChanAvatarDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
                                              DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeCamera()
    {
        ESP_LOGI(TAG, "Init Camera");

        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io =
                {
                    [0] = CAMERA_PIN_D0,
                    [1] = CAMERA_PIN_D1,
                    [2] = CAMERA_PIN_D2,
                    [3] = CAMERA_PIN_D3,
                    [4] = CAMERA_PIN_D4,
                    [5] = CAMERA_PIN_D5,
                    [6] = CAMERA_PIN_D6,
                    [7] = CAMERA_PIN_D7,
                },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io    = CAMERA_PIN_HREF,
            .pclk_io  = CAMERA_PIN_PCLK,
            .xclk_io  = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb  = false,
            .i2c_handle = i2c_bus_,
            .freq       = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin   = CAMERA_PIN_RESET,
            .pwdn_pin    = CAMERA_PIN_PWDN,
            .dvp_pin     = dvp_pin_config,
            .xclk_freq   = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new StackChanCamera(video_config);
        camera_->SetHMirror(false);
    }

public:
    M5StackCoreS3Board()
    {
        InitializeI2c();
        InitializeAxp2101();
        InitializePowerSaveTimer();
        InitializeAw9523();
        I2cDetect();
        InitializeSpi();
        InitializeIli9342Display();
        InitializeCamera();
        InitializeFt6336TouchPad();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override
    {
        static CoreS3AudioCodec audio_codec(i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                                            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_AW88298_ADDR,
                                            AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override
    {
        return display_;
    }

    virtual Camera* GetCamera() override
    {
        return camera_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override
    {
        static bool last_discharging = false;
        charging                     = pmic_->IsCharging();
        discharging                  = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override
    {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        StackChanNetBoard::SetPowerSaveLevel(level);
    }

    virtual Backlight* GetBacklight() override
    {
        static CustomBacklight backlight(pmic_);
        return &backlight;
    }

    i2c_master_bus_handle_t GetI2cBus()
    {
        return i2c_bus_;
    }

    int GetChargeVoltageReg()
    {
        return pmic_->GetChargeVoltageReg();
    }

    int GetChargePhase()
    {
        return pmic_->GetChargePhase();
    }

    int GetBatteryVoltageMv()
    {
        return pmic_->GetBatteryVoltageMv();
    }

    uint32_t GetPmicReadFailures()
    {
        return pmic_->GetReadFailureCount();
    }

    int GetItermReg()
    {
        return pmic_->GetItermReg();
    }

    int GetStatus1Reg()
    {
        return pmic_->GetStatus1Reg();
    }

    int GetVbusIlimReg()
    {
        return pmic_->GetVbusIlimReg();
    }

    int GetChargeVoltageRegStock()
    {
        return pmic_->GetChargeVoltageRegStock();
    }

    int GetBatteryVoltageMvSplit()
    {
        return pmic_->GetBatteryVoltageMvSplit();
    }

    int GetBatteryVoltageMvSplit2()
    {
        return pmic_->GetBatteryVoltageMvSplit2();
    }

    int GetRegRaw(int reg)
    {
        return pmic_->GetRegRaw(static_cast<uint8_t>(reg));
    }

    int CopyPowerSamples(hal_bridge::PowerSample_t* out, int max_out)
    {
        if (out == nullptr || max_out <= 0) {
            return 0;
        }
        const int n = std::min(max_out, power_sample_count_);
        // Walk back n slots from the write head so the caller gets them oldest-first,
        // which is the order anyone reading a trace expects.
        int idx = (power_sample_head_ - n + kPowerSampleCount * 2) % kPowerSampleCount;
        for (int i = 0; i < n; i++) {
            out[i] = power_samples_[idx];
            idx    = (idx + 1) % kPowerSampleCount;
        }
        return n;
    }
};

DECLARE_BOARD(M5StackCoreS3Board);

i2c_master_bus_handle_t hal_bridge::board_get_i2c_bus()
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.GetI2cBus();
}

int hal_bridge::board_get_charge_voltage_reg()
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.GetChargeVoltageReg();
}

int hal_bridge::board_get_charge_phase()
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.GetChargePhase();
}

int hal_bridge::board_get_battery_voltage_mv()
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.GetBatteryVoltageMv();
}

uint32_t hal_bridge::board_get_pmic_read_failures()
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.GetPmicReadFailures();
}

int hal_bridge::board_get_iterm_reg()
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.GetItermReg();
}

int hal_bridge::board_get_status1_reg()
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.GetStatus1Reg();
}

int hal_bridge::board_get_vbus_ilim_reg()
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.GetVbusIlimReg();
}

int hal_bridge::board_get_charge_voltage_reg_stock()
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.GetChargeVoltageRegStock();
}

int hal_bridge::board_get_battery_voltage_mv_split()
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.GetBatteryVoltageMvSplit();
}

int hal_bridge::board_get_battery_voltage_mv_split2()
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.GetBatteryVoltageMvSplit2();
}

int hal_bridge::board_get_pmic_reg(int reg)
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.GetRegRaw(reg);
}

int hal_bridge::board_copy_power_samples(hal_bridge::PowerSample_t* out, int max_out)
{
    auto& board = (M5StackCoreS3Board&)Board::GetInstance();
    return board.CopyPowerSamples(out, max_out);
}

StackChanCamera* hal_bridge::board_get_camera()
{
    auto& board = Board::GetInstance();
    auto camera = (StackChanCamera*)board.GetCamera();
    return camera;
}

int hal_bridge::board_get_battery_level()
{
    auto& board      = Board::GetInstance();
    int level        = 0;
    bool charging    = false;
    bool discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        return level;
    } else {
        return 100;
    }
}

bool hal_bridge::board_is_battery_charging()
{
    auto& board      = Board::GetInstance();
    int level        = 0;
    bool charging    = false;
    bool discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        return charging;
    } else {
        return false;
    }
}

bool hal_bridge::board_is_battery_discharging()
{
    auto& board      = Board::GetInstance();
    int level        = 0;
    bool charging    = false;
    bool discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        return discharging;
    } else {
        return false;
    }
}

void hal_bridge::board_set_backlight_brightness(uint8_t brightness, bool permanent)
{
    auto& board    = Board::GetInstance();
    auto backlight = board.GetBacklight();
    if (backlight) {
        backlight->SetBrightness(brightness, false);
        if (permanent) {
            Settings settings("display", true);
            settings.SetInt("brightness", brightness);
        }
    }
}

uint8_t hal_bridge::board_get_backlight_brightness()
{
    auto& board    = Board::GetInstance();
    auto backlight = board.GetBacklight();
    if (backlight) {
        return backlight->brightness();
    } else {
        return 0;
    }
}

void hal_bridge::board_set_speaker_volume(uint8_t volume, bool permanent)
{
    auto& board      = Board::GetInstance();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        Settings settings("audio", false);
        const int persisted_volume = settings.GetInt("output_volume", audio_codec->output_volume());
        audio_codec->SetOutputVolume(volume);
        if (!permanent) {
            Settings writable_settings("audio", true);
            writable_settings.SetInt("output_volume", persisted_volume);
            return;
        }
    }
}

uint8_t hal_bridge::board_get_speaker_volume()
{
    int volume = 70;
    Settings settings("audio", false);
    volume = settings.GetInt("output_volume", volume);
    if (volume <= 0) {
        volume = 10;
    }
    return volume;
}

bool hal_bridge::is_host_connected()
{
#if CONFIG_CONNECTION_TYPE_USB_NCM
    return WebsocketServerProtocol::IsHostConnected();
#else
    // Other transports dial out and are "connected" whenever the app thinks they are.
    auto& app = Application::GetInstance();
    return app.GetDeviceState() != kDeviceStateStarting;
#endif
}

void hal_bridge::end_conversation()
{
    // StopListening() rather than ToggleChatState(): toggling is state-dependent, and
    // the one moment this is called -- right after the agent finishes a goodbye -- is
    // exactly when the device may have already re-opened the turn on its own. A toggle
    // would then re-OPEN the session it was asked to close, which is the worst possible
    // outcome for a "goodbye" and would look like the robot refusing to let go.
    auto& app = Application::GetInstance();
    app.StopListening();
    app.SetDeviceState(kDeviceStateIdle);
}

hal_bridge::GatewayLink hal_bridge::gateway_link()
{
#if CONFIG_CONNECTION_TYPE_USB_NCM
    const uint32_t peer = WebsocketServerProtocol::HostPeerAddressV4();
    if (peer == 0) {
        return GatewayLink::None;
    }
    // Classified by subnet because that is the one fact the device can observe. The
    // USB-NCM link always hands the host 192.168.7.2 (the device is .1, and it runs
    // the DHCP server, so this is not a guess), and Tailscale hands out 100.64.0.0/10
    // by definition -- the CGNAT range it is required to use. Everything else is the
    // ordinary LAN, which is the right default: an unrecognised address is far more
    // likely to be a normal router than a mystery.
    if ((peer & 0xFFFFFF00u) == 0xC0A80700u) {   // 192.168.7.0/24
        return GatewayLink::Usb;
    }
    if ((peer & 0xFFC00000u) == 0x64400000u) {   // 100.64.0.0/10
        return GatewayLink::Vpn;
    }
    return GatewayLink::Wifi;
#else
    // Other transports dial out; there is no peer socket to interrogate, so fall back
    // to the binary answer rather than inventing a road.
    return is_host_connected() ? GatewayLink::Wifi : GatewayLink::None;
#endif
}

const char* hal_bridge::transport_label()
{
#if CONFIG_CONNECTION_TYPE_USB_NCM
    return "USB";
#elif CONFIG_CONNECTION_TYPE_USB_SLIP
    return "USB";
#else
    return "NET";
#endif
}

// A conversation needs a host on the other end -- this device does no inference of its
// own. Opening one with nobody attached lights the listening LED, captures the
// microphone and streams frames into a socket that does not exist, then falls back to
// idle seconds later with no explanation. That is worse than refusing: it looks like
// the robot heard you and chose to ignore you.
//
// Refusing is not silent: a chirp says "I felt that, and this is why nothing is going
// to happen". The LED is already red for exactly this reason, so it needs no flash --
// and this runs on the touch handler, where anything that blocks is felt directly as
// a laggy screen. An earlier version delayed 150 ms here to blink the LED and did
// precisely that.
static bool _refuse_if_no_host()
{
    if (hal_bridge::is_host_connected()) {
        return false;
    }
    ESP_LOGW(TAG, "face tap ignored -- no host connected");
    hal_bridge::app_play_sound(Lang::Sounds::OGG_EXCLAMATION);
    return true;
}

void hal_bridge::toggle_xiaozhi_chat_state()
{
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateStarting) {
        // EnterWifiConfigMode();
        return;
    }
    // Only guard the OPENING of a session. A tap while listening or speaking is a
    // deliberate interrupt, and must keep working even if the host has just vanished --
    // otherwise a dropped connection leaves the robot stuck talking with no way to
    // shut it up.
    if (app.GetDeviceState() == kDeviceStateIdle && _refuse_if_no_host()) {
        return;
    }
    app.ToggleChatState();
}

void hal_bridge::toggle_xiaozhi_chat_state_with_video()
{
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateStarting) {
        return;
    }
    if (app.GetDeviceState() == kDeviceStateIdle && _refuse_if_no_host()) {
        return;
    }
    app.ToggleChatStateWithVideo();
}

bool hal_bridge::is_mic_live()
{
    // Listening is the only state in which microphone audio leaves the device. Speaking
    // and idle do not capture, and saying otherwise on a privacy indicator would be
    // worse than having no indicator at all.
    return Application::GetInstance().GetDeviceState() == kDeviceStateListening;
}

// Written from the httpd task (the MCP handler) and read from the LVGL task, so atomic.
// Deliberately a timestamp rather than a flag: a flag would need someone to clear it,
// and "how long ago" is exactly what the blink duration is derived from.
static std::atomic<uint32_t> _last_camera_capture_ms{0};

void hal_bridge::note_camera_capture()
{
    _last_camera_capture_ms.store(GetHAL().millis(), std::memory_order_relaxed);
}

uint32_t hal_bridge::ms_since_camera_capture()
{
    const uint32_t then = _last_camera_capture_ms.load(std::memory_order_relaxed);
    if (then == 0) {
        return UINT32_MAX;      // nothing captured yet this boot
    }
    const uint32_t now = GetHAL().millis();
    return (now >= then) ? (now - then) : UINT32_MAX;
}

void hal_bridge::report_sensor_event(const char* event)
{
    if (event == nullptr) {
        return;
    }
    auto& app = Application::GetInstance();

    // Only while a client is actually listening. Outside a session there is nobody to
    // tell, and queueing these up would mean the robot opening a later conversation with
    // a report of everything that happened while it was alone.
    if (app.GetDeviceState() != kDeviceStateListening) {
        return;
    }

    // Rate limit. Petting fires repeatedly while a hand rests on the head, and shaking
    // fires on every jolt; unthrottled this would flood the link and bury the transcript
    // in the same event twice a second.
    static std::atomic<uint32_t> last_ms{0};
    const uint32_t now  = GetHAL().millis();
    const uint32_t then = last_ms.load(std::memory_order_relaxed);
    if (then != 0 && now - then < 4000) {
        return;
    }
    last_ms.store(now, std::memory_order_relaxed);

    app.SendSensorEvent(event);
}

bool hal_bridge::is_camera_live()
{
    auto& app = Application::GetInstance();
    return app.IsVideoSession() && app.GetDeviceState() == kDeviceStateListening;
}
