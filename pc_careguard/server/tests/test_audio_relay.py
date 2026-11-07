import asyncio

from fastapi import WebSocketDisconnect

from app.ws.audio_relay import CLOSE_SUPERSEDED, AudioRoom, AudioRoomRegistry


class FakeSocket:
    def __init__(self, error=None):
        self.frames = []
        self.closed_with = None
        self._error = error

    async def send_bytes(self, data):
        if self._error:
            raise self._error
        self.frames.append(data)

    async def close(self, code=1000):
        self.closed_with = code


def run(coro):
    return asyncio.run(coro)


def test_fanout_delivers_to_all_and_drops_dead_listener():
    room = AudioRoom()
    alive, dead = FakeSocket(), FakeSocket(error=WebSocketDisconnect())
    room.uplink_listeners.update({alive, dead})
    run(room.fanout_uplink(b"frame"))
    assert alive.frames == [b"frame"]
    assert room.uplink_listeners == {alive}


def test_relay_downlink_without_receiver_is_noop():
    run(AudioRoom().relay_downlink(b"x"))


def test_relay_downlink_clears_broken_receiver():
    room = AudioRoom()
    room.downlink_receiver = FakeSocket(error=RuntimeError("closed"))
    run(room.relay_downlink(b"x"))
    assert room.downlink_receiver is None


def test_unexpected_error_is_not_swallowed():
    room = AudioRoom()
    room.downlink_receiver = FakeSocket(error=KeyError("bug"))
    try:
        run(room.relay_downlink(b"x"))
    except KeyError:
        return
    raise AssertionError("KeyError가 삼켜졌다")


def test_new_robot_socket_closes_the_superseded_one():
    room = AudioRoom()
    old, new = FakeSocket(), FakeSocket()
    run(room.claim_uplink_sender(old))
    run(room.claim_uplink_sender(new))
    assert room.uplink_sender is new
    assert old.closed_with == CLOSE_SUPERSEDED
    assert new.closed_with is None


def test_registry_releases_only_empty_rooms():
    reg = AudioRoomRegistry()
    room = reg.room("R-1")
    room.uplink_listeners.add(FakeSocket())
    reg.release("R-1")
    assert "R-1" in reg._rooms
    room.uplink_listeners.clear()
    reg.release("R-1")
    assert "R-1" not in reg._rooms
