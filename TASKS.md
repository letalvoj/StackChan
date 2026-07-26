# wasm-chan: Future Tasks & Deferred Features

Captured during architectural design sessions. Items are roughly priority-ordered within each category.

---

## P0 — Active / In Progress

- [ ] **UsbProtocol over ttyACM0** — New `Protocol` subclass for physical ESP32 serial communication.
- [ ] **gateway.py** — Unified transport-agnostic server with pluggable backends (`--mode=echo`, `--mode=gemini-api`) and pluggable transports (`--serial`, `--tcp`, `--websocket`).

---

## P1 — Near-Term Enhancements

- [ ] **Web Serial API browser simulation** — Create a WASM-side shim that connects to a local physical ESP32 via the browser's [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API). This would let the WASM dashboard act as a live debug console for a real device, bridging browser UI controls directly to physical hardware without `socat` or SSH tunnels. *Complexity:* Moderate — requires Chrome-only API, async read/write streams, and TLV frame parsing in JavaScript.
- [ ] **USB Composite Device: Mass Storage** — Expose ESP32 SPIFFS/LittleFS partition as a USB mass storage device alongside CDC protocol and console interfaces. Enables mounting the device's filesystem on the host machine for direct file inspection, config editing, and state tree visualization as a Unix file tree. *Dependency:* TinyUSB composite descriptor with CDC + MSC classes.
- [ ] **`make all` top-level build target** — Create a root-level `Makefile` in `wasm-chan/` that orchestrates both `idf.py build` (ESP32 native) and `make build` (WASM/Emscripten) to catch cross-platform regressions early.

---

## P2 — Future Architecture

- [ ] **gateway.py `--mode=gemini-api`** — Implement Gemini API backend for `gateway.py`, replacing echo mode with real LLM conversational turns. Audio → Gemini speech-to-text → Gemini chat completion → Gemini text-to-speech → audio response.
- [ ] **gateway.py `--mode=a2a`** — Agent-to-Agent (A2A) protocol backend for `gateway.py`, enabling StackChan to participate in multi-agent conversations as a physical embodied agent.
- [ ] **Multi-device gateway** — Support multiple ESP32 devices connected simultaneously via separate serial ports, each with independent session state and backend routing.
