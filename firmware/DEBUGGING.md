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

**Two physical actions, always. This is structural, not a fluke.**

```bash
# 1. Human: hold RST ~3s until the LED turns green  → device enters download mode
say "Vojta, hold reset for three seconds until green, then tell me."

# 2. Verify 0x1001 on /dev/cu.usbmodem111101, then:
cd firmware && source /Users/letalvoj/Projects/stackchan/esp-idf/export.sh && idf.py build
cd build && python -m esptool --chip esp32s3 -p /dev/cu.usbmodem111101 -b 460800 \
    --before default_reset --after hard_reset write_flash "@flash_args" \
  && say "flash finished, pls press reset to reboot"

# 3. Human: short press RST  → boots the app
```

**`esptool` prints "Hard resetting via RTS pin" and it does nothing.** The S3 has no
USB-serial bridge chip — USB is native, there is no RTS line wired to anything. The
message is unconditional output, not a report of success. Always expect step 3.

Chain `say` onto the flash command itself rather than a separate call; there is always
a human in this loop and they may be away from the keyboard.

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
```

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
| Streamed camera frames are one step stale | V4L2 returns the **oldest** queued buffer. `CaptureFresh()` discards the queue first; plain `StreamCaptures()` does not |
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

## 8. Quick reference

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
