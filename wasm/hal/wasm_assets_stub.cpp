#include <assets.h>
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <unordered_map>

Assets::Assets() : partition_valid_(true) {}
Assets::~Assets() {}

bool Assets::Download(std::string url, std::function<void(int progress, size_t speed)> progress_callback) {
    return false;
}
bool Assets::Apply() {
    return true;
}

bool Assets::GetAssetData(const std::string& name, void*& ptr, size_t& size) {
    static std::unordered_map<std::string, std::vector<uint8_t>> s_asset_cache;

    auto it = s_asset_cache.find(name);
    if (it != s_asset_cache.end()) {
        ptr  = it->second.data();
        size = it->second.size();
        return true;
    }

    std::string path = "/assets_bin/" + name;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        printf("[WASM_ASSETS] Asset file not found in embedded VFS: '%s'\n", path.c_str());
        ptr  = nullptr;
        size = 0;
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(f);
        ptr  = nullptr;
        size = 0;
        return false;
    }

    std::vector<uint8_t> buf(fsize);
    size_t read_bytes = fread(buf.data(), 1, fsize, f);
    fclose(f);

    if (read_bytes != (size_t)fsize) {
        ptr  = nullptr;
        size = 0;
        return false;
    }

    s_asset_cache[name] = std::move(buf);
    ptr  = s_asset_cache[name].data();
    size = s_asset_cache[name].size();
    printf("[WASM_ASSETS] Successfully loaded embedded binary asset '%s' (%zu bytes)\n", name.c_str(), size);
    return true;
}

#include <jpg/image_to_jpeg.h>
extern "C" {
    bool image_to_jpeg_cb(uint8_t *src, size_t src_len, uint16_t width, uint16_t height,
                          v4l2_pix_fmt_t format, uint8_t quality, jpg_out_cb cb, void *arg) {
        return false;
    }
    bool image_to_jpeg(uint8_t *src, size_t src_len, uint16_t width, uint16_t height,
                       v4l2_pix_fmt_t format, uint8_t quality, uint8_t **out, size_t *out_len) {
        return false;
    }

    char _binary_0_ogg_start[] = {0}; char _binary_0_ogg_end[] = {0};
    char _binary_1_ogg_start[] = {0}; char _binary_1_ogg_end[] = {0};
    char _binary_2_ogg_start[] = {0}; char _binary_2_ogg_end[] = {0};
    char _binary_3_ogg_start[] = {0}; char _binary_3_ogg_end[] = {0};
    char _binary_4_ogg_start[] = {0}; char _binary_4_ogg_end[] = {0};
    char _binary_5_ogg_start[] = {0}; char _binary_5_ogg_end[] = {0};
    char _binary_6_ogg_start[] = {0}; char _binary_6_ogg_end[] = {0};
    char _binary_7_ogg_start[] = {0}; char _binary_7_ogg_end[] = {0};
    char _binary_8_ogg_start[] = {0}; char _binary_8_ogg_end[] = {0};
    char _binary_9_ogg_start[] = {0}; char _binary_9_ogg_end[] = {0};
    char _binary_activation_ogg_start[] = {0}; char _binary_activation_ogg_end[] = {0};
    char _binary_err_pin_ogg_start[] = {0}; char _binary_err_pin_ogg_end[] = {0};
    char _binary_err_reg_ogg_start[] = {0}; char _binary_err_reg_ogg_end[] = {0};
    char _binary_exclamation_ogg_start[] = {0}; char _binary_exclamation_ogg_end[] = {0};
    char _binary_low_battery_ogg_start[] = {0}; char _binary_low_battery_ogg_end[] = {0};
    char _binary_popup_ogg_start[] = {0}; char _binary_popup_ogg_end[] = {0};
    char _binary_success_ogg_start[] = {0}; char _binary_success_ogg_end[] = {0};
    char _binary_upgrade_ogg_start[] = {0}; char _binary_upgrade_ogg_end[] = {0};
    char _binary_vibration_ogg_start[] = {0}; char _binary_vibration_ogg_end[] = {0};
    char _binary_welcome_ogg_start[] = {0}; char _binary_welcome_ogg_end[] = {0};
    char _binary_wificonfig_ogg_start[] = {0}; char _binary_wificonfig_ogg_end[] = {0};
}
