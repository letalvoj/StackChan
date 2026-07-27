# wasm-chan Architecture

Companion to `AGENT.md` (which states the rules). This document describes what is
actually built, as of the USB CDC-ACM work. Where reality diverges from intent, that is
called out rather than smoothed over.

---

## 1. What this project is

Three goals, in priority order:

1. **A local sandbox** for developing StackChan without an ESP32 on the desk, and
   without breaking the ESP32 build.
2. **No cloud dependence.** Local USB `/dev/ttyACM*` as a first-class transport
   alongside WebSocket. No remote server required to hold a conversation.
3. **Mock hardware strictly at the HAL layer.** The WASM build is the *real firmware*
   in a browser harness, not a reimplementation of it.

The load-bearing consequence of (3): if a behaviour differs between the browser and the
device, that is a bug, not a design choice — with exactly one sanctioned exception, the
audio processor (§4.3).

---

## 2. Repository topology

```
stackchan/
├── AGENT.md                     ← rules (NOT under version control)
├── esp-idf/                     ← ESP-IDF v5.5.5 checkout
└── wasm-chan/                   ← the git repo (branch: wasm)
    ├── ARCHITECTURE.md          ← this file
    ├── TASKS.md
    ├── Makefile                 ← `make all` builds BOTH targets
    ├── firmware/                ← ESP32 target
    │   ├── main/                ← StackChan app code (ours)
    │   ├── components/          ← fetched, git-ignored
    │   └── xiaozhi-esp32/       ← nested repo, git-ignored (see below)
    └── wasm/                    ← browser harness
```

### 2.1 The xiaozhi-esp32 checkout — read this before touching `fetch_repos.py`

`firmware/xiaozhi-esp32/` is a **separate git repository**, **git-ignored** by the
parent, sitting on a **local-only branch `wasm`** that is deliberately **never pushed**:

```
origin  = https://github.com/78/xiaozhi-esp32.git   (upstream; not writable by us)
wasm    = v2.2.4 + ApplicationCore extraction + UsbProtocol + CDC-ACM transport
```

Those commits exist in exactly one place: this working copy. There is no remote that can
restore them.

`fetch_repos.py` used to run `git checkout v2.2.4` unconditionally, which would silently
revert the tree to vanilla upstream — the branch ref survived, but the build quietly lost
ApplicationCore and UsbProtocol with no error. It now honours a `local_branch` key in
`repos.json` and refuses to touch a checkout that has one.

There is **no patch file any more**. `patches/xiaozhi-esp32.patch` was removed once every
line of it was confirmed present in the branch commits. Do not reintroduce patching.

---

## 3. Build targets

`make all` builds **both**. This is not a convenience — the two targets share C++ sources,
so a change that satisfies one toolchain routinely breaks the other, and building only one
hides it until much later.

| Target | Command | Toolchain |
|---|---|---|
| ESP32-S3 | `make esp32` | ESP-IDF v5.5.5, `idf.py` |
| Browser | `make wasm` | Emscripten, `emcmake` |

Both are expected to build with **zero warnings**. Vendored third-party trees are
classified as `SYSTEM` includes in the WASM build so their diagnostics are suppressed
*without* desensitising our own code; `xiaozhi-esp32/main` and `.../protocols` are
deliberately kept non-SYSTEM because our own headers live there.

`IDF_PATH` is resolved from the environment, then `../esp-idf`, then `~/esp/esp-idf`.

---

## 4. The WASM harness

### 4.1 Layering

```
  browser DOM / canvas / WebAudio / getUserMedia
        │  (dumb transducer: pixels out, events in)
  wasm/ui/*.js
        │  Module.ccall / EM_ASM
  wasm/hal/*            ← the HAL seam: WasmBoard, WasmDisplay, WasmCamera,
        │                 WasmSettings, WasmProtocol, WasmApplication
  ─────────────────────────────────────────────────────────────────
  firmware/main/**      ← REAL firmware, compiled unmodified
  xiaozhi-esp32/main/   ← ApplicationCore, DeviceStateMachine, Protocol base
```

Everything below the line is the same code the ESP32 runs.

### 4.2 What is real vs replaced

Compiled from the real firmware tree: `application_core.cc`, `device_state_machine.cc`,
`protocol.cc`, `display.cc`, all of `firmware/main/stackchan/**` (avatar, motion,
`avatar_controller.cc`), all of `firmware/main/apps/**`, and the assets layer.

Replaced at the HAL seam: `application.cc` → `wasm_application.cpp` (the RTOS-only half,
which upstream explicitly marks as such), `settings.cc`, `system_info.cc`, the display
back-ends, `firmware/main/hal/**` → `hal_wasm.cpp`, and the protocol transport.

### 4.3 The one sanctioned exception

The audio processor. The real one uses ESP-DSP/AFE, which cannot compile to WASM, so it
is drop-in replaced and hooked from the browser (Silero VAD via ONNX). This is the *only*
place where a behavioural component legitimately differs.

### 4.4 Known divergences (these are bugs, not design)

Recorded here so they are not mistaken for intent. See TASKS.md.

- `WasmProtocol::OpenAudioChannel()` sets a flag and returns `true` without connecting or
  waiting for a server hello. `IsAudioChannelOpened()` is therefore true from boot, which
  makes `kDeviceStateConnecting` and the whole `ContinueOpenAudioChannel` deferral path
  unreachable in the browser.
- The boot sequence is three synchronous `SetDeviceState` calls
  (`Starting → Activating → Idle`) with nothing between them, where the device gates those
  transitions on network and activation completing.
- A **second protocol parser lives in JavaScript** (`ui/audio_pipeline.js`,
  `window.onProtocolRxJson`), fed by a tee in `wasm_protocol.cpp`. It is load-bearing,
  not observational, because `OnAudioResetDecoder()` is a no-op — deleting the JS parser
  would break playback. This is the single largest fidelity hole.
- Turn shaping (pre-roll/post-roll, emitting `listen/detect_end`) lives in
  `WasmApplication` rather than `ApplicationCore`, so it exists only in the browser.
- `shell.html` loads petite-vue, onnxruntime and VAD from CDNs, so the emulator does not
  boot offline — contradicting goal (2).

---

## 5. The USB transport

### 5.1 Physical layer

The ESP32-S3 has **two** USB-capable peripherals, and the distinction matters:

| Peripheral | Pins | Role here |
|---|---|---|
| USB-Serial-JTAG | GPIO19/20 | **Disabled.** Was the secondary log console. |
| USB-OTG (TinyUSB) | GPIO19/20 | **Used.** CDC-ACM device → host sees `/dev/ttyACM*`. |

They share the same pin pair, so only one can be routed at a time. USB-Serial-JTAG had to
go: it is wired to the ESP_LOG console, and `console_write()` mirrors every log line onto
it — the transport would have corrupted its own stream with its own logging. Logging
continues on **UART0**.

Note also that the S3 has a single OTG peripheral, so device mode is mutually exclusive
with USB *host* mode. The `iot_usbh_*` components in the tree are host-side stacks for the
cellular modem on other boards; this board does not use them, which is what frees OTG.

Selected by `CONFIG_CONNECTION_TYPE_USB`, **enabled by default** in
`sdkconfig.defaults`. When on, `Application::InitializeProtocol()` picks `UsbProtocol`
*before* consulting the OTA config — the point of this transport is to work with no
network and no server configured.

### 5.2 Wire format — what is actually implemented

**RFC 1055 SLIP framing, with a one-byte type prefix. No length field, no checksum, no
sequence numbers.**

```
  0xC0  escape(type_byte)  escape(payload ...)  0xC0
  └─────┴──────────────────┴────────────────────┴────  leading + trailing delimiter

  type_byte:  0x00 = JSON control message (UTF-8)
              0x01 = audio frame (Opus)

  escapes:    0xC0  →  0xDB 0xDC     (END  → ESC ESC_END)
              0xDB  →  0xDB 0xDD     (ESC  → ESC ESC_ESC)
```

Implementations: `xiaozhi-esp32/main/protocols/usb_protocol.cc` (device) and
`wasm/gateway/transport.py` (host). Both must change together.

Properties: self-delimiting, resynchronises after garbage (a leading `0xC0` on every frame
means a corrupt frame costs you at most one message), and trivially decodable in Python.
Worst case 2× overhead if every byte needs escaping — irrelevant for Opus at 16 kHz.

Weaknesses: **no integrity check and no length.** A flipped bit inside a frame is
undetectable; a corrupted Opus payload goes straight into the decoder.

### 5.3 On reusing the WebSocket wire layout

Worth stating clearly, because it is a natural assumption: **`BinaryProtocol2` /
`BinaryProtocol3` in `protocols/protocol.h` are not a framing format.** They are packed
payload *headers* (version, type, timestamp, `payload_size`) that ride inside a WebSocket
message. WebSocket itself supplies the message boundaries and the length — that is what
makes them mature and reusable.

A raw USB CDC byte stream has no message boundaries at all. So adopting the WebSocket
layout does **not** remove the need for a framing layer underneath; the two are
complementary, not alternatives. Any design here is "framing + header", and only the
header part can be borrowed.

The recommended direction (see TASKS.md) is therefore to keep SLIP and strengthen it,
rather than invent a new format:

- **Keep SLIP** for framing. It is an RFC, already implemented on both sides, needs no
  dependencies, and is readable in a hexdump.
- **Add CRC-16/CCITT** as a frame trailer. Cheap, and closes the integrity gap.
- **Reuse `BinaryProtocol3` as the audio payload header** so device and gateway share one
  struct definition and the jitter buffer gets its timestamp.

Alternatives considered: **COBS** has strictly better worst-case overhead (+1 byte per
254) and a clean `cobs` pip package, and would be a reasonable swap if framing is ever
revisited — but it is not enough of a win to justify changing both ends now.
**HDLC/PPP** brings CRC for free but is heavier than this link needs.

### 5.4 Status

Enumeration, handshake and framing are implemented and build clean. **Audio does not work
end to end yet:** the device advertises and sends Opus, while `wasm/gateway/backends/*`
unpack raw PCM16, and `ParseServerHello` never negotiates `format`. Until the gateway
learns to decode Opus, audio is noise in both directions. This is tracked, not hidden.

---

## 6. App model and the AI Agent trapdoor

Apps are Mooncake `AppAbility` objects installed in `main.cpp`. The status bar and home
indicator are **not** global chrome: each app creates them in `onOpen()`, polls them from
`onRunning()`, and destroys them in `onClose()`. The swipe gestures are a polled state
machine (`apps/common/home_indicator/`), not LVGL `LV_EVENT_GESTURE`.

**`AppAiAgent` is not really an app.** Its `onOpen()` calls `requestXiaozhiStart()`, and
the main loop then breaks out and runs:

```cpp
GetMooncake().uninstallAllApps();
DestroyMooncake();
GetHAL().startXiaozhi();   // never returns
```

It is a one-way trapdoor out of the app framework. This is why the top and bottom swipe
gestures are dead there and everywhere else they work: the layer that owns and polls those
panels has been destroyed. Adding `create_status_bar()` to `AppAiAgent` would not fix it —
the objects would be torn down moments later and nothing would poll them. The panels need
an owner inside the Xiaozhi runtime, and there is currently no route back to the launcher
at all. The WASM harness reproduces this faithfully (`hal_wasm.cpp`), so it is genuine
firmware behaviour, not a harness artifact.
