#ifndef WASM_DISPLAY_H
#define WASM_DISPLAY_H

#include <display/display.h>
#include <stackchan/avatar_controller.h>

class WasmDisplay : public Display {
public:
    WasmDisplay();
    ~WasmDisplay() override;

    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetupUI() override;

protected:
    bool Lock(int timeout_ms = 0) override { return true; }
    void Unlock() override {}

private:
    AvatarController controller_;
    bool setup_ui_called_ = false;
};

#endif // WASM_DISPLAY_H
