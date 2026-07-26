#!/usr/bin/env python3
"""Standalone Gemini Live API interactive audio test binary simulating StackChan hardware at 16kHz."""

import argparse
import asyncio
import logging
import os
import signal
import sys
import pyaudio

from gateway.backends.gemini_api import (
    GeminiLiveSession,
    DEFAULT_MODEL,
    DEFAULT_VOICE,
    DEFAULT_SYSTEM_INSTRUCTION,
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("gemini_sim")

SAMPLE_RATE = 16000  # Strict 16kHz parity with ESP32 audio peripherals
CHANNELS = 1
FORMAT = pyaudio.paInt16
CHUNK_SIZE = 1024  # ~64ms acoustic frames


async def run_audio_simulation(args: argparse.Namespace) -> None:
    pya = pyaudio.PyAudio()

    # 1. Open 16kHz hardware playback audio stream
    out_stream = await asyncio.to_thread(
        pya.open,
        format=FORMAT,
        channels=CHANNELS,
        rate=SAMPLE_RATE,
        output=True,
    )

    # 2. Open 16kHz hardware capture microphone stream
    try:
        mic_info = pya.get_default_input_device_info()
        in_stream = await asyncio.to_thread(
            pya.open,
            format=FORMAT,
            channels=CHANNELS,
            rate=SAMPLE_RATE,
            input=True,
            input_device_index=mic_info["index"],
            frames_per_buffer=CHUNK_SIZE,
        )
        logger.info(f"🎙️ Connected to input device: {mic_info.get('name')}")
    except (IOError, OSError) as err:
        logger.error(f"❌ Failed to open microphone: {err}. Ensure audio drivers are operational.")
        out_stream.close()
        pya.terminate()
        return

    # 3. Define simulated hardware callbacks
    async def send_json_to_terminal(payload: dict) -> None:
        """Mimics authoritive firmware JSON handling by rendering state shifts to terminal."""
        msg_type = payload.get("type")
        msg_state = payload.get("state")
        if msg_type == "tts" and msg_state == "start":
            print("\n🤖 [FIRMWARE STATE: SPEAKING] 🔊 Audio playback active...", end="", flush=True)
        elif msg_type == "tts" and msg_state == "stop":
            print("\n👂 [FIRMWARE STATE: LISTENING] Awaiting microphone speech...", flush=True)
        elif msg_type == "tts" and msg_state == "sentence_start":
            text = payload.get("text", "")
            print(f"\n💬 {text}", end="", flush=True)

    async def send_audio_to_speakers(pcm_bytes: bytes) -> None:
        """Plays downsampled 16kHz audio directly through desktop speakers."""
        await asyncio.to_thread(out_stream.write, pcm_bytes)

    # 4. Instantiate and connect Gemini Live session
    session = GeminiLiveSession(
        api_key=args.api_key,
        send_json_to_firmware=send_json_to_terminal,
        send_audio_to_firmware=send_audio_to_speakers,
        on_monitor_emit=None,
        model=args.model,
        voice=args.voice,
        system_instruction=args.system_instruction,
    )

    try:
        await session.connect()
        print("\n👂 [FIRMWARE STATE: LISTENING] Speak into your microphone (Press Ctrl+C to abort)...", flush=True)

        stop_event = asyncio.Event()
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, stop_event.set)
            except (NotImplementedError, RuntimeError, ValueError):
                pass

        # 5. Master audio capture loop
        while not stop_event.is_set() and session.is_connected:
            pcm_chunk = await asyncio.to_thread(in_stream.read, CHUNK_SIZE, exception_on_overflow=False)
            if pcm_chunk:
                await session.forward_audio_chunk(pcm_chunk)
            await asyncio.sleep(0.001)

    except (asyncio.CancelledError, ConnectionError) as err:
        logger.info(f"🛑 Simulation terminated: {err}")
    finally:
        logger.info("Closing sound streams and releasing audio devices...")
        await session.disconnect()
        in_stream.stop_stream()
        in_stream.close()
        out_stream.stop_stream()
        out_stream.close()
        pya.terminate()
        logger.info("✓ Hardware simulation exited cleanly.")


def main() -> None:
    parser = argparse.ArgumentParser(description="StackChan Standalone 16kHz Hardware AI Simulator")
    parser.add_argument("--api-key", default=os.environ.get("GEMINI_API_KEY", ""), help="Google GenAI Cloud API key")
    parser.add_argument("--model", default=DEFAULT_MODEL, help=f"Target Gemini model (default: {DEFAULT_MODEL})")
    parser.add_argument("--voice", default=DEFAULT_VOICE, help=f"Target TTS voice (default: {DEFAULT_VOICE})")
    parser.add_argument("--system-instruction", default=DEFAULT_SYSTEM_INSTRUCTION, help="System personality prompt")
    args = parser.parse_args()

    if not args.api_key:
        logger.error("❌ --api-key flag or GEMINI_API_KEY environment variable required.")
        sys.exit(1)

    try:
        asyncio.run(run_audio_simulation(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
