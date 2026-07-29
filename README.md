# wasm-chan — StackChan, offline and agent-driven

A fork of M5Stack's StackChan firmware. Three differences from upstream:

1. **No cloud.** The Chinese endpoints (`xiaozhi.me`, `api.tenclass.net`) are gone. The
   device works with no internet at all.
2. **USB is the network.** It enumerates as a CDC-NCM adapter and *listens* on
   `192.168.7.1:8081`; the host connects to it. Nothing to discover or configure.
3. **A live voice agent** — Gemini Live over that link, with the robot's head, face and
   camera as tools.

Upstream's README follows below.

---

## Quick start

```bash
# 1. Build and flash. Hold RST ~3s until the LED turns GREEN first (download mode),
#    then a SHORT press after flashing -- esptool's "hard reset" does nothing here.
cd firmware && source ../../esp-idf/export.sh && idf.py build
cd build && python -m esptool --chip esp32s3 -p /dev/cu.usbmodem111101 \
    -b 460800 --before default_reset --after hard_reset write_flash "@flash_args"

# 2. Is it alive?  (safe to poll at any time, even mid-conversation)
curl -s http://192.168.7.1:8081/debug | python3 -m json.tool

# 3. Talk to it
cp .env .env.local && $EDITOR .env.local        # add GEMINI_API_KEY
./wasm/.venv/bin/python wasm/clients/gemini_live.py
```

Then **tap its face** for a voice session, or the **camera button** (bottom-right) for
voice + 1 fps video. One client at a time — the device hangs up on the previous one.

**If anything misbehaves, read [firmware/DEBUGGING.md](firmware/DEBUGGING.md) first.** It
is the playbook: what each USB PID means, where the logs are, and a symptom-to-cause table
for the traps that each cost real flash cycles.

## What it can do

Eleven tools: move and read its head, change its face, take a photo, play a sound, set and
list and cancel reminders, report its own battery/volume/brightness/uptime, and adjust
volume and brightness. Petting and shaking are reported to the agent, so it can react to
being touched. Camera streaming is gated on the device's own voice detection, so a quiet
room costs nothing.

## Map

| | |
|---|---|
| [AGENT.md](../AGENT.md) | The constitution — read before changing anything |
| [firmware/DEBUGGING.md](firmware/DEBUGGING.md) | Hardware playbook: PIDs, flashing, log locations, symptom table |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the WASM sandbox and the USB transport work |
| [firmware/AVATAR.md](firmware/AVATAR.md) | How the face is drawn, and how to replace it |
| [TESTING.md](TESTING.md) | Test layout and the QA harness |
| [tools/facelab/](tools/facelab/) | Render the real avatar natively — iterate on the face without flashing |
| `wasm/clients/gemini_live.py` | The voice agent |
| `firmware/examples/` | `jingle.py`, `say.py`, `deliver_joke.py` — worked examples of driving the device |

---

# StackChan Open-Source

<img src="https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1205/K151_stack_chan_main_pictures_01.webp" width="60%">

Here are StackChan related open-source resources, including source code of the StackChan firmware, remote controller firmware, mobile app (iOS and Android), and server. 

Update of this repo could be a little late than the released firmware and mobile app. 

----

<img src="https://cdn.shopify.com/s/files/1/0056/7689/2250/files/5a589623895f65487717894d9240f6b8.png" width="60%">

**StackChan is a super kawaii AI desktop robot co-created by M5Stack and the user community.** It uses the M5Stack **flagship IoT development kit [CoreS3](https://docs.m5stack.com/en/core/CoreS3)** as its main controller, powered by an ESP32-S3 SoC featuring a 240 MHz dual-core processor, with 16MB Flash and 8MB PSRAM onboard, and supporting Wi-Fi and BLE. The main unit also integrates a 2.0-inch capacitive touch display with a high-strength glass cover, a 0.3 MP camera, a proximity & ambient light sensor, a 9-axis IMU (accelerometer + gyroscope + magnetometer), a microSD card slot, a 1W speaker, dual microphones, and power/reset buttons. 

The **robot body**, connected to the main unit, includes a USB-C interface for power and data, a 550 mAh battery, two feedback servos (360-degree continuous rotation on the horizontal axis and 90-degree movement on the vertical axis), two rows totaling 12 RGB LEDs, infrared transmitter and receiver, a three-zone touch panel, and a full-featured NFC module. 

The **factory firmware** is feature-rich, including an AI Agent, lively and expressive animations, ESP-NOW wireless remote control, and online app downloads. It can connect to a mobile app for video viewing, remote avatar control, and more, and also supports online updates (OTA). The product also supports programming via Arduino, UiFlow2, and other methods, and can connect to various expansion units in the M5Stack ecosystem, making it easy to implement a wide range of custom functions. 

> ⚠️ Do not forcibly rotate any movable parts connected to the motors by hand when you are unsure whether the motors are powered and under control, as this may cause hardware damage. 

- Purchase link: [M5Stack Official Store](https://shop.m5stack.com/products/stackchan-kawaii-co-created-open-source-ai-desktop-robot) | [淘宝 Taobao](https://item.taobao.com/item.htm?id=1042238294510)

- Product document page: [English](https://docs.m5stack.com/en/StackChan) | [日本語](https://docs.m5stack.com/ja/StackChan) | [中文](https://docs.m5stack.com/zh_CN/StackChan)

- Board support package: https://github.com/m5stack/StackChan-BSP

Thank you to the contributors of the StackChan community, especially: 

| ![](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1205/avatar_stack_chan.jpg) | ![](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1205/avatar_takao.jpg) |
| -------------------------------------------------------------------------------- | --------------------------------------------------------------------------- |
| [@stack_chan](https://x.com/stack_chan)                                          | [@mongonta555](https://x.com/mongonta555)                                   |
| Shinya Ishikawa                                                                  | Takao Akaki                                                                 |
