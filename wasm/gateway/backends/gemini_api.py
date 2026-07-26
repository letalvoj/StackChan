"""Gemini Live API authoritative real-time bidirectional streaming backend and gateway adapter."""

import asyncio
import json
import logging
import math
import struct
from typing import Callable, Optional

from google import genai
from google.genai import types, errors
import websockets.exceptions

from audio_dsp import downsample_24k_to_16k
from gateway.session import SessionState
from gateway.transport import Transport

logger = logging.getLogger("gateway.gemini")

DEFAULT_MODEL = "gemini-3.1-flash-live-preview"
DEFAULT_VOICE = "Puck"
DEFAULT_SYSTEM_INSTRUCTION = (
    "You are StackChan, a cute desk robot companion. "
    "Keep responses brief, cheerful, and conversational."
)


class GeminiLiveSession:
    """Manages a live bidirectional audio session using the official google-genai SDK."""

    def __init__(
        self,
        api_key: str,
        send_json_to_firmware: Callable,
        send_audio_to_firmware: Callable,
        on_monitor_emit: Optional[Callable] = None,
        model: str = DEFAULT_MODEL,
        voice: str = DEFAULT_VOICE,
        system_instruction: str = DEFAULT_SYSTEM_INSTRUCTION,
    ) -> None:
        self._client = genai.Client(api_key=api_key)
        self._send_json = send_json_to_firmware
        self._send_audio = send_audio_to_firmware
        self._on_monitor = on_monitor_emit
        self._model = model
        self._voice = voice
        self._system_instruction = system_instruction

        self._session: Optional[genai.types.AsyncSession] = None
        self._worker_task: Optional[asyncio.Task] = None
        self._connected_event = asyncio.Event()
        self._stop_requested = False
        self._is_speaking = False

    @property
    def is_connected(self) -> bool:
        return self._connected_event.is_set() and self._session is not None

    async def connect(self) -> None:
        """Launch the asynchronous SDK streaming loop and block until session is active."""
        self._stop_requested = False
        self._worker_task = asyncio.create_task(self._session_worker())
        try:
            await asyncio.wait_for(self._connected_event.wait(), timeout=10.0)
            logger.info(f"🤖 [GEMINI] Live stream active against model: {self._model}")
        except asyncio.TimeoutError:
            logger.error("🤖 [GEMINI] Connection handshake timed out.")
            await self.disconnect()
            raise ConnectionError("Gemini Live SDK handshake timed out")

    async def _session_worker(self) -> None:
        """Master coroutine: manages async context lifetime and receives streaming frames."""
        config = types.LiveConnectConfig(
            response_modalities=["AUDIO"],
            speech_config=types.SpeechConfig(
                voice_config=types.VoiceConfig(
                    prebuilt_voice_config=types.PrebuiltVoiceConfig(voice_name=self._voice)
                )
            ),
            system_instruction=types.Content(
                parts=[types.Part(text=self._system_instruction)]
            ),
            context_window_compression=types.ContextWindowCompressionConfig(
                trigger_tokens=25600,
                sliding_window=types.SlidingWindow(target_tokens=12800),
            ),
            input_audio_transcription=types.AudioTranscriptionConfig(),
            output_audio_transcription=types.AudioTranscriptionConfig(),
        )

        try:
            async with self._client.aio.live.connect(model=self._model, config=config) as session:
                self._session = session
                self._connected_event.set()

                while not self._stop_requested:
                    async for response in self._session.receive():
                        server_content = response.server_content
                        if server_content is None:
                            continue

                        # 1. Barge-In / Interruption Recovery Guarantee
                        if server_content.interrupted:
                            logger.info("🛑 [GEMINI:BARGE_IN] User interrupted model speech! Sending tts:stop.")
                            self._is_speaking = False
                            await self._send_json({"type": "tts", "state": "stop"})

                        # 2. Process High-Level Audio Shortcut (.data is raw PCM16 bytes @ 24kHz)
                        if response.data:
                            if not self._is_speaking:
                                self._is_speaking = True
                                logger.info("🔊 [GEMINI:AUDIO] Initial voice frame — sending tts:start.")
                                await self._send_json({"type": "llm", "emotion": "happy"})
                                await self._send_json({"type": "tts", "state": "start"})

                            pcm_16k = downsample_24k_to_16k(response.data)
                            if pcm_16k:
                                await self._send_audio(pcm_16k)
                                if self._on_monitor:
                                    num_samples = len(pcm_16k) // 2
                                    if num_samples > 0:
                                        samples = struct.unpack(f"<{num_samples}h", pcm_16k)
                                        rms_raw = math.sqrt(sum(s * s for s in samples) / num_samples)
                                        rms_normalized = min(1.0, rms_raw / 32768.0)
                                        await self._on_monitor({"type": "downlink_rms", "rms": rms_normalized})

                        # 3. Process High-Level Text Transcript Shortcut (.text)
                        if response.text:
                            logger.info(f"📝 [GEMINI:TEXT] {response.text[:100]}")
                            await self._send_json({
                                "type": "tts",
                                "state": "sentence_start",
                                "text": response.text,
                            })

                        # 4. Turn Completion End-pointing
                        if server_content.turn_complete:
                            if self._is_speaking:
                                self._is_speaking = False
                                await self._send_json({"type": "tts", "state": "stop"})
                                logger.info("✅ [GEMINI:TURN] Model turn complete.")

        except asyncio.CancelledError:
            pass
        except (
            ConnectionError,
            OSError,
            RuntimeError,
            ValueError,
            errors.APIError,
            errors.ClientError,
            errors.ServerError,
            websockets.exceptions.WebSocketException,
        ) as err:
            logger.error(f"🔌 [GEMINI:ERROR] Streaming loop terminated: {err}")
        finally:
            self._connected_event.clear()
            self._session = None
            self._is_speaking = False

    async def forward_audio_chunk(self, pcm_chunk: bytes) -> None:
        """Stream a raw 16kHz PCM audio chunk upstream to Gemini using SDK Blob structure."""
        if not self._session or not self.is_connected:
            return

        try:
            await self._session.send_realtime_input(
                audio=types.Blob(
                    data=pcm_chunk,
                    mime_type="audio/pcm;rate=16000",
                )
            )
        except (
            ConnectionError,
            OSError,
            RuntimeError,
            ValueError,
            errors.APIError,
            errors.ClientError,
            errors.ServerError,
            websockets.exceptions.WebSocketException,
        ) as err:
            logger.warning(f"🔌 [GEMINI] Failed transmitting audio chunk: {err}")

    async def abort_response(self) -> None:
        """Signal client-initiated abort or emergency cut-off."""
        self._is_speaking = False
        logger.info("🛑 [GEMINI] Turn manually aborted.")

    async def disconnect(self) -> None:
        """Tear down the streaming worker cleanly."""
        self._stop_requested = True
        self._connected_event.clear()
        if self._worker_task and not self._worker_task.done():
            self._worker_task.cancel()
            try:
                await self._worker_task
            except asyncio.CancelledError:
                pass
        self._worker_task = None
        self._session = None
        logger.info("🤖 [GEMINI] Session shutdown complete.")


class GeminiGatewayBackend:
    """Manages Gemini Live API bidirectional audio streaming over Gateway Transport."""

    def __init__(
        self,
        transport: Transport,
        session: SessionState,
        api_key: str,
        model: str = DEFAULT_MODEL,
        voice: str = DEFAULT_VOICE,
        system_instruction: str = DEFAULT_SYSTEM_INSTRUCTION,
    ) -> None:
        self._transport = transport
        self._session = session
        self._sid = session.session_id[:8]
        self._api_key = api_key
        self._model = model
        self._voice = voice
        self._system_instruction = system_instruction
        self._gemini: Optional[GeminiLiveSession] = None

    async def start(self) -> bool:
        """Connect to Gemini Live WebSocket and start streaming receive loop."""
        if not self._api_key:
            logger.error(
                "[GEMINI:%s...] Missing API key! Cannot connect to Gemini Live API.",
                self._sid,
            )
            return False

        async def send_json_to_firmware(payload: dict) -> None:
            if self._transport.is_connected():
                try:
                    await self._transport.send(json.dumps(payload))
                except (ConnectionError, OSError, BrokenPipeError):
                    pass

        async def send_audio_to_firmware(pcm_bytes: bytes) -> None:
            if self._transport.is_connected():
                try:
                    await self._transport.send(pcm_bytes)
                except (ConnectionError, OSError, BrokenPipeError):
                    pass

        self._gemini = GeminiLiveSession(
            api_key=self._api_key,
            send_json_to_firmware=send_json_to_firmware,
            send_audio_to_firmware=send_audio_to_firmware,
            on_monitor_emit=None,
            model=self._model,
            voice=self._voice,
            system_instruction=self._system_instruction,
        )
        try:
            await self._gemini.connect()
            logger.info(
                "[GEMINI:%s...] Live streaming backend connected (%s, voice: %s).",
                self._sid,
                self._model,
                self._voice,
            )
            return True
        except (
            OSError,
            ConnectionError,
            RuntimeError,
            ValueError,
            errors.APIError,
            errors.ClientError,
            errors.ServerError,
        ) as err:
            logger.error(
                "[GEMINI:%s...] Connection failure to Gemini API: %s", self._sid, err
            )
            self._gemini = None
            return False

    async def forward_audio_chunk(self, chunk: bytes) -> None:
        """Stream an audio frame upstream to Gemini Live."""
        if self._gemini and self._gemini.is_connected:
            await self._gemini.forward_audio_chunk(chunk)

    async def handle_speech_end(self, received_chunks: list[bytes]) -> None:
        """Called when local VAD detects end of speech."""
        logger.info(
            "[GEMINI:%s...] Local VAD speech end detected (%d chunks forwarded). "
            "Awaiting model streaming audio.",
            self._sid,
            len(received_chunks),
        )

    async def handle_abort(self) -> None:
        """Handle client interruption or turn abort."""
        if self._gemini:
            await self._gemini.abort_response()
            logger.info("[GEMINI:%s...] Response aborted by client.", self._sid)

    async def stop(self) -> None:
        """Cleanly disconnect Gemini WebSocket session."""
        if self._gemini:
            await self._gemini.disconnect()
            self._gemini = None
            logger.info("[GEMINI:%s...] Backend session shut down.", self._sid)
