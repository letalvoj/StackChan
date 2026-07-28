# Flashing, testing and troubleshooting the USB-NCM build

For bringing a device up on the USB transport for the first time. Architecture is in
`ARCHITECTURE.md` §5; this is the operational side.

**Nothing here has been run against real hardware yet.** The firmware builds clean and
the QA harness has been verified against a mock device, but the first person to plug in a
board is doing the first real test. §4 exists because of that.

---

## 1. Two cables

This is the single most common way to lose an hour.

| Cable | Carries | Used for |
|---|---|---|
| **USB-OTG** (the one the device enumerates on) | CDC-NCM network | the protocol |
| **UART** | `ESP_LOG` console | flashing and *all* logging |

USB-Serial-JTAG shares GPIO19/20 with USB-OTG, so enabling NCM takes the pins and the USB
console disappears. **`idf.py monitor` over the USB cable will show nothing.** Logs come
out of UART0 only.

Also: plenty of USB cables are charge-only. If the host never sees a new device at all,
suspect the cable before the firmware.

---

## 2. Flash and watch

```bash
cd /Users/letalvoj/Projects/stackchan/wasm-chan && make esp32
```

```bash
cd firmware && idf.py -p /dev/tty.usbserial-XXXX flash monitor
```

Confirm the build is the one you think it is:

```bash
grep -E "CONFIG_CONNECTION_TYPE|CONFIG_TINYUSB_NET" firmware/sdkconfig | grep -v '^#'
```

Expected: `CONFIG_CONNECTION_TYPE_USB_NCM=y` and `CONFIG_TINYUSB_NET_MODE_NCM=y`.

### The log sequence that means it worked

```
I UsbNetBoard: USB adapter MAC 02:xx:xx:xx:xx:xx (host sees this as its peer)
I UsbNetBoard: USB network up: device 192.168.7.1, host will be offered 192.168.7.2
I UsbNetBoard: protocol endpoint pinned to ws://192.168.7.2:8081/ws
I UsbNetBoard: USB cable attached; waiting for the host to take a DHCP lease
I UsbNetBoard: Host took DHCP lease 192.168.7.2; USB link is usable
I Application:  Using WebSocket protocol over USB networking
I Application:  USB transport: skipping remote version/assets checks
```

If it stalls, a heartbeat fires every 5 s telling you exactly how far it got:

```
W UsbNetBoard: waiting for host: usb_mounted=yes dhcp_lease=NO ...
```

`usb_mounted` is the USB-level attach; `dhcp_lease` is the host actually taking an
address. They fail for completely different reasons — see §4.

---

## 3. Host side

### Check the link came up

```bash
ip -br link | grep -i usb; ip -4 addr show dev usb0
```

Expected: an interface holding `192.168.7.2/24`. On macOS use `ifconfig` and look for a
new `enX`. If the interface exists but has no address, the device's DHCP server did not
answer — the USB link is up but the network is not.

### Run the QA harness

The device **listens**, so dial it:

```bash
cd wasm && ./.venv/bin/python qa_selftest.py --connect 192.168.7.1
```

It walks every protocol path and prints a pass/fail bar:

```
  PASS handshake       0.0s  device_id=DA:E5:31:89:AB:CD session=4243f985
  PASS mcp.tools       0.1s  12 tools: self.camera.take_photo, self.get_device_status…
  PASS device.status   0.1s  {"content":[...]}
  PASS screen          2.3s  brightness 30/100/70 + theme dark/light applied
  PASS camera.photo    1.9s  {"content":[...]}
  PASS tts.downlink    0.6s  9 frames sent; codec is Opus, see TASKS.md
  PASS mic.uplink      5.5s  82 frames / 6560 bytes

  ███████  7/7 passed
```

Audio now works end to end: the server adapts to whatever the device advertises
(Opus from firmware, PCM from the WASM harness), so `tts.downlink` should produce an
audible 440 Hz tone rather than noise.

`screen` is deliberately **visible** — the panel dims, brightens and flips theme while you
watch. A JSON reply only proves the message arrived; the panel changing proves the whole
display path. Same for `photo`. During `mic` it will prompt you to make a noise.

Exit status is 0 only if everything passed, so it can gate a fixture or CI job.

Useful flags:

```bash
./.venv/bin/python qa_selftest.py --only mcp,status   # subset
./.venv/bin/python qa_selftest.py --verbose           # echo every inbound frame
./.venv/bin/python qa_selftest.py --keep-open         # stay connected and watch
```

### Or run the real gateway

```bash
cd wasm && ./.venv/bin/python serve.py
```

Same port, same endpoint — `serve.py` also serves the WASM UI, so the browser build and a
physical device can be compared side by side against one server.

---

## 4. When it does not work

Ordered by how often each is likely to be the cause.

### Host sees no new device at all

Not firmware. In order: charge-only cable; wrong port (the OTG port, not the UART one);
device not actually running (check UART for a boot loop). Confirm with `dmesg -w` on Linux
while plugging — silence means the host saw no electrical attach.

### Host sees the device but no network interface

The device enumerated but the class driver did not bind. Check `dmesg` for `cdc_ncm`. If
you see `cdc_acm` instead, you flashed the **SLIP** build — check `CONFIG_CONNECTION_TYPE`
per §2. Some older kernels and Windows handle NCM poorly; NCM is well supported on Linux
and modern macOS, which is why RNDIS was not chosen.

### `usb_mounted=yes` but `dhcp_lease=NO`

Device is enumerated, host is not taking an address. Either the host set a static IP on
the interface, or NetworkManager is ignoring it. Force it:

```bash
sudo dhclient -v usb0
```

If that gets an address, the device side is fine and it is host network policy. As a
workaround you can assign statically — `sudo ip addr add 192.168.7.2/24 dev usb0` — but
note the firmware fires "connected" **on the DHCP lease**, so a static address means that
event never fires and the device will never dial out. Prefer fixing DHCP.

### Link is up, but the device never connects to the harness

Check the endpoint matches: the firmware logs `protocol endpoint pinned to …` and it must
be reachable from the device. From the host, `ss -lntp | grep 8081` should show the
harness listening on `0.0.0.0`, not `127.0.0.1`. **Host firewall is a common culprit** —
the device is on a new interface most firewall profiles treat as untrusted.

### `TX drop: host not draining USB`

The host stopped reading. Rate-limited to one line per 100 drops. Occasional lines under
load are survivable; a continuous stream means the host side is wedged.

### Everything passes but audio is silent or noise

**Expected today.** The device sends and expects **Opus**; the gateway backends unpack raw
PCM16, and nothing negotiates `format`. `tts.downlink` and `mic.uplink` verify that frames
*flow*, not that they decode — that separation is deliberate so a wiring fault looks
different from the known codec gap. Tracked as the top P0 in `TASKS.md`.

### Device reboots or hangs when the host disconnects

Should not happen — unplug is handled via `tud_umount_cb` and closes the audio channel.
If it does, capture the UART log across the unplug; that is a real bug worth a stack trace.

---

## 5. Falling back to SLIP

If NCM turns out to be unworkable on your host, the serial transport still builds:

```
CONFIG_CONNECTION_TYPE_USB_SLIP=y
CONFIG_TINYUSB_CDC_ENABLED=y
CONFIG_TINYUSB_CDC_COUNT=1
```

(replacing the two NCM lines in `firmware/sdkconfig.defaults`), then
`rm firmware/sdkconfig && make esp32`. The device appears as `/dev/ttyACM*` and is driven
by `wasm/gateway.py --transport=serial` instead. It is a private wire format with no
integrity check — see `ARCHITECTURE.md` §5.5 and the caveats in `TASKS.md`.

---

## 6. Capturing USB traffic

If it comes to bus-level debugging, the vendored TinyUSB tree ships a `usbmon` skill for
capturing host-side URBs into Wireshark. Reach for it only after §4 is exhausted —
enumeration and DHCP problems are almost always visible in `dmesg` and the device's own
heartbeat log.
