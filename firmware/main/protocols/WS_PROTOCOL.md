# Device WebSocket & MCP Protocol Specification

This document provides the definitive, code-grounded specification for network communication with the StackChan firmware.

It covers the physical network topology, connection lifecycle, duplex Opus audio streaming, camera frame retrieval, JSON control messages, and the embedded Model Context Protocol (MCP) server.

---

## 1. Architecture & Networking Overview

The firmware implements an **inverted WebSocket server model**:
- **Device is the server (`WebsocketServerProtocol`)**: The robot listens for incoming TCP connections; the external host (PC, server, edge box, or LLM agent) connects as a client.
- **Physical interfaces (2 Network Devices)**:
  1. **USB CDC-NCM (Ethernet-over-USB)**: Implemented in `main/hal/board/usb_net_board.cc`. When plugged into a host via USB-C, the robot enumerates as an Ethernet adapter (`usb0` on Linux, `enX` on macOS). The robot runs a local DHCP server with fixed IP `192.168.7.1` (netmask `255.255.255.0`) and assigns `192.168.7.2` to the host. Router and DNS solicitations are zeroed out so host default routes are unaffected.
  2. **Wi-Fi Station (`StackChanWifiStation`)**: Implemented in `main/hal/board/network_link.cc` and `main/hal/utils/wifi_connect/wifi_station.cc`. When enabled (`CONFIG_STACKCHAN_WIFI_ENABLE=y`), the device associates with the local wireless network and receives a DHCP IP. If enabled (`CONFIG_STACKCHAN_TAILSCALE_ENABLE=y`), a Tailscale VPN tunnel (MicroLink) is also established.
- **Single Listening Daemon**:
  - The HTTP/WebSocket daemon (`WebsocketServerProtocol` in `main/protocols/websocket_server_protocol.cc`) binds `INADDR_ANY` (`0.0.0.0`) on port `CONFIG_USB_NET_LISTEN_PORT` (default: **`8081`**). Control port is `8082`.
  - The exact same server endpoints are reachable via USB (`192.168.7.1:8081`), local Wi-Fi (`<wifi_ip>:8081`), or Tailnet (`<tailnet_ip>:8081`).
- **Active Protocol Implementation**:
  - Under `CONFIG_CONNECTION_TYPE_USB_NCM=y` (the default), `main/main.cpp` launches directly into `xiaozhi` mode on boot, instantiating `WebsocketServerProtocol` (`xiaozhi-esp32/main/application.cc`).
  - Face taps, camera taps, audio pipelines, and MCP commands interact exclusively with this protocol instance.
  - *(Note: `hal_ws_avatar.cpp` contains an unrelated outbound client for remote avatar video calling in Mooncake mode; it is not active during USB-NCM operation).*

---

## 2. Server Endpoints & Connection Rules

The internal `esp_http_server` registers three endpoints on port `8081`:

| Endpoint | Method | Transport | Purpose |
|---|---|---|---|
| `/ws` | `GET` (Upgrade) | WebSocket | Full duplex bidirectional communication: JSON control messages, raw Opus audio, and MCP tool envelopes. |
| `/debug` | `GET` | Plain HTTP | Read-only JSON diagnostics (uptime, task stacks, memory, client fd, battery status, Wi-Fi IP). Safe to poll without disturbing the active WebSocket. |
| `/debug/reset` | `POST` | Plain HTTP | Recovery endpoint: forcibly disconnects any active WebSocket client and resets device state to `idle`. Does not reboot. |

### Single-Client Policy & Eviction
- Exactly **one active WebSocket client** is allowed at a time (`client_fd_`).
- If a new client connects while another connection is open, the device **evicts the previous connection**: it immediately closes the old socket (`httpd_sess_trigger_close`) and adopts the new one.
- Rationale: Device-originated traffic (microphone Opus streams, TTS state transitions, sensor events) cannot be multiplexed without explicit routing policy. Enforcing single ownership prevents race conditions.

### TCP Keepalive & Disconnect Detection
- `esp_http_server` keepalive is configured: `keep_alive_idle = 15s`, `keep_alive_interval = 5s`, `keep_alive_count = 3` (~35s to reap a dead socket if no TCP FIN/RST is received).
- Application-level WebSocket timeout (`IsTimeout()`) is explicitly disabled: an idle connected client (e.g. waiting for a user face tap) may stay connected indefinitely without sending keepalive pings.
- If Wi-Fi disconnects, `WebsocketServerProtocol::DropRemoteClient()` explicitly terminates non-USB clients (`HostPeerAddressV4() != 192.168.7.0/24`) to immediately clean up stale sockets.

---

## 3. WebSocket Handshake Sequence

Upon connecting to `ws://<device_ip>:8081/ws`:

```
   Host (Client)                             StackChan (Server)
        |                                             |
        | ------------- TCP Handshake ------------->  |
        | <------------ WebSocket Upgrade ----------  |
        |                                             | (starts 20ms timer)
        | <------------ Device Hello Frame ---------- |
        |                                             |
        | ------------ Host Hello Frame ------------> |
        |                                             | (sets session_id,
        |                                             |  unblocks audio channel)
        |                                             |
```

### Step 1: Device sends `hello` (within ~20ms of connection)
The device transmits a text frame with its capabilities, IDs, and default microphone audio configuration:
```json
{
  "type": "hello",
  "version": 1,
  "features": {
    "mcp": true
  },
  "transport": "websocket",
  "device_id": "AA:BB:CC:DD:EE:FF",
  "client_id": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 60
  }
}
```

### Step 2: Host replies with `hello`
The host **must** respond with a text frame specifying `"transport": "websocket"` and an arbitrary unique `"session_id"`:
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
- `"session_id"` (string, required): Used by both host and device in all subsequent JSON messages.
- `"audio_params"` (optional): Sets the sample rate (Hz) and frame duration (ms) for downlink audio (defaults: 24000 Hz, 60 ms).
- Once received, the device triggers `WS_SERVER_SERVER_HELLO_EVENT`, unblocking the audio pipeline for conversations.

---

## 4. Duplex Audio Streaming

Audio is transmitted via **Binary WebSocket Frames**.
- Frames carry **raw Opus packets** with **no envelope, no container headers (no Ogg/WAV), and no binary prefixes**.
- Maximum frame size accepted by server: **64 KB** (`kMaxFrameBytes`).

### Uplink: Device Microphone -> Host
1. **Trigger**: User taps the robot's face, taps the camera icon, a wake word is detected, or the state machine enters auto-listening mode after speaking.
2. **State**: The device enters `kDeviceStateListening` and transmits a `listen` start frame:
   ```json
   {"session_id":"my-session-uuid-1234","type":"listen","state":"start","mode":"auto"}
   ```
   *(If started via camera tap, `"video": true` is appended).*
3. **Audio Stream**: The microphone capture pipeline (`AudioProcessor` -> Opus encoder) produces **16 kHz, 1 channel (mono), 60 ms Opus frames** and sends each packet as a Binary WebSocket frame.
4. **VAD Signals**: While listening, the device analyzes voice activity and emits real-time VAD text frames:
   ```json
   {"session_id":"my-session-uuid-1234","type":"vad","state":"speech"}
   {"session_id":"my-session-uuid-1234","type":"vad","state":"silence"}
   ```
5. **Turn Completion**: When user stops talking (in `auto` mode) or tap occurs, device sends `{"session_id":"...","type":"listen","state":"stop"}`.

### Downlink: Host Speaker Audio (TTS) -> Device
1. **State Activation (Critical)**: Before streaming binary audio to the speaker, the host **must** put the device into `speaking` state by sending:
   ```json
   {"session_id":"my-session-uuid-1234","type":"tts","state":"start"}
   ```
   > **Important**: Any binary Opus frames received by the device while `device_state != kDeviceStateSpeaking` are **dropped immediately** on arrival!
2. **Text Bubble Display (Optional)**: The host can update the subtitle/chat bubble on the screen:
   ```json
   {"session_id":"my-session-uuid-1234","type":"tts","state":"sentence_start","text":"Hello, I am StackChan!"}
   ```
3. **Audio Stream**: Host transmits raw Opus binary frames matching the negotiated audio parameters (typically 24 kHz, 60 ms mono). The device decodes and plays them via the onboard I2S codec (`CoreS3AudioCodec`).
4. **Turn Completion**: When the host finishes sending audio:
   ```json
   {"session_id":"my-session-uuid-1234","type":"tts","state":"stop"}
   ```
   The device resets its decoder and transitions back to `listening` (in auto mode) or `idle` (in manual mode).

### User Barge-in / Interruption
If the user taps the face while the device is speaking (or a wake word triggers), the device immediately stops playback, clears the queue, and sends an `abort` frame:
```json
{"session_id":"my-session-uuid-1234","type":"abort"}
```
*(If triggered by wake word, `"reason": "wake_word_detected"` is included).*

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
4. The device captures a fresh frame (`CaptureFresh()` drops old queued V4L2 frames), encodes it to JPEG (quality 55 for streaming), blinks the on-screen camera indicator, and returns the image base64-encoded:
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
             "mimeType": "image/jpeg",
             "data": "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBD..."
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
  - Flushes 3 sensor frames for auto-exposure/auto-white-balance stabilization.
  - Displays preview on the robot's screen.
  - Automatically waits up to 1.5 seconds for head servo motion to settle before capturing, preventing motion blur.

---

## 6. JSON Control Messages Reference

All JSON frames carry `"session_id"` and `"type"`.

### Device -> Host Messages

| `type` | Parameters / Fields | Description |
|---|---|---|
| `hello` | `version`, `features`, `transport`, `device_id`, `client_id`, `audio_params` | Initial handshake greeting sent immediately on client connection. |
| `listen` | `state: "start"\|"stop"\|"detect"`<br>`mode: "auto"\|"manual"\|"realtime"`<br>`text: "<wake_word>"` *(on detect)*<br>`video: true` *(optional)* | Microphone state notification. `video: true` indicates conversation was started via camera button. |
| `vad` | `state: "speech"\|"silence"` | Real-time Voice Activity Detection events. Used by host to gate audio processing and vision streaming. |
| `abort` | `reason: "wake_word_detected"` *(optional)* | User interrupted device speech via face tap or wake word. Host should abort TTS generation. |
| `sensor` | `event: "head_pet"\|"shaken"` | Rate-limited (4s) physical interaction events. Only transmitted while in `listening` state. |
| `mcp` | `payload: { ... }` | Encapsulates JSON-RPC 2.0 responses from the embedded MCP tool server. |
| `goodbye` | — | Emitted when audio channel is cleanly closed by the device. |

### Host -> Device Messages

| `type` | Parameters / Fields | Description |
|---|---|---|
| `hello` | `transport: "websocket"`<br>`session_id: "<string>"`<br>`audio_params: { sample_rate, frame_duration }` | Required handshake reply to device `hello`. Establishes active session ID. |
| `tts` | `state: "start"\|"stop"\|"sentence_start"`<br>`text: "<subtitle>"` *(on sentence_start)* | Drives speech state: `start` sets device to `speaking` (enabling audio decode); `stop` returns to `listening`/`idle`; `sentence_start` displays subtitle text in avatar speech bubble. |
| `stt` | `text: "<user transcript>"` | Displays transcribed user speech in the chat message interface. |
| `llm` | `emotion: "<emotion_name>"` | Sets avatar facial expression. Supported: `neutral`, `happy`, `laughing`, `angry`, `sad`, `crying`, `sleepy`, `doubtful`. |
| `mcp` | `payload: { ... }` | Encapsulates JSON-RPC 2.0 requests (`initialize`, `tools/list`, `tools/call`) to the MCP server. |
| `system` | `command: "reboot"` | Executes system management commands (`reboot` triggers `esp_restart()`). |
| `alert` | `status: "<title>"`, `message: "<body>"`, `emotion: "<name>"` | Displays modal alert overlay with specified emotion. |

---

## 7. Model Context Protocol (MCP) Server

The robot embeds a compliant JSON-RPC 2.0 Model Context Protocol server inside the WebSocket protocol (`{"type": "mcp", "payload": { ... }}`).

### JSON-RPC Framing
- **Request envelope**:
  ```json
  {"session_id":"my-session-uuid-1234","type":"mcp","payload":{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}}
  ```
- **Response envelope**:
  ```json
  {"session_id":"my-session-uuid-1234","type":"mcp","payload":{"jsonrpc":"2.0","id":1,"result":{ ... }}}
  ```
- Errors are returned as standard JSON-RPC error objects:
  ```json
  {"session_id":"...","type":"mcp","payload":{"jsonrpc":"2.0","id":1,"error":{"message":"..."}}}
  ```

### Supported Methods
1. **`initialize`**:
   - Parameters: `capabilities: { vision: { url, token } }`
   - Response: `protocolVersion: "2024-11-05"`, server info, and tool capabilities.
2. **`tools/list`**:
   - Parameters: `cursor` (optional string for pagination), `withUserTools` (optional bool).
   - Response: Paginated list of available tools (`tools: [ ... ]`, `nextCursor`).
3. **`tools/call`**:
   - Parameters: `name: "<tool_name>"`, `arguments: { ... }`
   - Response: `content: [ { "type": "text"|"image", ... } ]`, `isError: false`. Tool executions are scheduled on the main thread via `Application::Schedule`.

---

## 8. Complete MCP Tool Catalog

### Robot Motion, Hardware & Sensory Tools (`main/hal/hal_mcp.cpp`)

#### `self.robot.get_head_angles`
- **Description**: Returns current head orientation angles in degrees.
- **Parameters**: None.
- **Returns**: `{"yaw": <int>, "pitch": <int>}` (e.g. `{"yaw": 0, "pitch": 0}`).

#### `self.robot.set_head_angles`
- **Description**: Moves robot head servos to desired orientation. Returns immediately without blocking audio.
- **Parameters**:
  - `yaw` (integer, default: `-9999` to leave unchanged): Range `-128` to `128`. **Negative values turn toward viewer's left (robot's right); positive values turn toward viewer's right (robot's left)**. Recommended interaction range: `-45` to `45`.
  - `pitch` (integer, default: `-9999` to leave unchanged): Range `0` to `90` (0 is level/down, 90 is looking straight up).
  - `speed` (integer, default: `150`, range `100` to `1000`): Servo motion speed.

#### `self.camera.capture`
- **Description**: Captures image from onboard camera and returns base64 JPEG in an image content block.
- **Parameters**:
  - `stream` (boolean, default `false`): `true` for silent, low-latency 55-quality stream frame; `false` for 80-quality shuttered photo with LCD preview and head-settle delay.
- **Returns**: Image content block `{"type":"image","mimeType":"image/jpeg","data":"<base64>"}`.

#### `self.robot.set_led_color`
- **Description**: Sets the color of the robot's internal RGB LED neon lights.
- **Parameters**:
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
- **Parameters**: `id` (integer, required).

#### `self.robot.end_conversation`
- **Description**: Programmatically closes the voice conversation session and returns robot to standby/idle.

---

### System & Display Tools (`xiaozhi-esp32/main/mcp_server.cc`)

#### `self.get_device_status`
- **Description**: Returns live device telemetry JSON (battery level/charging, speaker volume, screen brightness, network status, uptime).
- **Parameters**: None.

#### `self.audio_speaker.set_volume`
- **Description**: Sets speaker output volume.
- **Parameters**: `volume` (integer, 0 to 100).

#### `self.screen.set_brightness`
- **Description**: Adjusts LCD backlight brightness.
- **Parameters**: `brightness` (integer, 0 to 100).

#### `self.screen.set_theme`
- **Description**: Switches UI color theme.
- **Parameters**: `theme` (string: `"light"` or `"dark"`).

#### User-Only Tools (`audience: ["user"]`)
Included in `tools/list` only when `withUserTools: true`:
- `self.get_system_info`: Returns flash, chip model, and firmware version.
- `self.reboot`: Reboots device.
- `self.upgrade_firmware`: Triggers OTA firmware download from URL.
- `self.screen.get_info`: Returns screen resolution and color format.
- `self.screen.snapshot`: Takes screenshot of UI and uploads to URL.
- `self.screen.preview_image`: Displays remote image on screen.
- `self.assets.set_download_url`: Sets custom asset download URL.

---

## 9. Auxiliary HTTP Diagnostic Endpoints

### `GET /debug`
Returns real-time operational status as JSON. Safe to poll continuously:
```json
{
  "uptime_s": 1234,
  "version": "1.0.0",
  "device_state": "idle",
  "xiaozhi_ready": true,
  "client_fd": 54,
  "has_client": true,
  "audio_channel_open": false,
  "frames_rx": 412,
  "frames_tx": 280,
  "send_failures": 0,
  "last_send_err": "ESP_OK",
  "heap_free": 4194304,
  "heap_min": 3800000,
  "wifi_ip": "192.168.1.150",
  "tcpip_stack_free_min": 4096,
  "internal_free": 45000,
  "internal_largest_block": 22000,
  "battery_level": 95,
  "battery_charging": false,
  "battery_discharging": false,
  "tailnet": "registered ip=100.64.0.5 peers=2"
}
```
*Note: `internal_largest_block` is critical on ESP32-S3: FreeRTOS task stacks require contiguous internal SRAM.*

### `POST /debug/reset`
Forces an immediate cleanup when the robot is in a bad state:
- Drops the active WebSocket client socket (`close(client_fd)`).
- Sets device state machine to `idle`.
- Returns: `{"was":"listening","now":"idle"}`.

---

## 10. Step-by-Step Client Implementation Guide

If you are implementing a custom backend, agent, or gateway in Python, Node.js, Go, or Rust, follow this exact lifecycle:

1. **Connect**:
   - Connect WebSocket to `ws://192.168.7.1:8081/ws` (or Wi-Fi IP).
2. **Handle Handshake**:
   - Receive device `{"type":"hello", ...}`.
   - Reply immediately with host `{"type":"hello", "transport":"websocket", "session_id":"<id>", "audio_params":{"sample_rate":24000,"frame_duration":60}}`.
3. **Listen for User Speech**:
   - Wait for `{"type":"listen", "state":"start", ...}`.
   - Read incoming Binary WebSocket frames (each frame is one raw Opus packet @ 16 kHz mono). Feed into Opus decoder / STT engine.
   - Monitor `{"type":"vad", "state":"speech"|"silence"}`.
4. **Interleaved Camera Capture (Optional)**:
   - If `listen` contained `"video": true` and VAD is `"speech"`, invoke MCP `tools/call` with `self.camera.capture` (`{"stream": true}`) every 1-2 seconds to obtain latest visual context.
5. **Send Robot Speech & Output**:
   - Send `{"session_id":"<id>", "type":"tts", "state":"start"}`.
   - Optionally send `{"session_id":"<id>", "type":"llm", "emotion":"happy"}` and `{"session_id":"<id>", "type":"tts", "state":"sentence_start", "text":"..."}`.
   - Stream raw 24 kHz Opus binary packets to the WebSocket.
   - When finished, send `{"session_id":"<id>", "type":"tts", "state":"stop"}`.
   - **`tts stop` returns the device to whatever state `tts start` interrupted.** Inside a
     turn opened by a face tap that is `listening`, so the device reopens the user's turn
     with a fresh `listen start`. An utterance sent while the device was `idle` — using the
     robot as a speaker — returns to `idle`, and the microphone is never armed. Sending
     audio cannot put the device into `listening`; only a tap or a wake word does that.
6. **Handle Interruptions**:
   - If `{"type":"abort"}` is received from device, immediately cease sending audio frames and cancel active LLM/TTS generation.
7. **Control Head & Hardware**:
   - Send MCP `tools/call` for `self.robot.set_head_angles`, `self.robot.set_led_color`, or `self.robot.play_sound` whenever the agent wants physical actuation.
