/*
 * Face lab: render the REAL avatar, driven by the REAL modifiers.
 *
 * This compiles the same eyes.cpp / mouth.cpp the firmware runs, against the same LVGL,
 * and -- the part that matters most -- it runs BlinkModifier, SpeakingModifier,
 * BreathModifier and IdleExpressionModifier themselves rather than imitating them. A
 * preview that reimplements the behaviour it is previewing will eventually disagree with
 * the device, and this one did: it drew blinks the blink modifier never produces and
 * speaking mouths outside the band the speaking modifier emits.
 *
 * What made that reimplementation necessary was the shape of Modifiable, not laziness.
 * It handed out a concrete Motion, which owns two Servos, which reach the bus -- so
 * anything wanting to drive a modifier had to link the hardware. Modifiable now returns
 * motion::MotionControl, a behaviour-only interface, and the stub below satisfies it in
 * twenty lines. That is the whole reason this file can be thin.
 *
 * So the harness declares only *what to run and when to look*:
 *
 *   tiles        static parameter sweeps -- every emotion against every mouth opening,
 *                every lid position, every corner of the pupil's travel.
 *   performances a set of real modifiers, a stimulus, and a capture cadence.
 *
 * The clock is virtual and advances in lockstep with the LVGL tick, so two runs of one
 * commit produce identical frames and diff cleanly.
 *
 *   ./render_faces <outdir> [skin]
 */
#include <lvgl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../../firmware/main/stackchan/avatar/skins/chalk/chalk.h"
#include "../../firmware/main/stackchan/face_scene.h"
#include "../../firmware/main/stackchan/face_state.h"
#include "../../firmware/main/stackchan/modifiable.h"
#include "../../firmware/main/stackchan/modifiers/blink.h"
#include "../../firmware/main/stackchan/modifiers/breath.h"
#include "../../firmware/main/stackchan/modifiers/speaking.h"
#include "../../firmware/main/stackchan/modifiers/idle_expression.h"
#include "../../firmware/main/hal/hal.h"

using namespace stackchan;
using namespace stackchan::avatar;
namespace face = stackchan::face;

// The panel size is the firmware's, not the harness's. A face that fits here fits
// the device, because there is only one definition of how big the screen is.
static constexpr int kW = face::kScreenWidth;
static constexpr int kH = face::kScreenHeight;

// ------------------------------------------------------------------ virtual clock
//
// Decorators and modifiers time themselves off the HAL clock while LVGL lays out off its
// tick. When those two ran independently -- the clock reading CPU time, the tick advancing
// by a fixed step -- animation landed on a different frame every run and no two sheets
// could be compared. One counter drives both.

static std::uint32_t g_now_ms = 0;
static Hal g_hal;   ///< real object, so the modifiers' signal connections are valid

std::uint32_t Hal::millis()
{
    return g_now_ms;
}

Hal& GetHAL()
{
    return g_hal;
}

// ------------------------------------------------------------------ LVGL host display

static lv_color_t g_buf[kW * kH];
static uint8_t g_rgb[kW * kH * 3];

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    // Copy the flushed region into a full-frame RGB888 image we can write out. LVGL is in
    // partial mode, so this is called once per dirty rectangle -- exactly the behaviour
    // the firmware relies on, and worth exercising here rather than forcing full refresh.
    const int32_t w = area->x2 - area->x1 + 1;
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        for (int32_t x = area->x1; x <= area->x2; ++x) {
            const int32_t src = ((y - area->y1) * w + (x - area->x1)) * 2;
            const uint16_t p  = (uint16_t)(px_map[src] | (px_map[src + 1] << 8));
            uint8_t* d        = &g_rgb[(y * kW + x) * 3];
            d[0]              = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);   // R
            d[1]              = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);    // G
            d[2]              = (uint8_t)((p & 0x1F) * 255 / 31);           // B
        }
    }
    lv_display_flush_ready(disp);
}

static void write_bmp(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    const int row = kW * 3;
    const int pad = (4 - (row % 4)) % 4;
    const int img = (row + pad) * kH;
    const int off = 54;
    uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    const int total = off + img;
    memcpy(&hdr[2], &total, 4);
    memcpy(&hdr[10], &off, 4);
    const int ihdr = 40; memcpy(&hdr[14], &ihdr, 4);
    memcpy(&hdr[18], &kW, 4);
    const int h = -kH;                     // negative = top-down
    memcpy(&hdr[22], &h, 4);
    const uint16_t planes = 1, bpp = 24;
    memcpy(&hdr[26], &planes, 2);
    memcpy(&hdr[28], &bpp, 2);
    memcpy(&hdr[34], &img, 4);
    fwrite(hdr, 1, 54, f);
    const uint8_t zero[3] = {0, 0, 0};
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const uint8_t* s = &g_rgb[(y * kW + x) * 3];
            const uint8_t bgr[3] = {s[2], s[1], s[0]};
            fwrite(bgr, 1, 3, f);
        }
        if (pad) fwrite(zero, 1, pad, f);
    }
    fclose(f);
}

/* ========================================================================== */
/*                        a device without a device                           */
/* ========================================================================== */

/// A head that reports whatever it was last told, instantly. Enough for the modifiers
/// that read the pose -- which is how the gaze counter-swing gets exercised here.
class StubMotion : public motion::MotionControl {
public:
    uitk::Vector2i getCurrentAngles() override { return _angles; }
    int getCurrentYawAngle() override          { return _angles.x; }
    int getCurrentPitchAngle() override        { return _angles.y; }
    bool isMoving() override                   { return false; }

    void moveWithSpeed(int yaw, int pitch, int) override { _angles = {yaw, pitch}; }
    void moveYawWithSpeed(int yaw, int) override         { _angles.x = yaw; }
    void movePitchWithSpeed(int pitch, int) override     { _angles.y = pitch; }
    void goHome(int) override                            { _angles = {0, 0}; }
    void stop() override                                 {}

    void setModifyLock(bool locked) override { _locked = locked; }
    bool isModifyLocked() override           { return _locked; }
    void setTorqueEnabled(bool) override            {}
    void setAutoTorqueReleaseEnabled(bool) override {}
    void setAutoAngleSyncEnabled(bool) override     {}
    void update() override                          {}
    void zeroHere() override                        {}
    void resetZeroCalibration() override            {}

    /// Place the head directly, standing in for a servo that has finished moving.
    void place(int yaw, int pitch) { _angles = {yaw, pitch}; }

private:
    uitk::Vector2i _angles{0, 0};
    bool _locked = false;
};

/// NeonLight is already abstract; nothing about the face needs it to light up.
class StubNeon : public addon::NeonLight {
public:
    StubNeon() : NeonLight(1) {}

protected:
    void set_rgb_color_impl(uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void refresh_rgb_impl() override {}
};

/// The harness's stand-in for StackChan: the real avatar, a stub head, no hardware.
/// Its update() mirrors StackChan::update() so modifiers see the same call order.
class PreviewChan : public Modifiable {
public:
    explicit PreviewChan(Avatar& avatar) : _avatar(avatar) {}

    motion::MotionControl& motion() override { return _motion; }
    Avatar& avatar() override                { return _avatar; }
    bool hasAvatar() override                { return true; }
    addon::NeonLight& leftNeonLight() override  { return _neon; }
    addon::NeonLight& rightNeonLight() override { return _neon; }

    template <typename T, typename... Args>
    T& add(Args&&... args)
    {
        auto m = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *m;
        _modifiers.push_back(std::move(m));
        return ref;
    }
    void clearModifiers() { _modifiers.clear(); }

    void update()
    {
        for (auto& m : _modifiers) {
            m->_update(*this);
        }
        _avatar.update();
    }

    StubMotion& head() { return _motion; }

private:
    Avatar& _avatar;
    StubMotion _motion;
    StubNeon _neon;
    std::vector<std::unique_ptr<Modifier>> _modifiers;
};

/// Advance the clock, the modifiers, the avatar and LVGL together, one tick at a time.
static void advance(uint32_t ms, PreviewChan* chan,
                    const std::function<void(PreviewChan&, uint32_t)>& tick = {})
{
    const uint32_t step = 33;
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += step) {
        g_now_ms += step;
        lv_tick_inc(step);
        if (chan) {
            // The stimulus runs every tick, not once per captured frame. A head that
            // teleports between captures produces one delta spike and then nothing, which
            // is not what a servo does and would let a too-weak tracking gain look fine.
            if (tick) tick(*chan, g_now_ms);
            chan->update();
        }
        lv_timer_handler();
    }
    lv_refr_now(nullptr);
}

/* ========================================================================== */
/*                                   tiles                                    */
/* ========================================================================== */

/// A labelled still: one FaceState, and the matrix it belongs to.
struct Tile {
    std::string group;
    std::string name;
    face::FaceState state;
};

static const struct { const char* n; Emotion e; } kEmotions[] = {
    {"neutral", Emotion::Neutral}, {"happy", Emotion::Happy}, {"angry", Emotion::Angry},
    {"sad", Emotion::Sad},         {"doubt", Emotion::Doubt}, {"sleepy", Emotion::Sleepy},
    {"laugh", Emotion::Laugh},
};

static std::vector<Tile> buildTiles()
{
    std::vector<Tile> t;
    auto add = [&](const char* group, std::string name, face::FaceState f) { t.push_back({group, name, f}); };

    // --- the eighteen the artist drew, which is what compare.sh diffs against ------
    for (const auto& em : kEmotions) { face::FaceState f; f.emotion = em.e; add("main", em.n, f); }

    { face::FaceState p; p.eyeWeight = face::kBlinkClosedWeight; add("main", "blink", p); }
    { face::FaceState p; p.eyeWeight = face::kBlinkHalfWeight;   add("main", "halfblink", p); }
    { face::FaceState p; p.mouthWeight = face::kMouthSmall;  add("main", "speak-sm", p); }
    { face::FaceState p; p.mouthWeight = face::kMouthMedium; add("main", "speak-md", p); }
    { face::FaceState p; p.mouthWeight = face::kMouthWide;   add("main", "speak-lg", p); }
    { face::FaceState p; p.emotion = Emotion::Happy; p.mouthWeight = face::kMouthMedium; add("main", "happytalk", p); }

    { face::FaceState p; p.emotion = Emotion::Happy; p.reaction = face::Reaction::Affection; add("main", "heart", p); }
    { face::FaceState p; p.emotion = Emotion::Happy; p.reaction = face::Reaction::Bashful;   add("main", "shy", p); }
    { face::FaceState p; p.emotion = Emotion::Happy; p.reaction = face::Reaction::HeadPet; p.mouthWeight = face::kMouthMedium;
      add("main", "headpet", p); }
    { face::FaceState p; p.emotion = Emotion::Angry; p.reaction = face::Reaction::Irritation; add("main", "angrymark", p); }
    { face::FaceState p; p.emotion = Emotion::Sad;   p.reaction = face::Reaction::Discomfort; add("main", "sweat", p); }
    // Reproduces the shake handler: doubt brows, eyes hidden, the skin's own mouth hidden
    // in favour of the overlay's wavy one.
    { face::FaceState p; p.emotion = Emotion::Doubt; p.reaction = face::Reaction::Disoriented; p.hideEyes = true;
      add("main", "dizzy", p); }

    // --- every emotion against every mouth opening --------------------------------
    // Talking is not one face. The corners an emotion holds have to survive the jaw
    // opening, and that only shows up with all of them side by side.
    const struct { const char* n; int w; } kMouths[] = {
        {"rest", -1}, {"sm", face::kMouthSmall}, {"md", face::kMouthMedium}, {"lg", face::kMouthWide},
    };
    for (const auto& em : kEmotions) {
        for (const auto& m : kMouths) {
            face::FaceState p;
            p.emotion = em.e;
            p.mouthWeight = m.w;
            add("mouth", std::string(em.n) + "-" + m.n, p);
        }
    }

    // --- every emotion against every lid position ---------------------------------
    // A blink has to compose with the emotion underneath it rather than replacing it.
    const struct { const char* n; int w; } kLids[] = {
        {"open", -1}, {"half", face::kBlinkHalfWeight}, {"closed", face::kBlinkClosedWeight},
    };
    for (const auto& em : kEmotions) {
        for (const auto& l : kLids) {
            face::FaceState p;
            p.emotion = em.e;
            p.eyeWeight = l.w;
            add("eyes", std::string(em.n) + "-" + l.n, p);
        }
    }

    // --- the pupil across its whole travel ----------------------------------------
    // Corners included, because that is where a pupil escapes the white if the gaze
    // limit is wrong.
    const struct { const char* n; int x, y; } kGaze[] = {
        {"up-left", -100, -100},  {"up", 0, -100},   {"up-right", 100, -100},
        {"left", -100, 0},        {"centre", 0, 0},  {"right", 100, 0},
        {"down-left", -100, 100}, {"down", 0, 100},  {"down-right", 100, 100},
    };
    for (const auto& g : kGaze) {
        face::FaceState p;
        p.gaze = {g.x, g.y};
        add("gaze", g.n, p);
    }

    // --- odds and ends that had no home -------------------------------------------
    { face::FaceState p; p.eyePosition = {0, 95}; p.mouthPosition = {0, 95}; add("extra", "breath-peak", p); }
    { face::FaceState p; p.emotion = Emotion::Sleepy; p.speech = "Zzz..."; add("extra", "sleepy-speech", p); }
    { face::FaceState p; p.eyeWeight = face::kEyeRestWeight; p.mouthWeight = face::kMouthWide;
      add("extra", "dance-panic", p); }

    return t;
}

/* ========================================================================== */
/*                               performances                                 */
/* ========================================================================== */

/**
 * @brief A set of real modifiers, a stimulus, and a capture cadence.
 *
 * Note what is absent: any description of how a blink or a spoken syllable looks. That
 * lives in the modifiers, which is the point. The harness only says which of them to run
 * and how often to take a picture.
 */
struct Performance {
    std::string name;
    std::string caption;
    face::FaceState base;                           ///< emotion and any overlays
    std::function<void(PreviewChan&)> arm;          ///< construct the real modifiers
    std::function<void(PreviewChan&, uint32_t)> cue; ///< optional stimulus, run every tick
    uint32_t frame_ms = 120;                        ///< how often to capture
    int frames        = 12;
};

static Performance talking(const char* name, Emotion e, const char* caption)
{
    Performance p;
    p.name         = name;
    p.caption      = caption;
    p.base.emotion = e;
    // The real modifier at its real interval. It picks its own weights inside the bands
    // it actually emits, so a talking strip cannot show an opening the device never makes.
    p.arm      = [](PreviewChan& c) { c.add<SpeakingModifier>(0, 180, false); };
    p.frame_ms = 90;
    p.frames   = 14;
    return p;
}

static std::vector<Performance> buildPerformances()
{
    std::vector<Performance> v;

    // --- the blink, as BlinkModifier drives it ------------------------------------
    // Open holds are shortened from the modifier's 5.2 s so the loop is watchable; the
    // half and closed steps are its own.
    {
        Performance p;
        p.name     = "blink";
        p.caption  = "BlinkModifier, open interval shortened";
        p.arm      = [](PreviewChan& c) { c.add<BlinkModifier>(0, 700, 200); };
        p.frame_ms = 45;
        p.frames   = 26;
        v.push_back(p);
    }

    // --- talking, per emotional family --------------------------------------------
    // Same modifier every time; what differs is the mouth the emotion is holding while
    // the jaw works. Side by side these show whether an emotion survives being spoken.
    v.push_back(talking("talk-neutral", Emotion::Neutral, "SpeakingModifier, neutral"));
    v.push_back(talking("talk-happy",   Emotion::Happy,   "SpeakingModifier, happy"));
    v.push_back(talking("talk-angry",   Emotion::Angry,   "SpeakingModifier, angry"));
    v.push_back(talking("talk-sad",     Emotion::Sad,     "SpeakingModifier, sad"));
    v.push_back(talking("talk-doubt",   Emotion::Doubt,   "SpeakingModifier, doubt"));

    // --- the laugh, as laugh.sh compares it against the artist -----------------------
    // Nothing armed: setting the emotion is what starts the burst, so this is the firmware
    // playing the artist's performance on its own. Captured on every 33 ms update tick, the
    // burst's first frame lands on capture 0, which is what lets laugh.sh put each capture
    // beside the artist's frame at exactly the same millisecond.
    {
        Performance p;
        p.name         = "laugh";
        p.caption      = "Emotion::Laugh set, nothing else";
        p.base.emotion = Emotion::Laugh;
        p.frame_ms     = 33;
        p.frames       = 90;
        v.push_back(p);
    }
    // Talking while laughing: the speech amplitude walks the six laugh drawings, and each
    // one brings its own eyes and cheeks.
    {
        Performance p  = talking("talk-laugh", Emotion::Laugh, "SpeakingModifier, laughing");
        p.arm          = [](PreviewChan& c) {
            c.add<SpeakingModifier>(0, 180, false);
            c.add<BlinkModifier>(0, 900, 200);    // shut eyes must not pop open on a blink
        };
        p.frame_ms     = 120;
        p.frames       = 30;
        v.push_back(p);
    }

    // --- talking and blinking together --------------------------------------------
    // The two tracks own different features and must not fight. Running both is the only
    // way to see that a blink mid-sentence leaves the mouth alone.
    {
        Performance p;
        p.name    = "talk-and-blink";
        p.caption = "SpeakingModifier + BlinkModifier together";
        p.arm     = [](PreviewChan& c) {
            c.add<SpeakingModifier>(0, 180, false);
            c.add<BlinkModifier>(0, 600, 200);
        };
        p.frame_ms = 90;
        p.frames   = 18;
        v.push_back(p);
    }

    // --- the idle eye --------------------------------------------------------------
    // IdleExpressionModifier choosing its own saccades. Nothing here says where to look.
    {
        Performance p;
        p.name     = "idle-gaze";
        p.caption  = "IdleExpressionModifier picking its own targets";
        p.arm      = [](PreviewChan& c) { c.add<IdleExpressionModifier>(500, 1200); };
        p.frame_ms = 150;
        p.frames   = 20;
        v.push_back(p);
    }

    // --- the eyes against a moving head --------------------------------------------
    // The head is swept yaw-left to yaw-right while the idle modifier runs. What the
    // strip should show is the pupils swinging the *other* way and then catching up,
    // which is the whole point of the head-tracking term.
    {
        Performance p;
        p.name    = "head-counterlook";
        p.caption = "head sweeps right; pupils counter-swing and catch up";
        p.arm     = [](PreviewChan& c) { c.add<IdleExpressionModifier>(4000, 6000); };
        p.cue = [](PreviewChan& c, uint32_t now) {
            // A steady sweep across most of the yaw range, in servo units, advanced every
            // tick so the modifier sees the same gradual change a real servo produces.
            const uint32_t span = 1600;
            const uint32_t t    = now % (span * 2);
            const int progress  = (int)(t < span ? t : span * 2 - t);
            c.head().place(-450 + progress * 900 / (int)span, 0);
        };
        p.frame_ms = 100;
        p.frames   = 16;
        v.push_back(p);
    }

    // --- breathing ------------------------------------------------------------------
    {
        Performance p;
        p.name     = "breath";
        p.caption  = "BreathModifier, cycle shortened";
        p.arm      = [](PreviewChan& c) { c.add<BreathModifier>(0, 16, 3300, 150); };
        p.frame_ms = 275;
        p.frames   = 12;
        v.push_back(p);
    }

    // --- the decorators, which animate themselves ------------------------------------
    // No modifiers and no stimulus: only the clock advances, so what the strip shows is
    // each overlay's own motion at its own interval.
    auto overlay = [](const char* name, const char* caption, face::FaceState base, uint32_t ms, int n) {
        Performance p;
        p.name     = name;
        p.caption  = caption;
        p.base     = base;
        p.frame_ms = ms;
        p.frames   = n;
        return p;
    };
    { face::FaceState p; p.emotion = Emotion::Happy; p.reaction = face::Reaction::Affection;
      v.push_back(overlay("deco-heart", "heart, beating", p, 500, 6)); }
    { face::FaceState p; p.emotion = Emotion::Sad; p.reaction = face::Reaction::Discomfort;
      v.push_back(overlay("deco-sweat", "sweat, running once", p, 700, 6)); }
    { face::FaceState p; p.emotion = Emotion::Doubt; p.reaction = face::Reaction::Disoriented; p.hideEyes = true;
      v.push_back(overlay("deco-dizzy", "spirals, spinning", p, 300, 12)); }
    { face::FaceState p; p.emotion = Emotion::Angry; p.reaction = face::Reaction::Irritation;
      v.push_back(overlay("deco-angrymark", "anger mark, twitching", p, 500, 6)); }

    // --- the artist's six-panel performance, beat for beat ---------------------------
    // independent-motion.gif runs six faces side by side for 5.4 s. Reproducing it at the
    // same cadence is the only way to compare nuance against the source rather than
    // against a memory of it: every one of these is one panel of that reference.
    {
        struct Panel {
            const char* name;
            Emotion emotion;
            bool talking;
            face::Reaction reaction;
        };
        const Panel kPanels[] = {
            {"astra-idle",      Emotion::Neutral, false, face::Reaction::None},
            {"astra-curious",   Emotion::Doubt,   false, face::Reaction::None},
            {"astra-delighted", Emotion::Happy,   true,  face::Reaction::None},
            {"astra-grumpy",    Emotion::Angry,   true,  face::Reaction::None},
            {"astra-sad",       Emotion::Sad,     true,  face::Reaction::None},
            {"astra-petted",    Emotion::Happy,   true,  face::Reaction::HeadPet},
        };
        for (const Panel& panel : kPanels) {
            Performance q;
            q.name         = panel.name;
            q.caption      = "astra reference panel";
            q.base.emotion = panel.emotion;
            q.base.reaction = panel.reaction;
            // Idle panels still blink and look around; talking ones also work the jaw.
            const bool talking = panel.talking;
            q.arm = [talking](PreviewChan& c) {
                c.add<BlinkModifier>(0, 1400, 200);
                c.add<IdleExpressionModifier>(700, 1600);
                if (talking) {
                    c.add<SpeakingModifier>(0, 180, false);
                }
            };
            // 120 ms, not 180. The speaking modifier works the jaw every 180 ms, so
            // capturing on that same interval phase-locks to it and samples the closed
            // beat every single time -- the face looks like it never opens its mouth.
            // 120 drifts through the cycle and 45 of them is the reference's own 5.4 s.
            q.frame_ms = 120;
            q.frames   = 45;
            v.push_back(q);
        }
    }

    // --- petted: happy face, open mouth, heart and blush, all at once ---------------
    {
        face::FaceState p;
        p.emotion = Emotion::Happy;
        p.reaction = face::Reaction::HeadPet;
        Performance q = overlay("headpet", "petted, with the speaking mouth running", p, 120, 14);
        q.arm = [](PreviewChan& c) { c.add<SpeakingModifier>(0, 180, false); };
        v.push_back(q);
    }

    return v;
}

/* ========================================================================== */

int main(int argc, char** argv)
{
    const std::string out = (argc > 1) ? argv[1] : ".";

    lv_init();
    lv_display_t* disp = lv_display_create(kW, kH);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, g_buf, nullptr, sizeof(g_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // "chalk" is the shipping face; "default" is the original, kept as a reference to
    // diff against. Both satisfy the same Avatar interface, which is the property under
    // test as much as the drawing is.
    const std::string skinName = (argc > 2) ? argv[2] : "chalk";
    const face::Skin skin = (skinName == "default") ? face::Skin::Default : face::Skin::Chalk;
    if (skin == face::Skin::Chalk) {
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    }
    std::unique_ptr<Avatar> avatar = face::createAvatar(scr, skin);

    PreviewChan chan(*avatar);

    FILE* manifest = fopen((out + "/manifest.txt").c_str(), "w");
    // order.txt is what the original contact sheet reads, and it only ever wanted the
    // eighteen drawn faces. Kept so grid.sh needs no knowledge of the rest.
    FILE* order = fopen((out + "/order.txt").c_str(), "w");

    for (const Tile& t : buildTiles()) {
        t.state.applyTo(*avatar);
        std::vector<int> decos = face::attachReaction(*avatar, lv_screen_active(), t.state.reaction);
        avatar->update();
        advance(264, nullptr);

        const std::string file = t.group + "-" + t.name + ".bmp";
        write_bmp(out + "/" + file);
        fprintf(manifest, "tile|%s|%s|%s\n", t.group.c_str(), t.name.c_str(), file.c_str());
        if (t.group == "main") {
            // grid.sh expects the bare name, both in the listing and as the filename.
            write_bmp(out + "/" + t.name + ".bmp");
            fprintf(order, "%s\n", t.name.c_str());
        }

        face::detachReaction(*avatar, decos);
        avatar->update();
        advance(66, nullptr);
    }
    fclose(order);

    for (const Performance& p : buildPerformances()) {
        p.base.applyTo(*avatar);
        std::vector<int> decos = face::attachReaction(*avatar, lv_screen_active(), p.base.reaction);
        chan.clearModifiers();
        if (p.arm) {
            p.arm(chan);
        }

        fprintf(manifest, "seq|%s|%s\n", p.name.c_str(), p.caption.c_str());
        for (int i = 0; i < p.frames; ++i) {
            // Let the modifiers run for a frame's worth of time, then look. Capturing
            // after the interval rather than before is what makes a self-timed animation
            // show a different frame each beat.
            advance(p.frame_ms, &chan, p.cue);

            char file[256];
            snprintf(file, sizeof(file), "seq-%s-%02d.bmp", p.name.c_str(), i);
            write_bmp(out + "/" + file);
            fprintf(manifest, "frame|%s|%s|%u\n", p.name.c_str(), file, p.frame_ms);
        }

        chan.clearModifiers();
        face::detachReaction(*avatar, decos);
        avatar->update();
        advance(66, nullptr);
    }

    // ---- every drawn family in the pack ------------------------------------------
    // Iterated from the generated registry rather than a list kept here, so a family the
    // artist adds cannot be silently left out of review.
    if (skin == face::Skin::Chalk) {
        // Sleepy is the one emotion the reference draws bare -- no brows, no cheeks -- so
        // selecting it clears everything the avatar owns, and hiding the two Features
        // clears the rest. A family then appears on an empty panel, by itself.
        avatar->setEmotion(Emotion::Sleepy);
        avatar->leftEye().setVisible(false);
        avatar->rightEye().setVisible(false);
        avatar->mouth().setVisible(false);

        for (int i = 0; i < chalk::kAllClipCount; ++i) {
            const chalk::NamedClip& entry = chalk::kAllClips[i];
            ClipTrack track(lv_screen_active(), 2);

            fprintf(manifest, "seq|family-%s|drawn family, artist timing\n", entry.name);
            for (uint8_t f = 0; f < entry.clip->frameCount; ++f) {
                track.hold(entry.clip, f);
                advance(66, nullptr);

                char file[256];
                snprintf(file, sizeof(file), "fam-%s-%02u.bmp", entry.name, f);
                write_bmp(out + "/" + file);
                fprintf(manifest, "frame|family-%s|%s|%u\n", entry.name, file,
                        entry.clip->holdMs[f]);
            }
            track.setVisible(false);
            advance(33, nullptr);
        }
    }

    fclose(manifest);
    printf("wrote %s/manifest.txt\n", out.c_str());
    return 0;
}
