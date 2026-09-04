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
parent, sitting on branch `wasm` of a **fork**:

```
upstream  = https://github.com/78/xiaozhi-esp32.git       (not writable by us)
fork      = https://github.com/letalvoj/xiaozhi-esp32.git (branch `wasm`)
wasm      = v2.2.4 + ApplicationCore extraction + USB transport selection
```

`repos.json` points at the fork, so **a fresh clone of this repo builds**: `fetch_repos.py`
clones `letalvoj/xiaozhi-esp32` at `wasm` and gets ApplicationCore, the USB transports and
the `tts stop` lifecycle fix. This was not always true — the branch used to be local-only,
and a fresh clone silently got vanilla v2.2.4 and a tree that would not build.

`fetch_repos.py` used to run `git checkout v2.2.4` unconditionally, which would silently
revert the tree to vanilla upstream — the branch ref survived, but the build quietly lost
ApplicationCore and the USB transport with no error. It now honours a `local_branch` key in
`repos.json` and refuses to touch a checkout that has one, which still matters: an existing
checkout can hold commits that have not been pushed to the fork yet.

**Work in the nested repo must still be pushed explicitly** (`git push fork wasm`). Nothing
in `make all` or the parent repo's own push does it for you.

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

Under NCM the device is simply a host on a tiny network. The wire protocol above the
link is *unchanged from WiFi* -- same WebSocket, same JSON, same binary audio frames --
but the device **listens** rather than dialling out, so it never needs to know the host's
address:

```
host ──connects to ws://192.168.7.1:8081/ws──> device
       ↑ same wire protocol as the WiFi path and the WASM harness
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
entirely absent, under SLIP it is present.

**Verified on real hardware.** NCM enumerates (`en9`), DHCP leases `192.168.7.2`, the
device listens on `192.168.7.1:8081`, and both directions carry traffic: host→device audio
plays through the speaker, and device→host delivers the hello, MCP results, and protocol
events. `firmware/examples/jingle.py` is the worked end-to-end demo.

Audio is no longer blocked — `wasm/audio_codec.py` negotiates `format` from the hello and
transcodes Opus↔PCM at the socket edge.

Two behaviours worth knowing before writing a client:

**One client at a time, enforced.** Adopting a new socket closes the previous one. See
`AGENT.md` §6 for the full reasoning; the short version is that device-originated frames
(audio, TTS state) have no requester to route back to, so a second client would silently
steal a live audio stream. Fan-out belongs in a host-side client, not in firmware.

**`tts stop` returns to the state `tts start` interrupted.** Inside a real turn that is
`listening`, so the device emits `{"type":"listen","state":"start","mode":"auto"}` and
reopens the user's turn — a client that never closes it leaves the device parked there,
where the idle servo animation does not run, and closing it is the half of the loop a
conversational client must implement. An utterance that began from `idle` returns to
`idle`.

That distinction is the rule that **only the device arms its own microphone.** Upstream
hardcodes `speaking → listening`, which is right when the device is a *client* dialling a
conversational server and every utterance is a reply inside a turn a human opened. Here the
device is the *server* and the peer may be a script using it as a speaker, so the same edge
let any peer turn the microphone on by sending audio. `ApplicationCore` now records the
pre-speaking state and restores it (`application_core.cc`, `tts` handler); the entry to
`listening` stays where it belongs, on a tap or a wake word.

Note also that mic audio only flows once the app calls `OpenAudioChannel()` (wake word or
button). A client that wants to *hear* the device must account for that; playback into the
device works without it.

### 5.5 The agent link, in one page

`wasm/clients/gemini_live.py` is the reference client. Beyond audio it carries:

- **`"video":true` on the listen message** — set when the session started from the camera
  button rather than a face tap. Additive, so older clients stay audio-only.
- **`{"type":"vad","state":...}`** — the device's own voice detection, forwarded so a
  client can gate expensive work. Camera streaming uses it: a frame per second *only*
  while someone is speaking, because every frame stays in the model's context for the
  rest of the session.
- **`{"type":"sensor","event":...}`** — `head_pet`, `shaken`. Rate-limited on the device
  and dropped outside a session. The client folds these in with `turn_complete=False`, so
  they colour what the robot says next rather than interrupting to announce them.

Two lanes exist and they are **not** interchangeable, which cost three attempts to learn:

| | |
|---|---|
| `send_realtime_input` | Ordered against the **audio clock**. Right for the mic and for the camera *stream* |
| `send_client_content` | Ordered against **conversation turns**. Required for a one-shot photo, which must be in context before the turn it belongs to completes |

Sending a one-shot photo on the realtime lane makes the model answer one turn behind;
sending it with `turn_complete=False` and no following turn makes it wait forever. The
working shape is: answer the tool call, *then* send the image as its own complete turn.

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

### 5.6 The tailnet as a second road to the same server

**Status: registers, does not yet carry traffic.** Off by default
(`CONFIG_STACKCHAN_TAILSCALE_ENABLE`), and currently boot-loops when enabled — see
TASKS.md for the live crash.

MicroLink (an ESP32 Tailscale client, vendored at `components/microlink_vendor` and
pinned in `repos.json`) brings up WiFi STA *alongside* USB-NCM rather than instead of
it. The design point worth keeping: **this required no protocol work at all.**
`WebsocketServerProtocol` already binds `INADDR_ANY`, so `/ws`, `/debug` and
`/debug/reset` become reachable at the device's `100.x` tailnet address the moment the
link is up. One server, two roads to it.

That also settles the authentication question the earlier design discussion circled:
there is nothing to add. Tailscale's own WireGuard tunnel is the authentication and the
encryption, so the device needs no bearer tokens, no per-request checks and no second
credential system — which is why USB stays entirely open and unauthenticated too.

Two constraints this environment imposes that an emulator does not, both learned the
hard way (details in DEBUGGING.md):

* **Flash and PSRAM share the SPI bus and cache.** Any flash write disables the cache
  PSRAM is reached through, so a task executing from a PSRAM stack — or a buffer handed
  to a flash write API — must not be live in that window. MicroLink's four tasks were
  moved to PSRAM stacks to survive internal-RAM fragmentation; `ml_wg_mgr` is the
  exception and must stay internal, because it is the only one that writes NVS.
* **Internal RAM is the scarce resource,** not total heap. With LVGL, audio and USB
  running, the largest free *contiguous* internal block measured a stable ~7680 B —
  enough to fail an 8 KB task stack while several megabytes sat free overall.

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

## 7. The internal RAM budget

**Internal RAM is the binding constraint on this device.** Not flash, not total heap.
Every crash in the 2026-07-31 session traced back to this one line item, wearing four
different disguises. Read this before adding a task, a buffer, or a network interface.

### 7.1 Three budgets, only one of which is tight

| Budget | Size | Free | Holds |
|---|---:|---:|---|
| Flash | 16 MB | ~1.4 MB | code, `.rodata`, assets, fonts, menus |
| PSRAM | 8 MB | ~8 MB | anything not listed below |
| **DIRAM** | **341,760 B** | **~110 KB after static** | **task stacks, DMA buffers, cache-off code** |

Only the third one is scarce, and nothing else can substitute for it. This matters
because the intuitive economies — pruning menus, dropping unused apps, trimming
assets — all free **flash**, which is not what runs out. They buy nothing.

Measured after the 2026-07-31 pruning: 230,931 B (67.6%) consumed by the static image
before `app_main` runs, leaving ~110 KB for every task stack and driver buffer on the
device combined. Steady-state free internal RAM sat around 18 KB, with a largest free
*contiguous* block of ~7,680 B — which is how an 8 KB task stack request fails while
"several megabytes free" is simultaneously true. **Track largest-contiguous-block, not
free-bytes;** only the first one predicts allocation failures.

### 7.2 PSRAM task stacks, and the rule that is not "does it write flash"

Moving a task stack to PSRAM is the single biggest lever available, and MicroLink
depends on it existing: `ml_net_io` (8 KB), `ml_derp_tx` (14 KB) and `ml_coord` (12 KB)
are all on PSRAM stacks. That is **34 KB** that would otherwise have to come out of an
18 KB pool. The VPN is not merely helped by this technique — it is impossible without it.

Two traps, both hit for real:

* `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` gates `xTaskCreateStatic` **only**. There
  is no PSRAM fallback for dynamic `xTaskCreate`, on any IDF version. Use
  `xTaskCreateStaticPinnedToCore` (MicroLink's `ml_task_create_psram`) or
  `xTaskCreatePinnedToCoreWithCaps` (the app-side idiom, see `hal_imu.cpp`).

* **The disqualifying test is "can this task cause a cache freeze?", NOT "does it write
  flash?"** The narrower rule was written into a comment, believed, and boot-looped the
  device: the `stackchan` update task touches no flash whatsoever, but drives an SPI LCD
  with a 2 MB PSRAM image cache, and DMA coherency freezes the cache constantly. It
  died on `assert failed: esp_cache_freeze_caches_disable_interrupts
  (s_task_stack_is_sane_when_cache_frozen())`. ESP-IDF asserts rather than corrupting
  silently, which is the only reason this was cheap to find.

  Qualifying today: `imu`, `headtouch` (I2C sensor reads, no DMA'd framebuffer), and
  MicroLink's three. Disqualified: `stackchan` (LCD DMA), `ml_wg_mgr` (writes NVS).

### 7.3 Concessions already made, and what they cost

| Decision | Δ internal | Concession |
|---|---:|---|
| NCM NTB buffers `3×3200` → `2×2048` both ways | **−11,024** | lower USB throughput ceiling; nowhere near saturated by audio + debug |
| `CONFIG_LWIP_TCPIP_TASK_STACK_SIZE` `3072` → `6144` | +3,072 | necessary, not optional — see below |
| `stackchan` task → PSRAM stack | 0 | **reverted**; violated 7.2 |
| Disabling unused `*_IN_IRAM` driver flags | **0** | **measured worthless.** Unlinked drivers never occupied IRAM. Do not retry |

**The lwIP increase was not a precaution.** With one netif, `tiT` peaked at 2,996 bytes
against a 3,072-byte stack — **76 bytes of margin**, which presented as an intermittent
crash a few seconds after boot and stable operation afterwards. Adding WiFi alongside
USB-NCM put two interfaces plus DHCP and DNS on that one task. If an intermittent
post-boot crash ever follows adding a netif, this is the first thing to check;
`/debug` reports `tcpip_stack_free_min` precisely so it is a number and not a debate.

### 7.4 What the VPN will additionally cost

Enabling MicroLink adds, on top of everything above:

* **~34 KB of PSRAM stacks** — free, in the sense that matters here.
* **6 KB of internal RAM** for `ml_wg_mgr`, which cannot move (7.2). This is the
  allocation that failed at 8 KB against a 7,680-byte largest block; it was reduced to
  6 KB, and the ~11 KB freed since should make it comfortable. Unverified.
* **A third netif on `tiT`'s stack.** WireGuard runs its Noise handshake — X25519,
  blake2s HMAC with 64-byte pads, `message_handshake_initiation` at 148 B — in lwIP's
  context. X25519 alone is typically 1–2 KB of stack. 6144 was chosen with this in
  mind rather than sized to two interfaces; watch `tcpip_stack_free_min` when it is
  first enabled, and expect to need 8192.
* **ChaCha20-Poly1305 in mbedTLS**, pulled in by three `select`s in Kconfig.

The honest summary: the device fits WiFi comfortably and fits the VPN only barely. That
is the reason the VPN is parked at P2 rather than pushed through.

### 7.5 USB-NCM and the tailnet should probably be mutually exclusive

The current design runs the tailnet *alongside* USB-NCM, on the reasoning that USB
stays the trusted no-setup path and the VPN is purely additive. That is the right call
while the VPN is experimental. It is probably the wrong call once it is not.

Both exist to answer the same question — *how does a host reach this device?* — and
paying for two answers is what makes the memory budget tight:

| Kept only for USB-NCM | Internal RAM |
|---|---:|
| NCM NTB buffers (already trimmed from 19,200) | 8,192 |
| TinyUSB task stack | 4,096 |
| **Total reclaimable** | **~12 KB** |

That is roughly double `ml_wg_mgr`'s 6 KB — the one allocation that cannot move to
PSRAM and the one that has actually failed. Making the transport a **choice** rather
than a stack would turn the VPN from "fits barely" into "fits comfortably", and it
costs nothing real: a device on a tailnet does not need a USB network adapter, and a
device on a bench cable does not need a tailnet.

It also buys back the serial console. TinyUSB is what takes over GPIO19/20 and kills
USB-Serial-JTAG once the app runs (`firmware/DEBUGGING.md` §3); without NCM, a
WiFi/tailnet device would keep a live console for its whole life instead of going dark
at boot — which is the single most expensive property of the current arrangement.

The natural shape is a `choice` in Kconfig alongside the existing
`CONNECTION_TYPE_USB_NCM` / `CONNECTION_TYPE_USB_SLIP`, not a third orthogonal flag.
