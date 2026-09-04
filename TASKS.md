# wasm-chan: Future Tasks & Deferred Features

Captured during architectural design sessions. Items are roughly priority-ordered within each category.

---

## P0 — Active / In Progress

- [x] **`tts stop` no longer arms the mic.** It was hardcoded to `speaking → listening`
      (`application_core.cc`), so *any* peer could put the device into `listening` — mic
      live, `OnAudioVoiceProcessing(true)` — just by sending a `tts start`/`stop` pair.
      Using the robot as a speaker left it stuck there indefinitely, and it walked around
      the rule that the tap is the privacy boundary. `ApplicationCore` now records the
      state `tts start` interrupted and restores it: a real turn returns to `listening`,
      an announcement from `idle` returns to `idle`. No protocol change; `gemini_live.py`
      is unaffected. **Built for both targets, not yet verified on hardware.**

- [ ] **`listening` still has no automatic exit.** The fix above removes the *cause* for
      announcements, not the general case: a real agent that opens a turn and then dies
      leaves the device listening forever. Only the human's tap and the model's
      `self.robot.end_conversation` get out; upstream's `IsTimeout()` is overridden to
      false and must stay that way (it measures socket silence, meaningless for a device
      in the server role). Candidate: a watchdog on **room silence** rather than socket
      silence — the device already computes `audio_service_.IsVoiceDetected()` and
      forwards it as `{"type":"vad"}` (`application.cc:284`), so N seconds in `listening`
      with no speech → `listen stop` + idle, any speech resets the clock. N as Kconfig,
      45–60 s. Cannot fire mid-conversation; needs no new sensing.

- [x] **WiFi works, alongside USB-NCM.** `CONFIG_STACKCHAN_WIFI_ENABLE` brings up a
      WiFi STA next to the USB link; the server binds `INADDR_ANY`, so `/ws`, `/debug`
      and `/debug/reset` answer on the LAN address with no protocol changes. Verified
      on hardware: Gemini Live ran a full voice session over WiFi — head movement,
      face changes, petting, zero send failures. `/debug` reports the address as
      `wifi_ip`. WiFi power save is off (`WIFI_PS_NONE`): it is a debug link, and the
      default `MIN_MODEM` quantised ping to the AP beacon, 100 ms vs 4–10 ms.

- [x] **lwIP `tiT` stack overflow.** `CONFIG_LWIP_TCPIP_TASK_STACK_SIZE` was IDF's
      stock 3072 and had never been revisited; `tiT` peaks at 2,996 B, so it ran on
      **76 bytes** of margin and overflowed once WiFi joined USB-NCM on the same task.
      Presented as an intermittent crash seconds after boot, stable afterwards. Now
      6144, and `/debug` exposes `tcpip_stack_free_min` so this is a number, not a
      debate. Independent of the VPN — it would have bitten on plain WiFi.

- [x] **Internal RAM pruning.** Freed ~11 KB of static DIRAM (USB-NCM NTB buffers
      `3×3200` → `2×2048` both ways). See `ARCHITECTURE.md` §7 for the full budget,
      the rule for PSRAM task stacks, and two levers that were measured and found
      worthless. Do not re-run those experiments.

- [x] **Removed `GET /debug/logs` and `POST /debug/download-mode`.** Both only worked
      when the device was healthy, which is not when either was needed. The log ring
      was reachable only while the HTTP server was up, and the failures worth debugging
      are boot loops and early panics — where the USB-Serial-JTAG console *is* alive,
      because TinyUSB never gets far enough to take the pins. download-mode never
      worked at all and was actively harmful: its `RTC_CNTL_FORCE_DOWNLOAD_BOOT` latch
      is sticky across resets and trapped the device until a power cycle. ~16 KB of
      PSRAM and both traps gone.

- [ ] **Getting a human out of the flash loop is still unsolved, and may be
      unsolvable.** esptool can write flash unattended from `0x1001`, but cannot start
      the app: `--after hard_reset` and `esptool run` both report success and leave the
      chip parked silently in ROM. So every flash costs one physical button press.
      `/debug/download-mode` was the attempt to avoid it and has been removed (see
      above). To park a crash-looping device quietly with no button — this does work:
      ```
      python -m esptool --chip esp32s3 -p "$(ls /dev/cu.usbmodem* | head -1)" \
          --before default_reset --after no_reset --no-stub chip_id
      ```

- [x] **Device must work with no cable and no WiFi.** UI was gated on
      `is_xiaozhi_ready()` (network-derived), so an offline device had no menu, no home
      button, no touch. Now gated on the avatar existing, with SNTP kept on the network
      gate — collapsing the two panicked lwIP. Both fixed; **offline case still untested
      on hardware.**

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

## P2 — Tailscale / MicroLink (parked, deliberately OFF)

**Decision (2026-07-31): not enabling the VPN now.** WiFi already delivers the thing
the VPN was wanted for — reaching the device without a cable — and it does it with no
extra memory pressure, no vendored networking stack, and a full verified voice session
behind it. The VPN extends that reach beyond the LAN, which is worth having later and
is not worth destabilising a working device for today.

`CONFIG_STACKCHAN_TAILSCALE_ENABLE` is `default n` and `depends on
STACKCHAN_WIFI_ENABLE`. The code is in tree and builds; the two were split into
separate Kconfig symbols precisely so WiFi could ship without it.

- [ ] **`ml_wg_mgr` NULL deref.** From a core dump, not inference:
      ```
      Crashed task: 'ml_wg_mgr'
      exccause 0x1c (LoadProhibitedCause)   excvaddr 0xb8
      epc1 0x4037aecf -> esp_psram_check_ptr_addr (esp_psram.c:593)
      ```
      `excvaddr 0xb8` is a small offset from zero — a NULL dereference reached via the
      heap alloc/free path. Reproduced every boot right after
      `ml_wg_mgr: CMM endpoint: LAN ...`, once real peers arrived from a MapResponse,
      which is why QEMU never hit it (the peer table stays empty there).
      Evidence: `/tmp/stackchan_core.elf` + `/tmp/stackchan_crash.elf`.
      **Next step:** `bt` in gdb for the caller of `esp_psram_check_ptr_addr`.

      **Caveat worth checking first:** that crash predates the ~11 KB of internal RAM
      freed since, and `ml_wg_mgr`'s 8 KB stack request had been failing against a
      7,680 B largest block. Some of what looked like corruption may simply have been
      allocation failure downstream. Re-run before debugging the old dump.

- [ ] **DERP TLS handshake times out** (`conn=0`, `SSL - The operation timed out`), so
      no traffic flows even when registration succeeds. Never exercised under QEMU.

- [ ] **Expect to raise `CONFIG_LWIP_TCPIP_TASK_STACK_SIZE` to 8192.** WireGuard runs
      its Noise handshake in lwIP's context; X25519 alone is typically 1–2 KB of stack.
      Watch `tcpip_stack_free_min` on first enable. See `ARCHITECTURE.md` §7.4.

- [ ] **Make USB-NCM and the tailnet mutually exclusive, not simultaneous.** They
      answer the same question — how a host reaches this device — and running both is
      what makes the memory budget tight. Dropping NCM when the tailnet is on reclaims
      ~12 KB of internal RAM (8,192 NTB buffers + 4,096 TinyUSB task), roughly double
      `ml_wg_mgr`'s 6 KB, and gives back the serial console for the device's whole life
      instead of losing it at boot. Shape: a Kconfig `choice` next to the existing
      `CONNECTION_TYPE_USB_NCM` / `_SLIP`, not a third orthogonal flag.
      See `ARCHITECTURE.md` §7.5. **Do this before debugging the VPN further** — it may
      dissolve the problem rather than solve it.

- [ ] **Publish the MicroLink fixes upstream** — PSRAM staging buffer for NVS writes,
      task-slot reuse to stop a per-retry leak. Both are genuine ESP-IDF-constraint
      fixes independent of this project.

---

## P2 — Future Architecture

- [ ] **gateway.py `--mode=gemini-api`** — Implement Gemini API backend for `gateway.py`, replacing echo mode with real LLM conversational turns. Audio → Gemini speech-to-text → Gemini chat completion → Gemini text-to-speech → audio response.
- [ ] **gateway.py `--mode=a2a`** — Agent-to-Agent (A2A) protocol backend for `gateway.py`, enabling StackChan to participate in multi-agent conversations as a physical embodied agent.
- [ ] **Multi-device gateway** — Support multiple ESP32 devices connected simultaneously via separate serial ports, each with independent session state and backend routing.
