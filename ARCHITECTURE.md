# wasm-chan Architecture

Companion to `AGENT.md` (which states the rules). This document describes what is
actually built, as of the USB CDC-NCM work. Where reality diverges from intent, that is
called out rather than smoothed over.

---

## 1. What this project is

Three goals, in priority order:

1. **A local sandbox** for developing StackChan without an ESP32 on the desk, and
   without breaking the ESP32 build.
2. **No cloud dependence.** A local USB link as a first-class transport. No remote
   server required to hold a conversation, and no internet at all.
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
wasm    = v2.2.4 + ApplicationCore extraction + USB transport selection
```

Those commits exist in exactly one place: this working copy. There is no remote that can
restore them.

`fetch_repos.py` used to run `git checkout v2.2.4` unconditionally, which would silently
revert the tree to vanilla upstream — the branch ref survived, but the build quietly lost
ApplicationCore and the USB transport with no error. It now honours a `local_branch` key in
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
| USB-OTG (TinyUSB) | GPIO19/20 | **Used.** CDC-NCM device -> host sees a network adapter. |

They share the same pin pair, so only one can be routed at a time. USB-Serial-JTAG had
to go: it is wired to the ESP_LOG console, and `console_write()` mirrors every log line
onto it. Logging continues on **UART0** -- note this means `idf.py monitor` needs the
UART, not the USB cable.

The S3 has a single OTG peripheral, so device mode is mutually exclusive with USB *host*
mode. The `iot_usbh_*` components in the tree are host-side stacks for the cellular modem
on other boards; this board does not use them, which is what frees OTG.

### 5.2 There is no wire format, and that is the point

Transport is a three-way Kconfig choice:

| `CONNECTION_TYPE_` | What it does |
|---|---|
| `DEFAULT` | MQTT or WebSocket from the OTA config. Stock upstream behaviour. |
| `USB_NCM` | **Default.** USB network adapter; stock `WebsocketProtocol` runs over it. |
| `USB_SLIP` | Legacy serial framing. Kept building, but superseded. |

Under NCM the device is simply a host on a tiny network, so the protocol stack above the
link is *unchanged from WiFi*:

```
device ──WebSocket over TCP over USB-ethernet──> serve.py ──> backend
         ↑ byte-identical to the WiFi path and to the WASM harness
```

TCP supplies length framing, ordering, retransmission and integrity; WebSocket supplies
message boundaries. None of that is ours, so none of it is ours to get wrong. This is
also what makes the no-divergence rule tractable: **one** protocol implementation now
covers WiFi, USB and the browser.

### 5.3 Addressing and discovery

Fixed and self-contained. The device runs a **DHCP server** on the USB link and takes
`192.168.7.1`; the host lands on `192.168.7.2`. Deliberately not `192.168.0/1.x`, so
plugging the device in cannot shadow the host's real network. No router, no internet, no
provisioning step. `StartNetwork()` overwrites the stored WebSocket URL from
`CONFIG_USB_NET_WEBSOCKET_URL`, so a stale URL from a previous WiFi setup cannot send the
device nowhere.

**Discovery is the device dialling out, not the host scanning.** The sequence on plug-in:

1. USB enumerates; the host kernel creates a network interface (`usb0`, `cdc_ncm` driver).
2. The device's DHCP server leases the host its address.
3. TinyUSB's init callback fires -> `NetworkEvent::Connected` -> the application opens
   the audio channel.
4. The device connects to `ws://192.168.7.2:8081/ws` and sends its `hello`, which carries
   the device id.

So a monitor that wants to know "StackChan is back" does not poll or scan -- it is a
WebSocket server, and step 4 *is* the event. Unplug drops the TCP connection; replug
repeats the sequence. The device MAC is derived from the factory MAC so the host sees a
stable adapter across replugs rather than a new one each time.

### 5.4 Status

Builds clean under both USB variants, verified by link map: under NCM the SLIP protocol is
entirely absent, under SLIP it is present. **Not yet exercised against real hardware.**

The Opus/PCM mismatch is unchanged by this work and still blocks audio: the device sends
Opus, `wasm/gateway/backends/*` unpack raw PCM16, and nothing negotiates `format`. Note
that under NCM the relevant server is `serve.py` (WebSocket), not the serial gateway.

### 5.5 If you ever need byte-level framing again

`USB_SLIP` remains selectable. Its format is RFC 1055 SLIP -- `0xC0` delimiter, `0xDB`
escape -- with a one-byte type prefix (`0x00` JSON, `0x01` audio) and no length or
checksum. Implementations must change together:
`xiaozhi-esp32/main/protocols/usb_protocol.cc` and `wasm/gateway/transport.py`.

Worth recording why the WebSocket *layout* was not simply copied onto a serial link:
`BinaryProtocol2`/`BinaryProtocol3` in `protocols/protocol.h` are packed payload
*headers*, not a framing format. They rely on WebSocket for message boundaries and
length, which a raw byte stream does not provide -- so adopting them would not have
removed the need for framing underneath. Making the link a network gets the mature stack
in full instead of imitating part of it.

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
