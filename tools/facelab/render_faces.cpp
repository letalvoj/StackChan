/*
 * Face lab: render the REAL avatar skin natively and dump PNG-able frames.
 *
 * The point is that this compiles the same eyes.cpp / mouth.cpp the firmware runs, against
 * the same LVGL, rather than approximating the face in a drawing script. A preview that
 * does not share the code cannot tell you whether the face you designed is the face the
 * robot will show.
 *
 * Writes 24-bit BMPs (trivial to emit with no dependencies); the driver script converts
 * and tiles them with ImageMagick.
 *
 *   ./render_faces <outdir>
 */
#include <lvgl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "../../firmware/main/stackchan/avatar/skins/default/default.h"
#include "../../firmware/main/stackchan/avatar/skins/cute/cute.h"
#include "../../firmware/main/stackchan/avatar/decorators/decorators.h"
#include "../../firmware/main/hal/hal.h"

using namespace stackchan::avatar;

static constexpr int kW = 320;
static constexpr int kH = 240;

// ------------------------------------------------------------------ minimal HAL stub
//
// The decorators are the only avatar code that reaches outside the skin, and all they
// want is a millisecond clock for their animation timers. Rather than link the real HAL
// (which drags in servos, FreeRTOS, power management and a display driver, none of which
// a static face render needs) we satisfy exactly the two symbols the linker asks for.
//
// millis() touches no members, so the storage below is never actually read as a Hal --
// it exists only to hand back a reference of the right type.
std::uint32_t Hal::millis()
{
    return (std::uint32_t)(clock() * 1000ull / CLOCKS_PER_SEC);
}

alignas(16) static unsigned char g_hal_storage[4096];

Hal& GetHAL()
{
    return *reinterpret_cast<Hal*>(g_hal_storage);
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

/// Settle LVGL: several handler passes so animations/layout converge before capture.
static void settle()
{
    for (int i = 0; i < 30; ++i) {
        lv_tick_inc(33);
        lv_timer_handler();
    }
    lv_refr_now(nullptr);
}

// Which overlay to attach, if any. These are NOT emotions -- they are separate Decorator
// objects the modifiers add on top of the face (head-pet shows heart + shy, the IMU shows
// dizzy), and leaving them out of the grid meant a third of what the robot actually
// displays was never being reviewed.
enum class Deco { None, Heart, Shy, Angry, Sweat, Dizzy };

struct Shot {
    std::string name;
    Emotion emotion;
    int eye_weight;    // -1 = leave at the emotion's own value
    int mouth_weight;
    Deco deco = Deco::None;
};

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

    // Which skin: "default" (the shipping face) or "cute" (the redesign). Both satisfy
    // the same Avatar interface, which is exactly the property under test.
    const std::string skin = (argc > 2) ? argv[2] : "default";
    std::unique_ptr<Avatar> avatar;
    if (skin == "cute") {
        auto a = std::make_unique<CuteAvatar>();
        a->init(scr);
        lv_obj_set_style_bg_color(scr, a->secondaryColor, 0);
        avatar = std::move(a);
    } else {
        auto a = std::make_unique<DefaultAvatar>();
        a->init(scr);
        avatar = std::move(a);
    }

    // The emotion set the firmware actually has, plus the two mouth/eye extremes that the
    // modifiers drive at runtime (blink and speaking), because those are half of what the
    // face does in practice and a static emotion grid would miss them entirely.
    const std::vector<Shot> shots = {
        {"neutral",  Emotion::Neutral, -1, -1},
        {"happy",    Emotion::Happy,   -1, -1},
        {"angry",    Emotion::Angry,   -1, -1},
        {"sad",      Emotion::Sad,     -1, -1},
        {"doubt",    Emotion::Doubt,   -1, -1},
        {"sleepy",   Emotion::Sleepy,  -1, -1},
        {"blink",    Emotion::Neutral,  0, -1},
        {"halfblink",Emotion::Neutral, 32, -1},
        {"speak-sm", Emotion::Neutral, -1, 30},
        {"speak-md", Emotion::Neutral, -1, 65},
        {"speak-lg", Emotion::Neutral, -1, 100},
        {"happytalk",Emotion::Happy,   -1, 80},

        // Decorators. head-pet shows heart+shy together, so that combination gets its own
        // tile -- it is what you actually see when you rub the robot's head.
        {"deco:heart",     Emotion::Happy,   -1, -1, Deco::Heart},
        {"deco:shy",       Emotion::Happy,   -1, -1, Deco::Shy},
        {"headpet",        Emotion::Happy,   -1, -1, Deco::Heart},   // + shy, added below
        {"deco:angrymark", Emotion::Angry,   -1, -1, Deco::Angry},
        {"deco:sweat",     Emotion::Sad,     -1, -1, Deco::Sweat},
        {"deco:dizzy",     Emotion::Doubt,   -1, -1, Deco::Dizzy},
    };

    for (const auto& s : shots) {
        avatar->setEmotion(s.emotion);

        std::vector<int> decos;
        auto add = [&](Deco d) {
            lv_obj_t* p = lv_screen_active();
            switch (d) {
                case Deco::Heart: decos.push_back(avatar->addDecorator(std::make_unique<HeartDecorator>(p, 0, 0))); break;
                case Deco::Shy:   decos.push_back(avatar->addDecorator(std::make_unique<ShyDecorator>(p, 0)));      break;
                case Deco::Angry: decos.push_back(avatar->addDecorator(std::make_unique<AngryDecorator>(p, 0)));    break;
                case Deco::Sweat: decos.push_back(avatar->addDecorator(std::make_unique<SweatDecorator>(p, 0)));    break;
                case Deco::Dizzy: decos.push_back(avatar->addDecorator(std::make_unique<DizzyDecorator>(p, 0)));    break;
                case Deco::None:  break;
            }
        };
        add(s.deco);
        if (s.name == "headpet") {
            add(Deco::Shy);          // head-pet shows both at once
        }
        if (s.eye_weight >= 0) {
            avatar->leftEye().setWeight(s.eye_weight);
            avatar->rightEye().setWeight(s.eye_weight);
        }
        if (s.mouth_weight >= 0) {
            avatar->mouth().setWeight(s.mouth_weight);
        }
        avatar->update();
        settle();
        std::string safe = s.name;
        for (auto& c : safe) if (c == ':') c = '_';
        write_bmp(out + "/" + safe + ".bmp");
        printf("%s\n", s.name.c_str());

        for (int id : decos) {
            avatar->removeDecorator(id);
        }
        avatar->update();
        settle();
    }
    return 0;
}
