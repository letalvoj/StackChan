# Device WebSocket & MCP Protocol Specification

This document is the code-grounded specification for network communication with the StackChan firmware. Every behavioural claim below was cross-checked against the source on 2026-09-23; where this build differs from upstream xiaozhi, the difference is called out.

It covers the physical network topology, connection lifecycle, duplex Opus audio streaming, camera frame retrieval, JSON control messages, and the embedded Model Context Protocol (MCP) server.

---

## 1. Architecture & Networking Overview

The firmware implements an **inverted WebSocket server model**:
- **Device is the server (`WebsocketServerProtocol`)**: The robot listens for incoming TCP connections; the external host (PC, server, edge box, or LLM agent) connects as a client.
- **Physical interfaces (2 Network Devices)**:
  1. **USB CDC-NCM (Ethernet-over-USB)**: Implemented in `main/hal/board/usb_net_board.cc`. When plugged into a host via USB-C, the robot enumerates as an Ethernet adapter (`usb0` on Linux, `enX` on macOS). The robot runs a local DHCP server with fixed IP `192.168.7.1` (netmask `255.255.255.0`) and offers `192.168.7.2` to the host. Router and DNS options are zeroed out so host default routes are unaffected.
  2. **Wi-Fi Station (`StackChanWifiStation`)**: Implemented in `main/hal/board/network_link.cc` and `main/hal/utils/wifi_connect/wifi_station.cc`. When enabled (`CONFIG_STACKCHAN_WIFI_ENABLE=y`), the device associates with the local wireless network and receives a DHCP IP. If also enabled (`CONFIG_STACKCHAN_TAILSCALE_ENABLE=y`), a Tailscale VPN tunnel (MicroLink) is established on top.
- **Single Listening Daemon**:
  - The HTTP/WebSocket daemon (`WebsocketServerProtocol` in `main/protocols/websocket_server_protocol.cc`) binds `INADDR_ANY` (`0.0.0.0`) on port `CONFIG_USB_NET_LISTEN_PORT` (default: **`8081`**). `esp_http_server` also opens an internal loopback control socket on `8082`; it is not a service and hosts should not connect to it.
  - The exact same server endpoints are reachable via USB (`192.168.7.1:8081`), local Wi-Fi (`<wifi_ip>:8081`), or Tailnet (`<tailnet_ip>:8081`).
- **Active Protocol Implementation**:
  - Under `CONFIG_CONNECTION_TYPE_USB_NCM=y` (the default), `main/main.cpp` launches directly into `xiaozhi` mode on boot, instantiating `WebsocketServerProtocol` (`xiaozhi-esp32/main/application.cc`).
  - Face taps, camera taps, audio pipelines, and MCP commands interact exclusively with this protocol instance.
  - *(Note: `hal_ws_avatar.cpp` contains an unrelated outbound client for remote avatar video calling in Mooncake mode; it is not active during USB-NCM operation).*

---

## 2. Server Endpoints & Connection Rules

The internal `esp_http_server` registers four endpoints on port `8081`:

| Endpoint | Method | Transport | Purpose |
|---|---|---|---|
| `/ws` | `GET` (Upgrade) | WebSocket | Full duplex bidirectional communication: JSON control messages, raw Opus audio, and MCP tool envelopes. |
| `/debug` | `GET` | Plain HTTP | Read-only JSON diagnostics (see §9). Safe to poll without disturbing the active WebSocket. |
| `/debug/history` | `GET` | Plain HTTP | Battery history: up to 120 recent samples (see §9). |
| `/debug/reset` | `POST` | Plain HTTP | Recovery endpoint: disconnects any active WebSocket client and resets device state to `idle`. Does not reboot. |

If the server fails to start (typically `httpd_start failed: ESP_ERR_HTTPD_TASK` — no contiguous internal RAM for its task stack), nothing retries it: the device boots to a normal-looking face, answers ping, and refuses every connection to `8081`. See `DEBUGGING.md`.

### Single-Client Policy & Eviction
- Exactly **one active WebSocket client** is allowed at a time (`client_fd_`). `esp_http_server` itself holds up to `max_open_sockets = 3` sockets with `lru_purge_enable = true`, but only one is ever the adopted client.
- A client is **adopted on its first inbound data frame**, not on the WebSocket upgrade (see §3). WebSocket pings do not count: `esp_http_server` answers them itself.
- When a second client's first frame arrives, the device **evicts the previous client**: it closes the old socket (`httpd_sess_trigger_close`) and adopts the new one.
- Eviction resets only the audio channel (§4). It does **not** reset the device state, so a new client can inherit a device that is still `speaking` or `listening`. Use `POST /debug/reset` first if that matters.
- Rationale: Device-originated traffic (microphone Opus streams, TTS state transitions, sensor events) cannot be multiplexed without explicit routing policy. Enforcing single ownership prevents race conditions.

### TCP Keepalive & Disconnect Detection
- `esp_http_server` keepalive is configured: `keep_alive_idle = 15s`, `keep_alive_interval = 5s`, `keep_alive_count = 3` (~35s to reap a dead socket if no TCP FIN/RST is received).
- Application-level WebSocket timeout (`IsTimeout()`) is explicitly disabled: an idle connected client (e.g. waiting for a user face tap) may stay connected indefinitely without sending keepalive pings.
- If Wi-Fi disconnects, `WebsocketServerProtocol::DropRemoteClient()` explicitly terminates non-USB clients (`HostPeerAddressV4() != 192.168.7.0/24`) to immediately clean up stale sockets.
- A frame larger than **64 KB** (`kMaxFrameBytes`) makes the handler fail, which closes the connection.

---

## 3. WebSocket Handshake Sequence

Upon connecting to `ws://<device_ip>:8081/ws`:

```
   Host (Client)                             StackChan (Server)
        |                                             |
        | ------------- TCP Handshake ------------->  |
        | <------------ WebSocket Upgrade ----------  |
        |                                             |
        | ------------ Host Hello Frame ------------> | (adopts the socket,
        |                                             |  sets session_id,
        |                                             |  starts 20ms timer)
        | <------------ Device Hello Frame ---------- |
        |                                             |
```

> **The host speaks first.** The device adopts a client on its first *inbound frame*,
> not on the upgrade — on this IDF the handshake is never dispatched to the handler — so
> until the host sends something the device has no socket to greet. A client that waits
> for the device's `hello` before sending anything waits forever on an open socket.
> Send the host `hello` immediately; the device `hello` follows within ~20 ms.

### Step 1: Host sends `hello` (immediately after connecting)
```json
{
  "type": "hello",
  "transport": "websocket",
  "session_id": "my-session-uuid-1234",
  "audio_params": {
    "sample_rate": 24000,
    "frame_duration": 60
  }
}
```
- `"transport"` (string, **required**): must be `"websocket"`. A host `hello` with any other value, or none, is logged and **ignored entirely** — no session, no handshake.
- `"session_id"` (string, optional but recommended): stored if present and echoed on device messages sent after it. It is not reset between clients, so a host that omits it inherits the previous session's ID. The device does not read `session_id` on any host message after the hello.
- `"audio_params"` (optional): sample rate (Hz) and frame duration (ms) of **downlink** audio (defaults: 24000 Hz, 60 ms). Other rates are resampled on the device.
- Receiving it sets `WS_SERVER_SERVER_HELLO_EVENT`, which is what lets an audio channel open (§4).

### Step 2: Device replies with `hello` (within ~20 ms of the host's first frame)
The device transmits a text frame with its capabilities, IDs, and microphone audio configuration. This is the one device message without a `session_id`:
```json
{
  "type": "hello",
  "version": 1,
  "features": {
    "mcp": true
  },
  "transport": "websocket",
  "device_id": "aa:bb:cc:dd:ee:ff",
  "client_id": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 60
  }
}
```
`device_id` is the MAC address in lowercase hex.

---

## 4. Duplex Audio Streaming

Audio is transmitted via **Binary WebSocket Frames**.
- Frames carry **raw Opus packets** with **no envelope, no container headers (no Ogg/WAV), and no binary prefixes**.
- Maximum frame size accepted by server: **64 KB** (`kMaxFrameBytes`); larger frames close the connection.

### Speaker vs. microphone

The two directions are gated differently:

- **Speaker (host → device)** works as soon as the host has sent its `hello` — no tap needed. This is what makes the robot usable for announcements: connect, `hello`, `tts start`, stream, `tts stop`.
- **Microphone (device → host)** flows only while the *audio channel* is open (`/debug` reports it as `audio_channel_open`). The channel is the user's conversation session, and only the user opens it:

| Event | Audio channel (microphone session) |
|---|---|
| New client adopted | **closed** |
| Face tap, camera tap or wake word while idle | **opened** — once the host `hello` has arrived. If it has not, the device waits 10 s, shows `SERVER_TIMEOUT` and returns to idle |
| `tts stop`, returning to `idle` | stays **open** |
| Face tap while `listening` | **closed** — device sends `goodbye` and goes `idle` |
| Protocol error | **closed** |

Before 2026-09-23 the speaker was gated on the audio channel too, so audio sent before anyone had tapped the robot — and after every reconnect — was silently discarded.

### Uplink: Device Microphone -> Host
1. **Trigger**: User taps the robot's face, taps the camera icon, a wake word is detected, or a `tts stop` returns the device to a turn it interrupted (see Downlink step 4).
2. **State**: The device enters `kDeviceStateListening` and transmits a `listen` start frame:
   ```json
   {"session_id":"my-session-uuid-1234","type":"listen","state":"start","mode":"auto"}
   ```
   *(If started via camera tap, `"video": true` is appended).* This build always uses `mode: "auto"`; `manual` and `realtime` exist upstream but are not reachable here.
3. **Audio Stream**: The microphone capture pipeline (`AudioProcessor` -> Opus encoder) produces **16 kHz, 1 channel (mono), 60 ms Opus frames** and sends each packet as a Binary WebSocket frame.
4. **VAD Signals**: While listening, the device analyzes voice activity and emits real-time VAD text frames:
   ```json
   {"session_id":"my-session-uuid-1234","type":"vad","state":"speech"}
   {"session_id":"my-session-uuid-1234","type":"vad","state":"silence"}
   ```
5. **Turn Completion**: The device does **not** end the user's turn itself — it never sends `listen stop` in this build. The host decides when the user has finished (typically from `vad`), then replies with `tts start`. A face tap while listening ends the whole session instead: the device sends `goodbye` and goes `idle`.

### Downlink: Host Speaker Audio (TTS) -> Device
1. **State Activation**: Before streaming binary audio, the host puts the device into `speaking` state by sending:
   ```json
   {"session_id":"my-session-uuid-1234","type":"tts","state":"start"}
   ```
   > **Which frames play**: binary Opus frames are accepted while the device is `speaking`, `listening` or `idle`, from a host that has sent its `hello`. They are discarded on arrival — never stored for later — before the host `hello` or while the device is busy elsewhere (connecting, activating, Wi-Fi setup, upgrading). Audio that arrives just before its `tts start` (for example after a pause) is kept and plays when speaking begins, rather than being flushed.
2. **Text Bubble Display (Optional)**: The host can update the subtitle in the speech bubble:
   ```json
   {"session_id":"my-session-uuid-1234","type":"tts","state":"sentence_start","text":"Hello, I am StackChan!"}
   ```
3. **Audio Stream**: Host transmits raw Opus binary frames matching the negotiated audio parameters (typically 24 kHz, 60 ms mono). The device decodes and plays them via the onboard I2S codec (`CoreS3AudioCodec`).
   - **Pace at about real time.** The decode queue holds 40 packets (~2.4 s at 60 ms) and incoming frames are pushed without waiting, so frames sent much faster than real time are **silently dropped**.
4. **Turn Completion**: When the host finishes sending audio:
   ```json
   {"session_id":"my-session-uuid-1234","type":"tts","state":"stop"}
   ```
   `tts stop` returns the device to whatever state `tts start` interrupted: `listening` if it was listening (the device then reopens the user's turn with a fresh `listen start`), otherwise `idle`. Returning to `listening` resets the decoder and clears the playback queue, so audio still buffered at that moment is cut off — send `tts stop` only once the audio has had time to play out.

### User Barge-in / Interruption
- **Face tap while speaking**: the device sends `abort` and **stays `speaking`**. It does not flush audio: queued and newly arriving frames keep playing. The host must stop streaming and send `tts stop`.
  ```json
  {"session_id":"my-session-uuid-1234","type":"abort"}
  ```
- **Wake word**: the device sends `abort` with `"reason": "wake_word_detected"`, flushes playback, enters `listening` and sends `listen start`.

---

## 5. Camera & Vision Protocol (Interleaved via MCP)

There is **no dedicated video streaming port or binary video frame**. Video frames are requested on demand by the host using the Model Context Protocol (MCP) tool `self.camera.capture`.

### Live Interleaved Camera Flow
1. **Initiation**: User taps the camera icon on the LCD screen (bottom-right).
2. The device sends `listen` with `"video": true`:
   ```json
   {"session_id":"my-session-uuid-1234","type":"listen","state":"start","mode":"auto","video":true}
   ```
3. While `video: true` is active and the device emits `{"type":"vad","state":"speech"}`, the host pulls camera frames by sending MCP tool calls at ~1 fps:
   ```json
   {
     "session_id": "my-session-uuid-1234",
     "type": "mcp",
     "payload": {
       "jsonrpc": "2.0",
       "id": 101,
       "method": "tools/call",
       "params": {
         "name": "self.camera.capture",
         "arguments": {
           "stream": true
         }
       }
     }
   }
   ```
4. The device captures a fresh frame (`CaptureFresh()` drops old queued V4L2 frames), encodes it to JPEG (quality 55 for streaming), blinks the on-screen camera indicator, and returns the image base64-encoded.

   > **Wire shape — note the nesting.** The content item is `{"type":"image","image":"<string>"}`, where `"image"` is itself a **JSON-encoded string** holding the actual `{"type":"image","mimeType":"image/jpeg","data":"<base64>"}` object. Clients must `JSON.parse(content[0].image)` to reach `data`. This is how `McpTool::Call` in `xiaozhi-esp32/main/mcp_server.h` serialises images; it is not the flat MCP image shape.

   ```json
   {
     "session_id": "my-session-uuid-1234",
     "type": "mcp",
     "payload": {
       "jsonrpc": "2.0",
       "id": 101,
       "result": {
         "content": [
           {
             "type": "image",
             "image": "{\"type\":\"image\",\"mimeType\":\"image/jpeg\",\"data\":\"/9j/4AAQSkZJRgABAQAAAQABAAD/2wBD...\"}"
           }
         ],
         "isError": false
       }
     }
   }
   ```

### Capture Modes (`self.camera.capture`)
- **`stream: true` (Live Conversational Vision)**:
  - Quality: JPEG 55.
  - Non-blocking, no shutter sound, no screen preview disruption.
  - Grabs fresh frame directly from sensor pipeline.
- **`stream: false` (Explicit Snapshot / Photo)**:
  - Quality: JPEG 80.
  - Plays camera shutter sound (`OGG_CAMERA_SHUTTER`).
  - Dequeues three sensor frames and keeps the third, so auto-exposure/auto-white-balance can settle.
  - Displays preview on the robot's screen.
  - Automatically waits up to 1.5 seconds for head servo motion to settle before capturing, preventing motion blur.

---

## 6. JSON Control Messages Reference

Device messages carry `"session_id"` (except the device `hello`) and `"type"`. On host messages only `"type"` is read.

### Device -> Host Messages

| `type` | Parameters / Fields | Description |
|---|---|---|
| `hello` | `version`, `features`, `transport`, `device_id`, `client_id`, `audio_params` | Handshake reply, sent ~20 ms after the host's first frame. No `session_id`. |
| `listen` | `state: "start"`<br>`mode: "auto"`<br>`video: true` *(optional)* | Microphone opened. `video: true` means the conversation was started via the camera button. Upstream also defines `state: "stop"\|"detect"` and modes `manual`/`realtime`; this build does not emit them (`detect` needs `CONFIG_SEND_WAKE_WORD_DATA`, which is off). |
| `vad` | `state: "speech"\|"silence"` | Real-time Voice Activity Detection, sent only while listening with the audio channel open. Used by the host to end the user's turn and to gate vision streaming. |
| `abort` | `reason: "wake_word_detected"` *(optional)* | User interrupted device speech via face tap or wake word. Host should stop streaming and abort TTS generation (see §4 Barge-in). |
| `sensor` | `event: "head_pet"\|"shaken"` | Physical interaction events, sent only while `listening`, sharing one 4 s rate limit. |
| `mcp` | `payload: { ... }` | JSON-RPC 2.0 responses from the embedded MCP tool server. |
| `goodbye` | — | The device closed the audio channel (a face tap while listening). The WebSocket stays open. |

### Host -> Device Messages

| `type` | Parameters / Fields | Description |
|---|---|---|
| `hello` | `transport: "websocket"`<br>`session_id: "<string>"`<br>`audio_params: { sample_rate, frame_duration }` | Must be the host's first frame (§3). Ignored if `transport` is not `"websocket"`. |
| `tts` | `state: "start"\|"stop"\|"sentence_start"`<br>`text: "<subtitle>"` *(on sentence_start)* | `start` sets the device to `speaking`; `stop` returns to the state `start` interrupted (`listening` or `idle`); `sentence_start` shows the subtitle in the speech bubble. See §4. |
| `stt` | `text: "<user transcript>"` | Accepted and **not displayed**. Upstream draws it in a chat log; this face has only the robot's own speech bubble, and putting the person's words there would show the robot saying them. To put text on screen use the MCP tools `self.screen.show_speech_bubble` / `self.screen.hide_speech_bubble`. |
| `llm` | `emotion: "<emotion_name>"` | Sets avatar facial expression. Supported: `neutral`, `happy`, `laughing`, `angry`, `sad`, `crying`, `sleepy`, `doubtful`. `laughing` is a distinct expression, not an alias for `happy`: it plays a short laugh as soon as it is set, and laughs while talking. `crying` is still an alias for `sad`. `sleepy` also puts `Zzz…` in the speech bubble; setting any other emotion, any status change, or `self.screen.hide_speech_bubble` takes it down. |
| `mcp` | `payload: { ... }` | JSON-RPC 2.0 requests (`initialize`, `tools/list`, `tools/call`) to the MCP server. |
| `system` | `command: "reboot"` | Executes system management commands (`reboot` triggers `esp_restart()`). |
| `alert` | `status: "<title>"`, `message: "<body>"`, `emotion: "<name>"` | Not a modal overlay on this face: `message` is written into the speech bubble and the emotion is set (names outside the list above fall back to `neutral`). `status` is shown only briefly before `message` replaces it. |

---

## 7. Model Context Protocol (MCP) Server

The robot embeds a JSON-RPC 2.0 Model Context Protocol server inside the WebSocket protocol (`{"type": "mcp", "payload": { ... }}`).

### JSON-RPC Framing
- **Request envelope**:
  ```json
  {"session_id":"my-session-uuid-1234","type":"mcp","payload":{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}}
  ```
- **Response envelope**:
  ```json
  {"session_id":"my-session-uuid-1234","type":"mcp","payload":{"jsonrpc":"2.0","id":1,"result":{ ... }}}
  ```
- **`id` must be a JSON number.** A request with a string `id` is logged and dropped with no reply. `notifications/*` methods are ignored.
- **Errors** carry only a `message`, no JSON-RPC `code`:
  ```json
  {"session_id":"...","type":"mcp","payload":{"jsonrpc":"2.0","id":1,"error":{"message":"..."}}}
  ```
- Tool failures — including argument validation such as `"Value exceeds maximum allowed: N"` — come back as this `error`, not as `result` with `isError: true`. `isError` is always `false` in successful results.

### Supported Methods
1. **`initialize`**:
   - Parameters: `capabilities: { vision: { url, token } }` (optional; the vision URL is what `self.camera.take_photo` posts to).
   - Response: `protocolVersion: "2024-11-05"`, server info (`name`, firmware `version`), and tool capabilities.
2. **`tools/list`**:
   - Parameters: `cursor` (optional string for pagination), `withUserTools` (optional bool).
   - Response: `tools: [ ... ]` and, when truncated, `nextCursor`. Pages are capped at ~8000 bytes; `nextCursor` is the name of the next tool to list.
3. **`tools/call`**:
   - Parameters: `name: "<tool_name>"`, `arguments: { ... }`
   - Response: `content: [ ... ]` with one `text` item (`{"type":"text","text":"..."}`; booleans become `"true"`/`"false"`) or one `image` item (nested shape, see §5), and `isError: false`. Tool executions are scheduled on the main thread via `Application::Schedule`.

---

## 8. Complete MCP Tool Catalog

### Robot Motion, Hardware & Sensory Tools (`main/hal/hal_mcp.cpp`)

#### `self.robot.get_head_angles`
- **Description**: Returns current head orientation angles in degrees.
- **Parameters**: None.
- **Returns**: `{"yaw": <int>, "pitch": <int>}` as text (e.g. `{"yaw": 0, "pitch": 0}`).

#### `self.robot.set_head_angles`
- **Description**: Moves robot head servos to desired orientation. Returns immediately without blocking audio.
- **Parameters**:
  - `yaw` (integer, default: `-9999` to leave unchanged): Physical range `-128` to `128`. **Negative values turn toward viewer's left (robot's right); positive values turn toward viewer's right (robot's left)**. Recommended interaction range: `-45` to `45`. The schema's minimum is `-9999` (the sentinel), so out-of-range negatives are not rejected by validation.
  - `pitch` (integer, default: `-9999` to leave unchanged): Physical range `0` to `90` (0 is level/down, 90 is looking straight up). Same `-9999` schema minimum.
  - `speed` (integer, default: `150`, range `100` to `1000`): Servo motion speed.

#### `self.camera.capture`
- **Description**: Captures image from onboard camera and returns a base64 JPEG in an image content item.
- **Parameters**:
  - `stream` (boolean, default `false`): `true` for silent, low-latency 55-quality stream frame; `false` for 80-quality shuttered photo with LCD preview and head-settle delay.
- **Returns**: One image content item in the nested shape described in §5.

#### `self.robot.set_led_color`
- **Description**: Sets the resting colour of all twelve head LEDs (the base layer; status and touch effects are composited on top — see `LEDS.md`).
- **Parameters** (all optional, default `0`):
  - `red` (integer, 0 to 168)
  - `green` (integer, 0 to 168)
  - `blue` (integer, 0 to 168)

#### `self.robot.play_sound`
- **Description**: Plays short onboard sound effect chime.
- **Parameters**:
  - `name` (string, required): Allowed values: `"success"`, `"exclamation"`, `"popup"`, `"vibration"`.

#### `self.robot.create_reminder`
- **Description**: Sets an alarm/reminder timer on the device.
- **Parameters**:
  - `duration_seconds` (integer, 1 to 86400, default `60`)
  - `message` (string, default `"Time's up!"`)
  - `repeat` (boolean, default `false`)
- **Returns**: Integer reminder ID.

#### `self.robot.get_reminders`
- **Description**: Lists all currently active reminders.
- **Parameters**: None.
- **Returns**: JSON array string: `[{"id": 1, "duration_ms": 60000, "message": "...", "repeat": false}]`.

#### `self.robot.stop_reminder`
- **Description**: Cancels an active reminder by its ID.
- **Parameters**: `id` (integer, optional, default `-1`).

#### `self.robot.end_conversation`
- **Description**: Returns the robot to standby/idle. It sends no `goodbye` and no `listen stop`, and leaves the audio channel open; the host only receives the tool's `true` result.

---

### System & Display Tools (`xiaozhi-esp32/main/mcp_server.cc`)

#### `self.get_device_status`
- **Description**: Returns live device telemetry JSON (battery level/charging, speaker volume, screen brightness, network status, uptime).
- **Parameters**: None.

#### `self.audio_speaker.set_volume`
- **Description**: Sets speaker output volume. Safe to call while the speaker is powered down after silence: the value is recorded and applied when output resumes.
- **Parameters**: `volume` (integer, 0 to 100).

#### `self.screen.set_brightness`
- **Description**: Adjusts LCD backlight brightness.
- **Parameters**: `brightness` (integer, 0 to 100).

#### `self.screen.set_theme`
- **Description**: Switches UI color theme.
- **Parameters**: `theme` (string: `"light"` or `"dark"`). An unknown name returns the text `"false"` rather than an error.

#### `self.camera.take_photo`
- **Description**: Takes a photo and asks a remote vision service about it. Model-visible (not user-only).
- **Parameters**: `question` (string, required).
- **Behaviour**: POSTs the photo to the vision `url` (with `token`) supplied in `initialize`'s `capabilities.vision`, and returns that service's text answer. Only useful after `initialize` has provided a vision URL; for raw frames use `self.camera.capture`.

#### User-Only Tools
Included in `tools/list` only when `withUserTools: true`. Being user-only hides them from a model's `tools/list`; `tools/call` reaches them by name regardless.
- `self.get_system_info`: Returns flash, chip model, and firmware version.
- `self.reboot`: Reboots device.
- `self.upgrade_firmware`: Triggers OTA firmware download from URL.
- `self.screen.get_info`: Returns screen `width`, `height` and `monochrome`.
- `self.screen.snapshot`: Takes screenshot of UI and uploads to URL.
- `self.screen.preview_image`: Displays remote image on screen.
- `self.assets.set_download_url`: Sets custom asset download URL.
- `self.screen.show_speech_bubble` (`text`, string, non-empty): Shows text in the avatar's speech bubble. Last writer wins — the next `tts sentence_start` subtitle replaces it, and the device going idle clears it. An empty `text` is an error, not a hide. (`main/hal/hal_mcp.cpp`)
- `self.screen.hide_speech_bubble`: Hides the speech bubble, whatever it shows, including the sleepy `Zzz…`. (`main/hal/hal_mcp.cpp`)

---

## 9. Auxiliary HTTP Diagnostic Endpoints

### `GET /debug`
Returns real-time operational status as JSON. Safe to poll continuously. The main fields:
```json
{
  "uptime_s": 1234,
  "version": "2026-09-23T11:32",
  "device_state": "idle",
  "xiaozhi_ready": true,
  "client_fd": 54,
  "has_client": true,
  "audio_channel_open": false,
  "frames_rx": 412,
  "frames_tx": 280,
  "send_failures": 0,
  "last_send_err": "ESP_OK",
  "heap_free": 8026635,
  "heap_min": 8003464,
  "wifi_ip": "192.168.1.150",
  "tcpip_stack_free_min": 6464,
  "internal_free": 23859,
  "internal_largest_block": 8192,
  "battery_level": 95,
  "battery_charging": false,
  "battery_discharging": false,
  "tailnet": "registered ip=100.64.0.5 peers=2"
}
```
It also reports charger/PMIC diagnostics (`battery_mv`, `charge_phase`, `pmic_read_failures`, `chg_v_reg`, `chg_v_reg_stock`, `iterm_reg`, `status1_reg`, `vbus_ilim_reg`, `battery_mv_split`, `battery_mv_split2`, `reg_batfet_0x12`, `reg_minsys_0x14`, `reg_vindpm_0x15`, `reg_voff_0x24`).

*Note: `internal_largest_block` is the number to watch on ESP32-S3: FreeRTOS task stacks require contiguous internal SRAM, and the HTTP server needs an 8 KB block at boot.*

### `GET /debug/history`
Recent battery samples, oldest first, up to 120:
```json
{"samples":[{"t":123,"mv":3979,"lvl":100,"phase":4,"chg":0,"dis":0}],"count":1}
```
`chg` and `dis` are `0`/`1` integers (charging / discharging), not booleans.

### `POST /debug/reset`
Forces an immediate cleanup when the robot is in a bad state:
- Closes the active WebSocket client socket (`httpd_sess_trigger_close`).
- Requests device state `idle` (refused while `starting` or `wifi_configuring`, so `now` is not always `idle`).
- Returns: `{"was":"listening","now":"idle"}`.

---

## 10. Step-by-Step Client Implementation Guide

If you are implementing a custom backend, agent, or gateway in Python, Node.js, Go, or Rust, follow this exact lifecycle:

1. **Connect**:
   - Connect WebSocket to `ws://192.168.7.1:8081/ws` (or Wi-Fi IP).
2. **Handshake — send first**:
   - Immediately send the host `{"type":"hello", "transport":"websocket", "session_id":"<id>", "audio_params":{"sample_rate":24000,"frame_duration":60}}`.
   - Then receive the device `{"type":"hello", ...}` (~20 ms later). Do **not** wait for the device to speak first — it never will.
3. **Listen for User Speech**:
   - Wait for `{"type":"listen", "state":"start", ...}` (the user tapped the face or camera icon, or said the wake word).
   - Read incoming Binary WebSocket frames (each frame is one raw Opus packet @ 16 kHz mono). Feed into Opus decoder / STT engine.
   - Monitor `{"type":"vad", "state":"speech"|"silence"}` and decide yourself when the user has finished — the device does not send `listen stop`.
   - `{"type":"goodbye"}` means the user tapped to end the session; the microphone is closed until the next tap. The speaker keeps working.
4. **Interleaved Camera Capture (Optional)**:
   - If `listen` contained `"video": true` and VAD is `"speech"`, invoke MCP `tools/call` with `self.camera.capture` (`{"stream": true}`) every 1-2 seconds to obtain latest visual context. Parse the nested image string (§5).
5. **Send Robot Speech & Output**:
   - Send `{"session_id":"<id>", "type":"tts", "state":"start"}`.
   - Optionally send `{"session_id":"<id>", "type":"llm", "emotion":"happy"}` and `{"session_id":"<id>", "type":"tts", "state":"sentence_start", "text":"..."}`.
   - Stream raw 24 kHz Opus binary packets to the WebSocket, **paced at about real time** (the device buffers ~2.4 s and drops the overflow).
   - When the audio has played out, send `{"session_id":"<id>", "type":"tts", "state":"stop"}`.
   - **`tts stop` returns the device to whatever state `tts start` interrupted.** Inside a
     turn opened by a face tap that is `listening`, so the device reopens the user's turn
     with a fresh `listen start`. An utterance sent while the device was `idle` — using the
     robot as a speaker — returns to `idle`, and the microphone is never armed. Sending
     audio cannot put the device into `listening`; only a tap or a wake word does that.
   - **Announcements need no tap.** Right after the handshake the host may `tts start`,
     stream and `tts stop` to use the robot as a speaker. The microphone stays off unless
     the user starts a turn.
6. **Handle Interruptions**:
   - If `{"type":"abort"}` is received from device, immediately cease sending audio frames, cancel active LLM/TTS generation, and send `tts stop` — after a face tap the device keeps playing what it has queued until you do.
7. **Control Head & Hardware**:
   - Send MCP `tools/call` for `self.robot.set_head_angles`, `self.robot.set_led_color`, or `self.robot.play_sound` whenever the agent wants physical actuation.
