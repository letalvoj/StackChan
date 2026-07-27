# wasm-chan: Future Tasks & Deferred Features

Captured during architectural design sessions. Items are roughly priority-ordered within each category.

---

## P0 — Active / In Progress

- [x] **UsbProtocol over ttyACM0** — `Protocol` subclass over TinyUSB CDC-ACM. Enumerates,
      handshakes, SLIP framing, selected by `CONFIG_CONNECTION_TYPE_USB` (on by default).
      See `ARCHITECTURE.md` §5. **Audio is not yet working end to end** — see next item.
- [ ] **Gateway must decode Opus** — The device advertises and sends Opus
      (`audio_service.cc` opens the Opus encoder unconditionally), but
      `wasm/gateway/backends/echo.py` and `gemini_api.py` unpack raw PCM16, and
      `ParseServerHello` never negotiates `format`. Audio is noise in both directions
      until this is closed. Python-side only; do **not** "fix" it by making the device
      advertise PCM, which would fork it from real firmware behaviour.
- [ ] **gateway.py** — Unified transport-agnostic server with pluggable backends (`--mode=echo`, `--mode=gemini-api`) and pluggable transports (`--serial`, `--tcp`, `--websocket`).

---

## P0 — From the architecture audit

Fidelity holes in the WASM harness. Each violates the "firmware in a harness, no
bypasses" rule in `AGENT.md`. Detail in `ARCHITECTURE.md` §4.4.

- [ ] **Delete the JavaScript protocol parser.** `window.onProtocolRxJson` in
      `ui/audio_pipeline.js` re-parses the firmware's own stream via a tee in
      `wasm_protocol.cpp`, and is load-bearing because `OnAudioResetDecoder()` is a
      no-op. Implement that seam as a down-call, then remove the tee, the JS parser, and
      the `window._wasmProtocolWs` export.
- [ ] **Make `WasmProtocol::OpenAudioChannel()` actually connect** and return `false` on
      failure, instead of setting a flag and returning `true`. This resurrects
      `kDeviceStateConnecting` and the `ContinueOpenAudioChannel` path, which are
      currently unreachable in the browser. ASYNCIFY is already enabled with no
      allowlist, so awaiting the socket is straightforward.
- [ ] **Stop faking the boot sequence.** `wasm_application.cpp` runs
      `Starting → Activating → Idle` as three synchronous calls with nothing between.
- [ ] **Move turn shaping into `ApplicationCore`.** Pre-roll/post-roll and the
      `listen/detect_end` message live in `WasmApplication`, so they exist only in the
      browser — and `application.cc` explicitly instructs that such logic belongs in the
      shared core.
- [ ] **Vendor the CDN scripts.** `shell.html` pulls petite-vue, onnxruntime and vad-web
      from unpkg/jsdelivr, so the emulator does not boot offline. Mechanical fix.
- [ ] **Stop fabricating vision responses.** `ui/camera_bridge.js` returns a
      successful-looking explanation from inside its network-error `catch`, so an outage
      is indistinguishable from success. Duplicated in `wasm_camera.cpp` and `serve.py`.

---

## P0 — USB hardening (host side)

The device-side equivalents of these are fixed; the Python mirror is not.

- [ ] **`wasm/gateway/transport.py` SLIP bugs.** The 64 KB cap is only checked on the
      unescaped path, so a stream of `DB DC` pairs grows the buffer without bound; and a
      malformed escape clears the buffer but keeps accumulating, delivering a corrupted
      frame's tail as a whole frame whose first byte is misread as the type.
- [ ] **Add CRC-16/CCITT to the frame** and reuse `BinaryProtocol3` as the audio payload
      header. Rationale and alternatives considered in `ARCHITECTURE.md` §5.3.
- [ ] **Round-trip test for the SLIP framer.** A property test over
      `encode`/`feed` would have caught both bugs above in minutes. The repo currently
      has exactly one test (`firmware/tests/motion_math_test.cpp`).
- [ ] **Commit a socat recipe.** `tcp_transport.py` and `gateway.py` both reference socat
      in their docstrings, but no script, Makefile target or README snippet actually
      constructs the bridge.

---

## P1 — UX / behaviour

- [ ] **Restore the top panel and home gesture in the AI Agent runtime.** Not an overlay
      bug: `AppAiAgent` tears down Mooncake entirely (`ARCHITECTURE.md` §6), destroying
      the layer that owns and polls those panels. Needs an owner inside the Xiaozhi
      runtime — and a route back to the launcher, which does not currently exist since
      `startXiaozhi()` never returns.
- [ ] **Drop the hardcoded Chinese cloud endpoint.** `sdkconfig` still carries
      `CONFIG_STACKCHAN_SERVER_URL="http://47.113.125.164:12800"`, which contradicts the
      stated goal of cloud independence.

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
