// voj-chan: WASM SystemInfo and Board identity implementation using browser localStorage
#include "system_info.h"
#include "wasm_board.h"
#include "wasm_camera.h"
#include <settings.h>
#include <emscripten.h>
#include <stdio.h>
#include <string>
#include <cstdlib>

std::string SystemInfo::GetMacAddress() {
    Settings settings("app_config", true);
    std::string mac = settings.GetString("mac_address", "");
    if (mac.empty()) {
        mac = "DA:E5:31:89:AB:CD";
        settings.SetString("mac_address", mac);
        printf("[WASM_SYSTEM_INFO] Generated and persisted virtual silicon MAC: %s\n", mac.c_str());
    }
    return mac;
}

std::string SystemInfo::GetChipModelName() {
    return "WASM-Emulator";
}

std::string SystemInfo::GetUserAgent() {
    return "wasm-emulator/1.4.3-wasm";
}

size_t SystemInfo::GetFlashSize() {
    return 16 * 1024 * 1024;
}

size_t SystemInfo::GetMinimumFreeHeapSize() {
    return 8 * 1024 * 1024;
}

size_t SystemInfo::GetFreeHeapSize() {
    return 8 * 1024 * 1024;
}

esp_err_t SystemInfo::PrintTaskCpuUsage(TickType_t xTicksToWait) {
    (void)xTicksToWait;
    return ESP_OK;
}

void SystemInfo::PrintTaskList() {}
void SystemInfo::PrintHeapStats() {}
void SystemInfo::PrintPmLocks() {}

WasmBoard::WasmBoard() {
    camera_ = new WasmCamera();
}

WasmBoard& WasmBoard::GetInstance() {
    static WasmBoard instance;
    return instance;
}

Camera* WasmBoard::GetCamera() {
    return camera_;
}

std::string WasmBoard::GetUuid() {
    Settings settings("app_config", true);
    std::string uuid = settings.GetString("uuid", "");
    if (uuid.empty()) {
        char* generated = (char*)EM_ASM_INT({
            var u = (typeof crypto !== 'undefined' && crypto.randomUUID) ? crypto.randomUUID() : '550e8400-e29b-41d4-a716-446655440000';
            var len = lengthBytesUTF8(u) + 1;
            var buf = _malloc(len);
            stringToUTF8(u, buf, len);
            return buf;
        });
        if (generated) {
            uuid = std::string(generated);
            free(generated);
        } else {
            uuid = "550e8400-e29b-41d4-a716-446655440000";
        }
        settings.SetString("uuid", uuid);
        printf("[WASM_BOARD] Generated and persisted virtual Client-ID UUID: %s\n", uuid.c_str());
    }
    return uuid;
}
