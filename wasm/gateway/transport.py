"""Transport ABC and RFC 1055 SLIP Framing Utility."""

from abc import ABC, abstractmethod
import logging
from typing import List, Optional, Union

logger = logging.getLogger("gateway.transport")

SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

FRAME_TYPE_JSON = 0x00
FRAME_TYPE_AUDIO = 0x01


class SlipFramer:
    """Zero-copy byte stream message boundary framer using RFC 1055 SLIP."""

    def __init__(self) -> None:
        self._buffer: bytearray = bytearray()
        self._in_escape: bool = False

    @staticmethod
    def encode(payload: Union[str, bytes]) -> bytes:
        frame = bytearray([SLIP_END])
        if isinstance(payload, str):
            data = bytes([FRAME_TYPE_JSON]) + payload.encode("utf-8")
        else:
            data = bytes([FRAME_TYPE_AUDIO]) + payload

        for byte in data:
            if byte == SLIP_END:
                frame.extend([SLIP_ESC, SLIP_ESC_END])
            elif byte == SLIP_ESC:
                frame.extend([SLIP_ESC, SLIP_ESC_ESC])
            else:
                frame.append(byte)
        frame.append(SLIP_END)
        return bytes(frame)

    def feed(self, chunk: bytes) -> List[Union[str, bytes]]:
        messages: List[Union[str, bytes]] = []
        for byte in chunk:
            if byte == SLIP_END:
                if self._buffer:
                    msg = self._decode_frame(self._buffer)
                    if msg is not None:
                        messages.append(msg)
                    self._buffer.clear()
                self._in_escape = False
            elif self._in_escape:
                if byte == SLIP_ESC_END:
                    self._buffer.append(SLIP_END)
                elif byte == SLIP_ESC_ESC:
                    self._buffer.append(SLIP_ESC)
                else:
                    logger.warning(
                        "[SLIP] ⚠️ Resync: corrupted escape byte 0x%02X, dropping frame",
                        byte,
                    )
                    self._buffer.clear()
                self._in_escape = False
            elif byte == SLIP_ESC:
                self._in_escape = True
            else:
                self._buffer.append(byte)
                if len(self._buffer) > 65536:
                    logger.warning("[SLIP] ⚠️ Resync: frame overflow (>64KB), reset")
                    self._buffer.clear()
                    self._in_escape = False
        return messages

    @staticmethod
    def _decode_frame(frame: bytearray) -> Optional[Union[str, bytes]]:
        if not frame:
            return None
        type_byte = frame[0]
        payload = bytes(frame[1:])
        if type_byte == FRAME_TYPE_JSON:
            try:
                return payload.decode("utf-8")
            except UnicodeDecodeError:
                logger.warning("[SLIP] ⚠️ UTF-8 decode error on JSON frame")
                return None
        elif type_byte == FRAME_TYPE_AUDIO:
            return payload
        else:
            logger.warning("[SLIP] ⚠️ Unknown frame type byte: 0x%02X", type_byte)
            return None


class Transport(ABC):
    """Bidirectional message channel. Text = JSON, Binary = Opus audio."""

    @abstractmethod
    async def recv(self) -> Union[str, bytes]:
        """Block until next message. Returns str for JSON, bytes for audio."""
        ...

    @abstractmethod
    async def send(self, data: Union[str, bytes]) -> None:
        """Send a message. str = JSON text, bytes = audio binary."""
        ...

    @abstractmethod
    async def connect(self) -> None:
        """Establish transport connection."""
        ...

    @abstractmethod
    async def disconnect(self) -> None:
        """Tear down transport connection."""
        ...

    @abstractmethod
    def is_connected(self) -> bool:
        ...

    @abstractmethod
    def get_transport_name(self) -> str:
        ...
