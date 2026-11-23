"""RoleChannel/RobotRelay 를 실제 WebSocket 서버(websockets.serve)에 붙여 시험한다."""
import asyncio
import json

import pytest
import websockets

from careguard_server_bridge import relay as relay_module
from careguard_server_bridge.relay import RobotRelay, is_json_object

relay_module.SUPERVISE_PERIOD_SEC = 0.02
relay_module.RETRY_DELAY_SEC = 0.05


class Clock:
    def __init__(self):
        self.now = 100.0

    def __call__(self):
        return self.now


class FakeServer:
    """FastAPI /ws/robot 흉내: 접속 경로를 기록하고 받은 메시지를 모은다."""

    def __init__(self, reject=False):
        self.paths = []
        self.received = []
        self.sockets = {}
        self.reject = reject

    async def handler(self, ws, path=None):
        path = path or ws.request.path
        self.paths.append(path)
        if self.reject:
            await ws.close(code=4404)
            return
        role = "downlink" if "role=downlink" in path else "uplink"
        self.sockets[role] = ws
        try:
            async for raw in ws:
                self.received.append((role, json.loads(raw)))
        finally:
            self.sockets.pop(role, None)


async def wait_for(predicate, timeout=2.0):
    deadline = asyncio.get_running_loop().time() + timeout
    while not predicate():
        if asyncio.get_running_loop().time() > deadline:
            raise AssertionError("timeout")
        await asyncio.sleep(0.01)


@pytest.fixture
def clock():
    return Clock()


def run(coro):
    asyncio.run(coro)


def start(server, clock, commands):
    async def _start():
        ws_server = await websockets.serve(server.handler, "127.0.0.1", 0)
        port = ws_server.sockets[0].getsockname()[1]
        relay = RobotRelay(f"ws://127.0.0.1:{port}", "R-05", commands.append,
                           heartbeat_timeout_sec=3.0, clock=clock)
        task = asyncio.ensure_future(relay.run())
        return ws_server, relay, task
    return _start()


def test_json_object_check():
    assert is_json_object('{"type": "position"}')
    assert not is_json_object("[1]") and not is_json_object("nope") and not is_json_object(None)


def test_connects_only_while_robot_heartbeat_alive(clock):
    async def scenario():
        server, commands = FakeServer(), []
        ws_server, relay, task = await start(server, clock, commands)
        await asyncio.sleep(0.1)
        assert server.paths == []  # 하트비트 전에는 서버에 붙지 않는다(꺼진 로봇이 온라인으로 보이면 안 됨)

        relay.note_heartbeat(json.dumps({"role": "downlink"}))
        relay.note_heartbeat(json.dumps({"role": "uplink"}))
        await wait_for(lambda: relay.server_state() == {"uplink": True, "downlink": True})
        assert sorted(server.paths) == ["/ws/robot?robot_code=R-05&role=downlink",
                                        "/ws/robot?robot_code=R-05&role=uplink"]

        # 로봇 → 서버
        relay.forward("uplink", json.dumps({"type": "position", "x": 1.0}))
        relay.forward("downlink", json.dumps({"type": "nav_status", "running": True}))
        relay.forward("uplink", "not json")
        await wait_for(lambda: len(server.received) == 2)
        assert ("uplink", {"type": "position", "x": 1.0}) in server.received
        assert relay.channels["uplink"].dropped == 1

        # 서버 → 로봇(downlink 로만)
        await server.sockets["downlink"].send(json.dumps({"type": "patrol_start", "waypoints": []}))
        await wait_for(lambda: commands)
        assert json.loads(commands[0])["type"] == "patrol_start"

        # 하트비트 끊김 → 서버 WS 닫힘(=서버가 robot_status offline 브로드캐스트)
        clock.now += 10.0
        await wait_for(lambda: relay.server_state() == {"uplink": False, "downlink": False})
        await wait_for(lambda: not server.sockets)

        task.cancel()
        ws_server.close()
        await ws_server.wait_closed()
    run(scenario())


def test_messages_dropped_while_disconnected(clock):
    async def scenario():
        server, commands = FakeServer(), []
        ws_server, relay, task = await start(server, clock, commands)
        relay.forward("uplink", json.dumps({"type": "position"}))
        await asyncio.sleep(0.05)
        assert server.received == [] and relay.channels["uplink"].dropped == 1
        task.cancel()
        ws_server.close()
        await ws_server.wait_closed()
    run(scenario())


def test_unknown_robot_backs_off(clock):
    async def scenario():
        server, commands = FakeServer(reject=True), []
        ws_server, relay, task = await start(server, clock, commands)
        relay.note_heartbeat(json.dumps({"role": "downlink"}))
        await wait_for(lambda: server.paths)
        await asyncio.sleep(0.1)
        # 일반 재시도 간격(0.05s)이 지나도 4404 이후에는 UNKNOWN_ROBOT_RETRY_SEC(30초) 동안 안 붙는다
        clock.now += 1.0
        relay.note_heartbeat(json.dumps({"role": "downlink"}))
        await asyncio.sleep(0.2)
        assert len(server.paths) == 1
        clock.now += 31.0
        relay.note_heartbeat(json.dumps({"role": "downlink"}))
        await wait_for(lambda: len(server.paths) == 2)
        task.cancel()
        ws_server.close()
        await ws_server.wait_closed()
    run(scenario())
