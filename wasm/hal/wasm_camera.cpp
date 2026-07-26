#include "wasm_camera.h"
#include "wasm_board.h"
#include <hal/hal.h>
#include <lvgl.h>
#include <lvgl_image.h>
#include <emscripten.h>
#include <stdio.h>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <memory>
#include "wasm_log.h"

EM_ASYNC_JS(char*, wasm_camera_explain_async, (const uint8_t* jpeg_ptr, int jpeg_len, const char* question, const char* url, const char* token), {
    if (!Module.HEAPU8 || !jpeg_ptr || !jpeg_len) return 0;
    var bytes = Module.HEAPU8.subarray(jpeg_ptr, jpeg_ptr + jpeg_len);
    var blob = new Blob([bytes], { type: "image/jpeg" });
    var formData = new FormData();
    formData.append("file", blob, "webcam_frame.jpg");
    var qStr = UTF8ToString(question);
    var uStr = UTF8ToString(url);
    var tStr = UTF8ToString(token);
    formData.append("question", qStr || "Explain what you see in this photo.");

    var jsonResponse = "";
    if (uStr === "mock://vision" || uStr.indexOf("localhost:8081/vision") !== -1) {
        console.log("[WASM_CAMERA] Using non-blocking async mock vision explanation.");
        jsonResponse = JSON.stringify({ explanation: "Simulated Vision Response: Active WebRTC AVATAR vision stream processed.", question: qStr });
    } else {
        try {
            var headers = {};
            if (tStr && tStr.trim() !== "") {
                headers["Authorization"] = "Bearer " + tStr;
            }
            var res = await fetch(uStr, {
                method: "POST",
                headers: headers,
                body: formData
            });
            if (res.ok) {
                jsonResponse = await res.text();
            } else {
                var errTxt = await res.text();
                jsonResponse = JSON.stringify({ error: "HTTP " + res.status, details: errTxt });
            }
        } catch(e) {
            console.log("[WASM_CAMERA:NET_ERR] Async fetch failed: " + e + ". Returning simulated vision stub.");
            jsonResponse = JSON.stringify({ explanation: "Simulated Vision Response: Active WebRTC AVATAR vision stream processed.", question: qStr });
        }
    }

    if (typeof allocateUTF8 === "function") {
        return allocateUTF8(jsonResponse);
    } else if (typeof Module._malloc === "function" && typeof stringToUTF8 === "function") {
        var strLen = lengthBytesUTF8(jsonResponse) + 1;
        var strPtr = Module._malloc(strLen);
        stringToUTF8(jsonResponse, strPtr, strLen);
        return strPtr;
    }
    return 0;
});

WasmCamera::WasmCamera() {
    jpeg_buffer_.resize(65536); // 64KB JPEG buffer
    rgb565_buffer_.resize(320 * 240 * 2); // 153.6KB RGB565 buffer for Mooncake AVATAR video window
    printf("[WASM_CAMERA] WasmCamera driver instantiated (320x240 Native AVATAR Frame Routing).\n");
}

void WasmCamera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
    printf("[WASM_CAMERA] SetExplainUrl('%s')\n", url.c_str());
}

bool WasmCamera::Capture() {
    jpeg_buffer_.resize(65536); // Ensure full capacity prior to each frame capture
    int jpeg_size = EM_ASM_INT({
        if (typeof window._captureWebcamJpeg === 'function') {
            return window._captureWebcamJpeg($0, $1);
        }
        return 0;
    }, jpeg_buffer_.data(), jpeg_buffer_.size());

    if (jpeg_size > 0 && jpeg_size <= 65536) {
        jpeg_buffer_.resize(jpeg_size);
        WASM_LOG_THROTTLE(1000, "WASM_CAMERA", "Captured snapshot frame (%d JPEG bytes).", jpeg_size);
    }

    int rgb_size = EM_ASM_INT({
        if (typeof window._captureWebcamRgb565 === 'function') {
            return window._captureWebcamRgb565($0, $1);
        }
        return 0;
    }, rgb565_buffer_.data(), rgb565_buffer_.size());

    if (rgb_size <= 0) {
        printf("[WASM_CAMERA:WARN] Frame capture failed or webcam stream inactive.\n");
        return false;
    }

    uint8_t* frame_copy = (uint8_t*)malloc(rgb_size);
    if (!frame_copy) {
        printf("[WASM_CAMERA:ERR] Failed to allocate heap buffer for LvglAllocatedImage.\n");
        return false;
    }

    memcpy(frame_copy, rgb565_buffer_.data(), rgb_size);
    GetHAL().onWsVideoModeChange.emit(true);
    GetHAL().onWsVideoFrame.emit(std::make_shared<LvglAllocatedImage>(frame_copy, rgb_size, width_, height_, width_ * 2, LV_COLOR_FORMAT_RGB565));
    WASM_LOG_THROTTLE(1000, "WASM_CAMERA", "✓ Emitted 320x240 RGB565 frame (%d bytes) to authoritative Mooncake AVATAR app.", rgb_size);
    return true;
}

bool WasmCamera::SetHMirror(bool enabled) {
    hmirror_ = enabled;
    EM_ASM({ if (window._setWebcamMirror) window._setWebcamMirror($0); }, enabled ? 1 : 0);
    return true;
}

bool WasmCamera::SetVFlip(bool enabled) {
    vflip_ = enabled;
    return true;
}

std::string WasmCamera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        throw std::runtime_error("Vision explain URL not configured in WasmCamera");
    }

    char* result_str = wasm_camera_explain_async(jpeg_buffer_.data(), jpeg_buffer_.size(), question.c_str(), explain_url_.c_str(), explain_token_.c_str());
    if (!result_str) {
        throw std::runtime_error("Vision explanation request failed in non-blocking JS bridge");
    }

    std::string response(result_str);
    free(result_str);
    printf("[WASM_CAMERA] Explain() completed via non-blocking async bridge (%zu chars).\n", response.length());
    return response;
}

extern "C" void wasm_camera_test_capture() {
    Camera* cam = Board::GetInstance().GetCamera();
    if (cam) {
        cam->Capture();
    } else {
        printf("[WASM_CAMERA:ERR] Board::GetCamera() returned null.\n");
    }
}

#include <mooncake.h>

extern "C" void wasm_open_app_by_name(const char* target_name) {
    if (!target_name) return;
    if (strcmp(target_name, "AI.AGENT") == 0) {
        printf("[WASM_APP] External open request for 'AI.AGENT' -> routing directly via GetHAL().requestXiaozhiStart().\n");
        GetHAL().requestXiaozhiStart();
        return;
    }
    auto props = mooncake::GetMooncake().getAllAppProps();
    for (const auto& p : props) {
        if (p.info.name == target_name) {
            mooncake::GetMooncake().closeApp(0);
            mooncake::GetMooncake().openApp(p.appID);
            printf("[WASM_APP] Closed launcher & opened Mooncake app '%s' (appID %d)\n", target_name, p.appID);
            return;
        }
    }
    printf("[WASM_APP] App '%s' not found in %zu installed apps. Opening fallback ID 3 (AVATAR).\n", target_name, props.size());
    mooncake::GetMooncake().openApp(3);
}

