#ifndef WASM_BOARD_H
#define WASM_BOARD_H

#include <string>

class Camera;

class WasmBoard {
private:
    Camera* camera_ = nullptr;

public:
    WasmBoard();
    static WasmBoard& GetInstance();

    std::string GetUuid();
    Camera* GetCamera();
};

using Board = WasmBoard;

#endif // WASM_BOARD_H
