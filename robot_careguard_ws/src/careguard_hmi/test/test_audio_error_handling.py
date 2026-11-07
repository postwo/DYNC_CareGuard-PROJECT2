import subprocess

import pytest

from careguard_hmi import audio_capture_node, audio_downlink_player


class Logger:
    def __init__(self):
        self.messages = []

    def info(self, msg): self.messages.append(('info', msg))
    def warn(self, msg): self.messages.append(('warn', msg))
    def error(self, msg): self.messages.append(('error', msg))

    def levels(self):
        return [level for level, _ in self.messages]


class FakeOpus:
    class OpusError(Exception):
        pass

    class Decoder:
        def __init__(self, *args): pass
        def decode(self, data, samples):
            if data == b'bad':
                raise FakeOpus.OpusError('corrupted')
            return b'\x01' * (samples * 2)


@pytest.fixture
def fake_opus(monkeypatch):
    monkeypatch.setattr(audio_downlink_player, 'opuslib', FakeOpus)
    monkeypatch.setattr(audio_downlink_player, 'OPUS_ERRORS', (FakeOpus.OpusError, ValueError))
    return FakeOpus


# ---- 재생(다운링크) -----------------------------------------------------------------

def test_playback_start_fails_cleanly_when_player_binary_is_missing(monkeypatch, fake_opus):
    """paplay/aplay가 없으면 예외가 ROS 콜백까지 새지 않고 False를 돌려줘야 한다 —
    호출부가 소리 낼 곳이 없는데 오디오를 받아 오지 않도록."""
    def no_such_binary(*args, **kwargs):
        raise FileNotFoundError('paplay')
    monkeypatch.setattr(audio_downlink_player.subprocess, 'Popen', no_such_binary)

    logger = Logger()
    session = audio_downlink_player.CallPlaybackSession(logger)
    assert session.start() is False
    assert not session.is_running
    assert 'error' in logger.levels()


def test_playback_start_returns_false_without_opus(monkeypatch):
    monkeypatch.setattr(audio_downlink_player, 'opuslib', None)
    logger = Logger()
    assert audio_downlink_player.CallPlaybackSession(logger).start() is False
    assert 'error' in logger.levels()


def test_corrupt_frame_is_skipped_not_fatal(fake_opus):
    logger = Logger()
    session = audio_downlink_player.CallPlaybackSession(logger)
    session._decoder = fake_opus.Decoder()

    session.on_frame_received(b'bad')
    assert 'warn' in logger.levels()
    assert not session._buffer.ready(1)  # 깨진 프레임은 버퍼에 안 들어간다

    session.on_frame_received(b'good')
    assert session._buffer.ready(1)


def test_unexpected_decoder_bug_is_not_swallowed(fake_opus):
    """Opus 오류가 아닌 예외(코드 버그)는 "손상된 프레임"으로 뭉개지 않고 드러나야 한다."""
    class BuggyDecoder:
        def decode(self, data, samples):
            raise KeyError('bug')

    session = audio_downlink_player.CallPlaybackSession(Logger())
    session._decoder = BuggyDecoder()
    with pytest.raises(KeyError):
        session.on_frame_received(b'x')


def test_terminate_player_falls_back_to_kill_on_timeout():
    class StuckProcess:
        stdin = None
        killed = False
        def terminate(self): pass
        def wait(self, timeout=None): raise subprocess.TimeoutExpired('player', timeout)
        def kill(self): self.killed = True

    session = audio_downlink_player.CallPlaybackSession(Logger())
    process = StuckProcess()
    session._process = process
    session._terminate_player()
    assert process.killed and session._process is None


# ---- 캡처(업링크) -------------------------------------------------------------------

def test_spawn_arecord_reports_missing_binary_instead_of_raising(monkeypatch):
    def no_such_binary(*args, **kwargs):
        raise FileNotFoundError('arecord')
    monkeypatch.setattr(audio_capture_node.subprocess, 'Popen', no_such_binary)

    logger = Logger()
    session = audio_capture_node.MicCaptureSession(logger)
    assert session._spawn_arecord() is False
    assert 'error' in logger.levels()


def test_terminate_arecord_falls_back_to_kill_on_timeout():
    class StuckProcess:
        killed = False
        def terminate(self): pass
        def wait(self, timeout=None): raise subprocess.TimeoutExpired('arecord', timeout)
        def kill(self): self.killed = True

    session = audio_capture_node.MicCaptureSession(Logger())
    process = StuckProcess()
    session._process = process
    session._terminate_arecord()
    assert process.killed and session._process is None
