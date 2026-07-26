#ifndef WASM_CAMERA_H
#define WASM_CAMERA_H

#include <camera.h>
#include <string>
#include <vector>
#include <stdexcept>

class WasmCamera : public Camera {
private:
    std::string explain_url_;
    std::string explain_token_;
    bool hmirror_ = false;
    bool vflip_ = false;
    std::vector<uint8_t> jpeg_buffer_;
    std::vector<uint8_t> rgb565_buffer_;
    uint16_t width_ = 320;
    uint16_t height_ = 240;

public:
    WasmCamera();
    ~WasmCamera() = default;

    void SetExplainUrl(const std::string& url, const std::string& token) override;
    bool Capture() override;
    bool SetHMirror(bool enabled) override;
    bool SetVFlip(bool enabled) override;
    std::string Explain(const std::string& question) override;

    uint16_t GetWidth() const { return width_; }
    uint16_t GetHeight() const { return height_; }
};

#endif // WASM_CAMERA_H
