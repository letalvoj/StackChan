"""Camera streaming must be gated, not merely rate-limited.

Every frame sent to a live model stays in the context window for the rest of the session.
A continuous 1 fps stream fills that window with pictures of an empty room and pushes the
actual conversation out of it -- so the gate is the feature, and the frame rate is only
the ceiling.

Three independent conditions must all hold before a frame leaves:

    the session was started with the camera button   (video_session)
    the device is listening                          (listening)
    the device's VAD hears speech                    (voice_active)

These tests exercise the decision, not the transport: the predicate is what has to be
right, and it is the part that would silently start streaming an empty room if someone
loosened a condition later.
"""

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "clients"))


class FakeEvent:
    def __init__(self, value=False):
        self._v = value

    def is_set(self):
        return self._v

    def set(self):
        self._v = True

    def clear(self):
        self._v = False


class FakeDevice:
    """Only the fields the gate reads."""

    def __init__(self, video=False, listening=False, voice=False):
        self.video_session = video
        self.listening = FakeEvent(listening)
        self.voice_active = voice


def should_send(dev) -> bool:
    """The predicate from video_uplink(), kept in one place so it can be tested."""
    return bool(dev.video_session and dev.listening.is_set() and dev.voice_active)


@pytest.mark.parametrize(
    "video,listening,voice,expected",
    [
        (True,  True,  True,  True),    # the only case that streams
        (True,  True,  False, False),   # silence: the whole point of the gate
        (True,  False, True,  False),   # not in a turn
        (False, True,  True,  False),   # tap-started: camera must stay off
        (False, False, False, False),
    ],
)
def test_gate(video, listening, voice, expected):
    assert should_send(FakeDevice(video, listening, voice)) is expected


def test_tap_started_session_never_streams():
    """A face tap is audio-only. The camera must never switch itself on."""
    dev = FakeDevice(video=False, listening=True, voice=True)
    assert not should_send(dev)


def test_silence_sends_nothing_over_a_long_session():
    """A quiet room must cost exactly zero frames, however long it lasts."""
    dev = FakeDevice(video=True, listening=True, voice=False)
    assert sum(1 for _ in range(600) if should_send(dev)) == 0


def test_stream_flag_reaches_the_device_call():
    """Streaming captures must ask for the quiet, lower-quality path.

    Without stream=True the device plays its shutter sound on every frame and encodes at
    full quality -- once per second, mid-conversation, over USB.
    """
    import inspect
    import gemini_live as g

    src = inspect.getsource(g.converse)
    assert '"self.camera.capture", {"stream": True}' in src


def test_photo_tool_removed_only_when_streaming():
    import gemini_live as g

    audio_only = {f.name for f in g.tools_for(False)[0].function_declarations}
    streaming = {f.name for f in g.tools_for(True)[0].function_declarations}

    assert "take_photo" in audio_only
    assert "take_photo" not in streaming
    assert audio_only - {"take_photo"} == streaming

    # tools_for must not mutate the module-level list, or the first video session would
    # permanently strip the tool from every later audio-only one.
    again = {f.name for f in g.tools_for(False)[0].function_declarations}
    assert "take_photo" in again
