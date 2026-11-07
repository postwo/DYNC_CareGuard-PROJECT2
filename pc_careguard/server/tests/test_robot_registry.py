import asyncio

from fastapi import WebSocketDisconnect

from app.services.robot_registry import ROLE_DOWNLINK, ROLE_UPLINK, RobotRegistry


class FakeSocket:
    def __init__(self, error=None):
        self.sent = []
        self._error = error

    async def send_json(self, message):
        if self._error:
            raise self._error
        self.sent.append(message)


def run(coro):
    return asyncio.run(coro)


def test_uplink_does_not_count_as_online_or_receive_commands():
    reg = RobotRegistry()
    uplink = FakeSocket()
    reg.attach(ROLE_UPLINK, "R-1", uplink)
    assert reg.is_connected("R-1") is False
    assert run(reg.send_command("R-1", {"type": "x"})) is False
    assert uplink.sent == []


def test_send_command_reaches_downlink():
    reg = RobotRegistry()
    down = FakeSocket()
    reg.attach(ROLE_DOWNLINK, "R-1", down)
    assert run(reg.send_command("R-1", {"type": "go"})) is True
    assert down.sent == [{"type": "go"}]


def test_send_failure_from_disconnect_drops_the_connection():
    reg = RobotRegistry()
    reg.attach(ROLE_DOWNLINK, "R-1", FakeSocket(error=WebSocketDisconnect()))
    assert run(reg.send_command("R-1", {"type": "go"})) is False
    assert reg.is_connected("R-1") is False


def test_unexpected_send_error_is_not_swallowed():
    """연결 끊김이 아닌 예외(코드 버그)는 False로 뭉개지 않고 위로 올라와야 한다."""
    reg = RobotRegistry()
    reg.attach(ROLE_DOWNLINK, "R-1", FakeSocket(error=KeyError("bug")))
    try:
        run(reg.send_command("R-1", {"type": "go"}))
    except KeyError:
        return
    raise AssertionError("KeyError가 삼켜졌다")


def test_old_connection_close_does_not_evict_reconnected_socket():
    reg = RobotRegistry()
    old, new = FakeSocket(), FakeSocket()
    reg.attach(ROLE_DOWNLINK, "R-1", old)
    reg.attach(ROLE_DOWNLINK, "R-1", new)
    reg.detach(ROLE_DOWNLINK, "R-1", old)
    assert reg.is_connected("R-1") is True


def test_position_and_nav_state_are_remembered_and_reset():
    reg = RobotRegistry()
    reg.remember_position("R-1", {"x": 1.0})
    reg.set_nav_running("R-1", True)
    assert reg.positions() == {"R-1": {"x": 1.0}}
    assert reg.nav_running("R-1") is True
    reg.reset()
    assert reg.positions() == {} and reg.nav_running("R-1") is False
