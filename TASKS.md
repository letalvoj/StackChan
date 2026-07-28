# wasm-chan: Future Tasks & Deferred Features

Captured during architectural design sessions. Items are roughly priority-ordered within each category.

---

## P0 — Active / In Progress

- [x] **USB transport** — CDC-**NCM**: the device is a USB network adapter and *listens*
      on 192.168.7.1 for the host to connect (`WebsocketServerProtocol`), so it never
      needs to know the host's address and there is no bespoke wire format.
      `CONFIG_CONNECTION_TYPE_USB_NCM`, on by default. The older SLIP path remains
      selectable as `CONFIG_CONNECTION_TYPE_USB_SLIP`. See `ARCHITECTURE.md` §5.
      **Not yet exercised against real hardware.**
- [ ] **Bring up NCM on the physical device.** Follow `TESTING.md`: flash over UART (the
      USB console is gone under NCM), watch for the expected log sequence, then run
      `wasm/qa_selftest.py --connect 192.168.7.1` and check the bar is green. Confirm the host enumerates
      `usb0`, takes a DHCP lease of 192.168.7.2, and that replug is handled. Expect
      `tts` to produce an audible 440 Hz tone; the Opus gap is closed.
- [x] **Gateway must decode Opus** — The device advertises and sends Opus
      (`audio_service.cc` opens the Opus encoder unconditionally), but
      `wasm/gateway/backends/echo.py` and `gemini_api.py` unpack raw PCM16, and
      `ParseServerHello` never negotiates `format`. Audio is noise in both directions
      until this is closed. Python-side only; do **not** "fix" it by making the device
      advertise PCM, which would fork it from real firmware behaviour. Under NCM the
      relevant server is `serve.py` (WebSocket), not the serial gateway.
      DONE: `wasm/audio_codec.py` picks a codec from the client's hello and transcodes at
      the socket edge, so backends only ever see PCM16. Covered by
      `wasm/tests/test_audio_codec.py` and `test_serve_negotiation.py` (20 tests).
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

## P2 — SLIP hardening (only if CONNECTION_TYPE_USB_SLIP is used)

Deprioritised by the NCM switch: under NCM none of this code is on the path. The
device-side equivalents are already fixed; the Python mirror is not.

- [ ] **`wasm/gateway/transport.py` SLIP bugs.** The 64 KB cap is only checked on the
      unescaped path, so a stream of `DB DC` pairs grows the buffer without bound; and a
      malformed escape clears the buffer but keeps accumulating, delivering a corrupted
      frame's tail as a whole frame whose first byte is misread as the type.
- [ ] **Add CRC-16/CCITT to the frame** and reuse `BinaryProtocol3` as the audio payload
      header. Rationale and alternatives considered in `ARCHITECTURE.md` §5.5.
- [ ] **Round-trip test for the SLIP framer.** A property test over `encode`/`feed` would
      have caught both bugs above in minutes — model it on `wasm/tests/test_audio_codec.py`.
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
- [ ] **3 remaining ESP32 warnings.** `_IO`/`_IOR`/`_IOW` redefinition in
      `xiaozhi-esp32/main/display/lvgl_display/lvgl_display.cc`, which includes lwIP (via
      `application.h`) and then esp_video's V4L2 headers. The `ioctl_compat.h` fix already
      used in `stackchan.cc` applies directly, but the file is upstream-unmodified and
      touching it adds drift. Only visible on a full rebuild -- an incremental build skips
      that TU and reports zero.
- [ ] **Drop the hardcoded Chinese cloud endpoint.** `sdkconfig` still carries
      `CONFIG_STACKCHAN_SERVER_URL="http://47.113.125.164:12800"`, which contradicts the
      stated goal of cloud independence.

---

## P1 — Near-Term Enhancements

- [ ] **Web Serial API browser simulation** — Create a WASM-side shim that connects to a local physical ESP32 via the browser's [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API). This would let the WASM dashboard act as a live debug console for a real device, bridging browser UI controls directly to physical hardware without `socat` or SSH tunnels. *Complexity:* Moderate — requires Chrome-only API, async read/write streams, and TLV frame parsing in JavaScript.
- [ ] **USB Composite Device: Mass Storage** — Expose ESP32 SPIFFS/LittleFS partition as a USB mass storage device alongside CDC protocol and console interfaces. Enables mounting the device's filesystem on the host machine for direct file inspection, config editing, and state tree visualization as a Unix file tree. *Dependency:* TinyUSB composite descriptor with CDC + MSC classes.
- [x] **`make all` top-level build target** — Root `Makefile` builds both ESP32 and WASM, with `IDF_PATH` resolved rather than hardcoded.

---

## P2 — Future Architecture

- [ ] **gateway.py `--mode=gemini-api`** — Implement Gemini API backend for `gateway.py`, replacing echo mode with real LLM conversational turns. Audio → Gemini speech-to-text → Gemini chat completion → Gemini text-to-speech → audio response.
- [ ] **gateway.py `--mode=a2a`** — Agent-to-Agent (A2A) protocol backend for `gateway.py`, enabling StackChan to participate in multi-agent conversations as a physical embodied agent.
- [ ] **Multi-device gateway** — Support multiple ESP32 devices connected simultaneously via separate serial ports, each with independent session state and backend routing.
