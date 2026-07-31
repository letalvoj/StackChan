# DEBUGGING.md — hardware bring-up playbook

For the next agent (or the next you, post-compaction). Everything here was paid for
in flash cycles. Read §1 and §2 before touching anything.

---

## 1. Orientation in 30 seconds

StackChan (M5Stack CoreS3, ESP32-S3) runs the real firmware. It talks over **USB
CDC-NCM** — it enumerates as a *network adapter*, not a serial port.

| What | Where |
|---|---|
| Device address | `192.168.7.1` (fixed; device **listens**) |
| Host address | `192.168.7.2` (DHCP from the device; iface usually `en9`) |
| Protocol | `ws://192.168.7.1:8081/ws` |
| Status page | `http://192.168.7.1:8081/debug` |
| Recovery | `POST http://192.168.7.1:8081/debug/reset` |
| Device log | `/dev/cu.usbmodem1234561` (CDC console, app mode) |
| Download-mode port | `/dev/cu.usbmodem111101` (USB-Serial-JTAG) |
| ESP-IDF | `/Users/letalvoj/Projects/stackchan/esp-idf` (v5.5.5) |
| Repo | `/Users/letalvoj/Projects/stackchan/wasm-chan` |
| Python | `./wasm/.venv/bin/python` |

**The device is single-client by design.** A second WebSocket connection evicts the
first (deliberately — see `AGENT.md` §6). `/debug` is exempt: poll it freely during a
live session, it never touches `client_fd_`.

---

## 2. The USB PID tells you everything

`system_profiler SPUSBDataType | grep -A3 "Espressif\|USB JTAG"`

| PID | Mode | Meaning |
|---|---|---|
| `0x1001` | USB-Serial-JTAG | **Download mode** (or app not running). Flash from here. |
| `0x4000` | TinyUSB, NCM only | App running, no CDC console — you are flying blind |
| `0x4001` | TinyUSB, NCM + CDC | **Normal.** Networking *and* readable logs |

USB-Serial-JTAG and USB-OTG share GPIO19/20, so only one can own the bus. The PID is
the instant answer to "which mode am I in" — check it before theorising.

---

## 3. The flash loop

**Whether a human is needed depends entirely on which USB identity the device
currently has.** Check first — it decides everything:

| Device state | PID | esptool can flash it? | esptool can BOOT it? |
|---|---|---|---|
| App running normally | `0x4001` | **No.** TinyUSB owns GPIO19/20, USB-Serial-JTAG does not exist | No |
| In bootloader, or crash-looping | `0x1001` | **Yes**, fully autonomous | **No** |

**Flashing and booting are separate questions, and the answer differs.** From
`0x1001` esptool connects, erases and writes with no human at all — but it cannot
start the application. `--after hard_reset`, and the dedicated `esptool run`, both
report success ("Hard resetting via RTS pin...") and leave the chip sitting in the
ROM: PID stays `0x1001`, no NCM interface appears, and the serial port is
*completely silent* rather than printing anything.

So **every flash needs one physical action** — a short press of RST, or a
power cycle. Budget for it and ask up front; do not discover it after the flash.

Two earlier versions of this table were wrong in opposite directions, so trust the
symptoms, not the prose: a blanket "hard reset does nothing" (wrong — flashing from
`0x1001` needs nobody), then "connect, flash *and reset* — fully autonomous" (wrong —
the reset half never worked). Verified twice on 2026-07-31.

**Do not use `POST /debug/download-mode` to avoid the button.** It sets
`RTC_CNTL_FORCE_DOWNLOAD_BOOT`, which is a *sticky RTC latch*: it survives resets,
so the device then lands in `boot:0x23 (DOWNLOAD)` on **every** subsequent reset and
looks exactly like a hung boot. Only a power cycle clears it.

**Do not hand-roll a DTR/RTS reset pulse either.** USB-Serial-JTAG needs esptool's
specific `USBJTAGSerialReset` ordering; a naive "pulse RTS" resets the chip but
leaves GPIO0 asserted, landing in download mode — again indistinguishable from a
hang at a glance.

**Do not hardcode the port name.** It has been observed as both
`/dev/cu.usbmodem1101` and `/dev/cu.usbmodem101` across boots of the same device.
Use `PORT=$(ls /dev/cu.usbmodem* | head -1)`.

```bash
# From 0x4001 (app running) -- a human must enter download mode first:
say "Vojta, hold reset for three seconds until green, then tell me."

# From 0x1001 -- no human needed at any point:
cd firmware && source /Users/letalvoj/Projects/stackchan/esp-idf/export.sh && idf.py build
cd build && python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 -b 460800 \
    --before default_reset --after hard_reset write_flash "@flash_args"
```

**To silence a crash-looping device and hold it for inspection**, park it in the
bootloader — no button, and it stays there until told otherwise:

```bash
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 \
    --before default_reset --after no_reset --no-stub chip_id     # "Staying in bootloader."
```

When a human IS needed, chain `say` onto the command that needs them rather than
calling it separately — they may be away from the keyboard. Equally: do not ask for a
button press you do not need, and do not narrate one you did not require.

Boot is confirmed when `ifconfig | grep 192.168.7.2` returns and `/debug` answers.

---

## 4. Where to look, in order

```bash
# 1. Is it alive and what state is it in?  (safe during a live session)
curl -s http://192.168.7.1:8081/debug | python3 -m json.tool

# 2. Device log — start capture BEFORE reproducing
cat /dev/cu.usbmodem1234561 > /tmp/dev.log &
grep -vE "SystemInfo" /tmp/dev.log | tail -40      # SystemInfo spams every 10s

# 3. Client log
./wasm/.venv/bin/python wasm/clients/gemini_live.py > /tmp/client.log 2>&1 &

# 4. Scrollback the console cannot give you -- survives a panic, needs no cable,
#    works over the tailnet. 16 KB ring, oldest first, installed before HAL init.
curl -s http://192.168.7.1:8081/debug/logs
```

**Sequencing rule, and it is not optional: start capture, VERIFY it is alive, THEN ask
the human to reproduce.** Not "start capture, then ask" -- confirm the process is
actually running first (`ps aux | grep <the cat command>`), because a `&`-backgrounded
`cat` can silently fail to attach (port busy, race with a prior capture dying) and you
will not notice until the reproduction is already over and the log is empty. Saying
"capturing now" before that check is true is worse than saying nothing -- it is a
false claim the human has no way to catch, and they will act on it (reproduce
immediately) before you have actually confirmed anything.

```bash
pkill -f "cat /dev/cu.usbmodem1234561" 2>/dev/null   # kill any stale prior capture
rm -f /tmp/dev.log
cat /dev/cu.usbmodem1234561 > /tmp/dev.log 2>&1 &
disown
sleep 1
ps aux | grep "cat /dev/cu.usbmodem1234561" | grep -v grep   # MUST show a live pid
# only now: say "capture is live, go ahead"
```

**A `cat` on this port can die silently on a device reboot** (USB re-enumerates,
the old file descriptor breaks) without printing an error -- the background process
just vanishes. Re-verify aliveness after every reboot, not just the first time; do
not assume a capture you started three reboots ago is still the one collecting data.

**If you expect more than one reboot in the session (bring-up work, "reset it and
tell me"), don't use plain `cat` at all -- use a self-healing loop that reattaches
the instant the port drops, so you stop racing a human's button press:**

```bash
cat > /tmp/watch_console.sh <<'SCRIPT'
#!/bin/bash
while true; do
    if [ -e /dev/cu.usbmodem1234561 ]; then
        cat /dev/cu.usbmodem1234561 >> /tmp/console_watch.log 2>&1
    fi
    sleep 0.2
done
SCRIPT
chmod +x /tmp/watch_console.sh
rm -f /tmp/console_watch.log
/tmp/watch_console.sh &
disown
sleep 1
ps aux | grep watch_console | grep -v grep   # MUST show it, then and only then say
```

This was worth the detour: chasing a plain `cat` across several manual restarts
during live DERP debugging cost multiple lost reboot windows in a row before this
existed -- every restart raced the exact log lines that mattered, and lost.

**Timestamps in the captured log are milliseconds since THAT boot, not wall-clock,
and can look stale-but-plausible even when something is wrong.** If a capture you
believe is fresh shows large timestamps and is missing the early boot lines (e.g. no
`UsbNetBoard: USB cable attached`), do not trust it -- kill it, verify no process is
attached to the port at all (`ps aux | grep cu.usbmodem`), and start over. Cross-check
suspicious timestamps against `/debug`'s own `uptime_s` (backed by
`esp_timer_get_time()`, authoritative) rather than trusting the console log's own
numbers in isolation.

`/debug` fields worth knowing:

- `device_state` — `idle` is the only state a tap works from (plus `speaking`/`connecting`)
- `xiaozhi_ready` — **if `false`, face taps are silently discarded.** No log line, by design
- `client_fd` / `has_client` — is anyone connected
- `frames_rx` / `frames_tx` — is traffic actually flowing (hello bypasses `frames_tx`)
- `uptime_s` — a small number you did not expect means **it rebooted**

**The CDC console cannot capture its own panic.** A crash kills TinyUSB before the
backtrace flushes, so you get an empty log and a re-enumeration. Symptom of a crash:
`curl` gets `Connection reset`, the CDC port disappears, and `uptime_s` restarts. If you
need a real backtrace, enable the core-dump partition.

---

## 5. Traps that cost real time

Each of these presented as something else entirely.

| Symptom | Actual cause |
|---|---|
| Taps do nothing, no log line at all | `_is_xiaozhi_ready` false. Check `/debug`. It latches on the first `STANDBY` status; the USB boot skips Mooncake and can reach idle before the avatar exists |
| Robot ignores its own button, seems crashed, `/debug` fine | Wedged in `connecting`. Now has a 15s watchdog + tap-cancels; if it returns, look for a path into `connecting` that never unwinds |
| Device receives but never replies | `client_fd_` never captured. The `HTTP_GET` handshake branch **never runs** on this IDF — the socket is adopted on every frame instead |
| Reboots on every request to an endpoint | `%llu` in `snprintf`. `CONFIG_LIBC_NEWLIB_NANO_FORMAT=y` drops `long long`; the vararg list desyncs and later `%s` dereferences garbage. Use `%lu` |
| Flashed fine, but old firmware boots | OTA rollback. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` reverts any image that never calls `MarkCurrentVersionValid()` |
| Choppy audio | Sending faster than realtime. The device decodes at exactly realtime; pace one frame per `frame_ms`. Bandwidth is never the cause — 3 KB/s on a 1500 KB/s link |
| Mic also choppy | CPU-bound work (resample/encode) on the event loop starves the uplink. Use a thread |
| Client connected, device says `has_client:false` | Ghost socket. Something cleared bookkeeping without closing the TCP connection |
| Nothing enumerates, no `Speed:` line in system_profiler | Bad USB cable. Try another before debugging anything else |
| LED colour "wrong" | Firmware owns the LED as state indicator (green listening / blue speaking / off idle). If a model has `set_led_color`, it will fight you |
| Model says it cannot take photos | It is telling the truth about whatever tool set it was given — check the declarations, not the firmware |
| Robot ignores taps, screen dead, LED green | Stuck in `listening`. State is driven by protocol messages, so a client can leave it there; the launcher gates the home indicator and status bar on `is_xiaozhi_idle()`, so the whole UI goes with it. `POST /debug/reset` |
| A new on-screen widget kills taps or gestures | LVGL objects are **clickable by default**. Anything added to `lv_layer_top()` or over the avatar panel swallows input — `lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE)` |
| Boot hangs at "Logging in…" | LVGL touched from the app task without `DisplayLockGuard`. LVGL is not thread-safe; use an `lv_timer` if the work belongs in LVGL's own context |
| Added a new `.cc`/`.cpp` file, link fails with `undefined reference` to its symbols | `main/CMakeLists.txt` finds sources with `file(GLOB_RECURSE ...)`, and **CMake evaluates globs at configure time only**. An incremental `idf.py build` never re-runs it, so a brand-new file is invisible to the build no matter how correct it is. `idf.py reconfigure`, then build |
| Silent reboot loop: no panic, no backtrace, log line truncated mid-write | The console cannot report it — TinyUSB dies with the crash. **Enable core dumps** (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`; the partition already exists) and read the real cause back from flash with `esp_coredump ... info_corefile`. Do not theorise from the truncated log; three separate theories died before the dump named the task and the exception in one shot |
| Core dump reads back as version `0xffff` / "not supported" | The partition is erased — **the firmware never actually ran**. Easy to cause by flashing and then reading without letting it boot and crash first. An empty dump means "no evidence yet", never "the panic handler didn't run" |
| Boot loop, `assert failed: tcpip_callback ... (Invalid mbox)` right after an `SNTP init` line | A network call ran before lwIP's TCP/IP thread existed. lwIP does **not** degrade gracefully when called too early — it panics. Anything network-touching must stay gated on the network actually being up (`is_xiaozhi_ready()`), never on a UI-availability signal. Do not assume "it will just fail and retry" |
| Device with no USB host and no reachable WiFi is totally unresponsive: no menu, no home button, no touch | The home indicator and status bar were gated behind `is_xiaozhi_ready()`, which only latches when the state machine reaches idle — which requires "Network connected". Offline, it never fires and the UI is simply never created. Gate local UI on the **avatar** existing, never on connectivity |
| Internal RAM falls steadily across repeated retries until the device OOMs | A retry loop re-allocating FreeRTOS static task buffers (`StackType_t`/`StaticTask_t`) each attempt without freeing them. Measured 12371 → 2759 bytes over ten cycles, ~1060 B each. Allocate the buffers **once per slot and reuse**; freeing them is unsafe when tasks `vTaskDelete(NULL)` themselves |
| Streamed camera frames are one step stale | V4L2 returns the **oldest** queued buffer. `CaptureFresh()` discards the queue first; plain `StreamCaptures()` does not |
| Tap goes green, robot hears nothing, `audio_channel_open:false`, log spams `Channel timeout N seconds` | Upstream declares a channel dead after 120 s with no inbound frame, and `IsAudioChannelOpened()` is gated on it. That rule is for the client role; here the device is the server and an idle agent legitimately sends nothing for hours. `frames_rx:1` (just the `hello`) is the tell. Fixed by overriding `IsTimeout()` to false — TCP keepalive already reaps dead peers |
| Talking does nothing: `listening`, `audio_channel_open:true`, but `frames_tx` frozen | The audio pipeline did not survive a USB drop. `uptime_s` keeps counting, so the app never rebooted and everything *looks* healthy. `frames_tx` counts every send including audio — if it does not move while you talk, the mic is not reaching the wire. `/debug/reset` restores the state machine but **cannot restart the audio service**; press reset |
| Model describes the previous photo, or answers blind | Ordering, not latency. Realtime media is ordered against the audio clock, not conversation turns — a one-shot image must go via `send_client_content` |

---

## 6. Method that actually worked

1. **Read `/debug` first.** It is one request and rules out half the hypothesis space.
2. **Start log capture before reproducing.** Retroactive logs do not exist, and the CDC
   console drops lines under load — absence of a line is weak evidence.
3. **Test the layer in isolation before the whole stack.** The camera was proven with a
   50-line script that saved a JPEG to disk, so when the model misbehaved later there
   was no question about capture, encoding, or transport.
4. **Prefer a log line to a theory.** Every long detour in this project came from
   reasoning about code instead of reading output. The device→host bug was three flash
   cycles of theorising and one line of log.
5. **`uptime_s` before blaming yourself** — and before blaming the human. It reboots for
   many reasons, including someone pressing reset.
6. **Verify the fix by reproducing the failure.** Two bugs in `/debug/reset` and the
   keepalive path were found only by running the test after committing the "fix".

---

## 7. Conversation loop, in one paragraph

After `tts stop` the device does **not** return to idle: it goes `speaking → listening`
and emits `{"type":"listen","state":"start","mode":"auto"}`, opening the user's turn.
A client that never closes that turn leaves the device parked in `listening`, where the
idle servo animation does not run — this looks like a bug and is not. Mic audio only
flows once the app calls `OpenAudioChannel()`, which is gated behind a tap or wake word;
playback into the device needs no such thing. A tap on the face is the whole trigger,
and is also the privacy boundary: nothing leaves the device before it.

---

## 8. Remote debugging over Tailscale, no cable

Off by default. `CONFIG_STACKCHAN_TAILSCALE_ENABLE` brings up WiFi STA **alongside**
USB-NCM (not instead of it) and joins a tailnet using MicroLink, vendored whole at
`components/microlink_vendor` (fetched by `fetch_repos.py`, pinned to
`letalvoj/microlink@lab-integration` in `repos.json`).

The reason this needed almost no new code: `WebsocketServerProtocol` already binds
`INADDR_ANY`. Once the tailnet link is up, `/ws`, `/debug` and `/debug/reset` are
reachable at the device's tailnet (`100.x`) address with **zero protocol changes** --
the same server, a second interface. Tailscale's own WireGuard tunnel is the auth and
the encryption; this deliberately does not add a second credential system on top of it.

**Setup** -- put real values in `sdkconfig.defaults.local` (gitignored, never committed):

```
CONFIG_STACKCHAN_TAILSCALE_ENABLE=y
CONFIG_STACKCHAN_TAILSCALE_WIFI_SSID="your home network"
CONFIG_STACKCHAN_TAILSCALE_WIFI_PASSWORD="its password"
CONFIG_STACKCHAN_TAILSCALE_AUTH_KEY="tskey-auth-..."
CONFIG_STACKCHAN_TAILSCALE_DEVICE_NAME="stackchan"
```

Use a **reusable, ephemeral, pre-approved** Tailscale auth key with a short expiry --
ephemeral matters because a device that stops responding (crashed, unplugged,
reflashed with blank NVS) gets reaped automatically instead of leaving a stale
"online" node on the tailnet forever. Both the WiFi password and the auth key end up
baked into the flash image in plaintext -- the same tradeoff `microlink-lab`'s own
README documents and for the same reason: the device needs them, so they have to live
somewhere.

Then, from any machine on the same tailnet:

```bash
tailscale status | grep stackchan          # find the 100.x address once it registers
curl -s http://100.x.y.z:8081/debug | python3 -m json.tool
./wasm/.venv/bin/python wasm/clients/gemini_live.py --host 100.x.y.z
```

**Verified so far: it builds and links on real hardware.** Confirmed by actually
flipping the config on (not just reading the Kconfig) and rebuilding:

- `idf.py build` with `STACKCHAN_TAILSCALE_ENABLE=y` and placeholder secrets compiles
  and links clean. The default-off build is unaffected -- back to the same 0x382750
  byte / 29%-free image once the override is removed.
- Two real bugs surfaced doing that, both worth knowing if this ever needs touching
  again:
  - **Never put a system header include inside `namespace stackchan { }`.**
    `FreeRTOS.h` declares `struct _reent` at *global* scope for its TLS block; from
    inside a namespace that silently resolves to `stackchan::_reent` instead, and the
    resulting error ("field has incomplete type") points at FreeRTOS's own header,
    nowhere near the actual mistake. All of `tailscale_link.cc`'s system/vendor
    includes now sit above the `namespace` line, guarded by the same `#if`.
  - **A link error two floors down in someone else's AEAD code, from a missing
    cipher.** `ml_noise.c: undefined reference to mbedtls_chachapoly_*` --
    WireGuard's Noise handshake needs ChaCha20-Poly1305, and this project's mbedTLS
    build didn't otherwise include it. Fixed with three `select`s on
    `STACKCHAN_TAILSCALE_ENABLE` (`MBEDTLS_CHACHA20_C`, `_POLY1305_C`,
    `_CHACHAPOLY_C`) so enabling the feature pulls the cipher in automatically.
    **`select` in a Kconfig entry does not apply to an already-generated `sdkconfig`**
    -- an incremental `idf.py build` after editing `Kconfig.projbuild` silently kept
    building with the old (missing) values. `idf.py reconfigure`, or delete
    `sdkconfig` and rebuild, forces Kconfig to re-resolve.

**Not yet exercised on real hardware: WiFi actually joining, and registering with a
real tailnet.** That needs a real SSID/password and a real auth key, which is exactly
the two-line gitignored file above -- once those are in place this is a normal flash
cycle away from a first real test, not further code.

---

## 9. Quick reference

```bash
# state
curl -s http://192.168.7.1:8081/debug | python3 -m json.tool
# unstick without touching the device (drops client, forces idle, does NOT reboot)
curl -s -X POST http://192.168.7.1:8081/debug/reset
# the voice agent
./wasm/.venv/bin/python wasm/clients/gemini_live.py
# preview the face without flashing (renders the REAL skin natively)
./tools/facelab/build.sh && ./tools/facelab/grid.sh mine cute
# ports / mode
ls /dev/cu.usbmodem*; system_profiler SPUSBDataType | grep -A3 "Espressif\|USB JTAG"
# is the link up
ifconfig | grep -A6 "^en9" | grep "inet "
# photo, no model involved
./wasm/.venv/bin/python firmware/examples/jingle.py     # audio + servos, known-good demo
# tests (hardware ones skip when unplugged)
cd wasm && ./.venv/bin/python -m pytest tests -q
```

Summon the human with `say` — chain it onto the command that needs them, never as a
separate step.
