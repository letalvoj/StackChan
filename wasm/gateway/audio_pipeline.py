"""Audio pipeline: chunk accumulation, max_duration cap, and speech lifecycle."""

import logging
import time
from dataclasses import dataclass, field
from typing import Awaitable, Callable

from gateway.session import SessionState
from gateway.transport import Transport

logger = logging.getLogger("gateway")


@dataclass
class AudioPipelineState:
    """Tracks audio accumulation state for a single session."""

    chunks: list[bytes] = field(default_factory=list)
    speech_active: bool = False
    phase: str = "idle"  # idle | accumulating | responding
    max_duration: int = 45  # seconds
    last_audio_time: float = field(default_factory=time.monotonic)


class AudioPipeline:
    """Accumulates audio chunks and triggers response when speech ends."""

    def __init__(
        self,
        transport: Transport,
        session: SessionState,
        on_utterance_complete: Callable[[list[bytes]], Awaitable[None]],
    ) -> None:
        self._transport = transport
        self._session = session
        self._on_utterance_complete = on_utterance_complete
        self._state = AudioPipelineState()
        self._sid = session.session_id[:8]

    @property
    def state(self) -> AudioPipelineState:
        """Access the current pipeline state."""
        return self._state

    async def handle_audio_chunk(self, chunk: bytes) -> None:
        """Process an incoming audio chunk."""
        if self._state.phase == "responding":
            logger.debug(
                "[AUDIO:%s...] Dropping audio chunk during response phase",
                self._sid,
            )
            return

        self._state.chunks.append(chunk)
        self._state.last_audio_time = time.monotonic()

        if not self._state.speech_active:
            self._state.speech_active = True
            self._state.phase = "accumulating"
            logger.info(
                "[AUDIO:%s...] First audio frame received. Accumulating.", self._sid
            )

        # Check max recording duration (at frame_duration ms per chunk)
        frame_seconds = self._session.frame_duration / 1000.0
        accumulated_seconds = len(self._state.chunks) * frame_seconds
        if (
            self._state.max_duration > 0
            and accumulated_seconds >= self._state.max_duration
        ):
            logger.info(
                "[AUDIO:%s...] Max duration cap (%ds) reached. "
                "Triggering response with %d chunks.",
                self._sid,
                self._state.max_duration,
                len(self._state.chunks),
            )
            await self._trigger_response("max_duration_cap")

    async def handle_speech_end(self, reason: str) -> None:
        """Trigger response processing when speech ends."""
        if self._state.chunks:
            await self._trigger_response(reason)
        else:
            logger.info(
                "[AUDIO:%s...] Speech end (%s) but no audio accumulated. Ignoring.",
                self._sid,
                reason,
            )
            self._state.phase = "idle"
            self._state.speech_active = False

    def reset(self) -> None:
        """Reset accumulation state for a new turn."""
        self._state.chunks.clear()
        self._state.speech_active = False
        self._state.phase = "idle"

    async def _trigger_response(self, reason: str) -> None:
        """Collect accumulated chunks and invoke the response callback."""
        chunks_snapshot = list(self._state.chunks)
        self._state.chunks.clear()
        self._state.speech_active = False
        self._state.phase = "responding"
        logger.info(
            "[AUDIO:%s...] Speech end via %s. "
            "Triggering response with %d chunks.",
            self._sid,
            reason,
            len(chunks_snapshot),
        )
        await self._on_utterance_complete(chunks_snapshot)
        if self._state.phase == "responding":
            self._state.phase = "idle"
