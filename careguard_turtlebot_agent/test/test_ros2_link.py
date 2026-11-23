"""Ros2Link(로봇 쪽 ROS 2 브리지 링크) 시험 — 가짜 Node 로 토픽 이름·하트비트·연결 판정을 본다."""
import json
import sys
import time
import types

import pytest

if 'std_msgs.msg' not in sys.modules:
    try:
        import std_msgs.msg  # noqa: F401
    except ImportError:
        class _String:
            def __init__(self, data=''):
                self.data = data
        sys.modules['std_msgs'] = types.ModuleType('std_msgs')
        sys.modules['std_msgs.msg'] = types.ModuleType('std_msgs.msg')
        sys.modules['std_msgs.msg'].String = _String

from careguard_turtlebot_agent import ros2_link  # noqa: E402
from careguard_turtlebot_agent.ros2_link import Ros2Link, robot_namespace  # noqa: E402


class Pub:
    def __init__(self, topic, sink):
        self.topic, self.sink = topic, sink

    def publish(self, msg):
        self.sink.append((self.topic, msg.data))


class FakeNode:
    def __init__(self):
        self.published, self.subs, self.timers = [], {}, []

    def create_publisher(self, _type, topic, _qos):
        return Pub(topic, self.published)

    def create_subscription(self, _type, topic, cb, _qos):
        self.subs[topic] = cb

    def create_timer(self, period, cb):
        self.timers.append(cb)

    def deliver(self, topic, data):
        self.subs[topic](types.SimpleNamespace(data=data))


class Logger:
    def __init__(self):
        self.lines = []

    def info(self, m): self.lines.append(('info', m))
    def warn(self, m): self.lines.append(('warn', m))
    def error(self, m): self.lines.append(('error', m))


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


@pytest.fixture
def clock():
    return Clock()


def make(role, clock, on_message=lambda raw: None):
    node = FakeNode()
    link = Ros2Link(node, 'R-05', role, on_message, Logger(), clock=clock)
    return node, link


def test_namespace():
    assert robot_namespace('R-05') == '/careguard/r_05'


def test_topics_by_role(clock):
    node, _ = make('uplink', clock)
    assert set(node.subs) == {'/careguard/r_05/server_heartbeat'}
    node, _ = make('downlink', clock)
    assert set(node.subs) == {'/careguard/r_05/server_heartbeat', '/careguard/r_05/to_robot'}


def test_send_only_when_bridge_reports_role_connected(clock):
    node, link = make('uplink', clock)
    link.send({'type': 'position'})
    assert not [p for p in node.published if p[0].endswith('/to_server/uplink')]

    node.deliver('/careguard/r_05/server_heartbeat', json.dumps({'uplink': True, 'downlink': False}))
    assert link.is_connected
    link.send({'type': 'position', 'x': 1})
    assert ('/careguard/r_05/to_server/uplink', json.dumps({'type': 'position', 'x': 1})) in node.published

    clock.now += ros2_link.SERVER_HEARTBEAT_TIMEOUT_SEC + 0.1  # 브리지 응답 끊김
    assert not link.is_connected


def test_role_specific_connection_state(clock):
    node, link = make('downlink', clock)
    node.deliver('/careguard/r_05/server_heartbeat', json.dumps({'uplink': True, 'downlink': False}))
    assert not link.is_connected


def test_heartbeat_published_with_role(clock):
    node, link = make('downlink', clock)
    node.timers[0]()
    topic, data = node.published[-1]
    assert topic == '/careguard/r_05/link_heartbeat'
    assert json.loads(data) == {'role': 'downlink', 'robot_code': 'R-05'}


def test_commands_run_on_worker_thread_and_errors_isolated(clock):
    received = []

    def handler(raw):
        if raw == 'boom':
            raise RuntimeError('x')
        received.append(raw)

    node, link = make('downlink', clock, handler)
    link.start()
    node.deliver('/careguard/r_05/to_robot', 'boom')
    node.deliver('/careguard/r_05/to_robot', '{"type": "patrol_start"}')
    deadline = time.time() + 2
    while not received and time.time() < deadline:
        time.sleep(0.01)
    assert received == ['{"type": "patrol_start"}']


def test_invalid_role_rejected(clock):
    with pytest.raises(ValueError):
        make('sideways', clock)


def test_link_factory_selects_transport(monkeypatch, clock):
    from careguard_turtlebot_agent import link_factory, ws_config

    monkeypatch.setattr(ws_config, 'ROBOT_CODE', 'R-05')
    monkeypatch.setattr(ws_config, 'LINK_MODE', 'ros2')
    link = link_factory.create_server_link(FakeNode(), 'uplink', lambda raw: None, Logger(), '업링크')
    assert isinstance(link, Ros2Link)

    monkeypatch.setattr(ws_config, 'LINK_MODE', 'ws')
    fake_server_link = types.ModuleType('careguard_turtlebot_agent.server_link')

    class FakeServerLink:
        def __init__(self, **kwargs):
            self.kwargs = kwargs
    fake_server_link.ServerLink = FakeServerLink
    fake_server_link.RECONNECT_DELAY_SEC = 3.0
    monkeypatch.setitem(sys.modules, 'careguard_turtlebot_agent.server_link', fake_server_link)
    link = link_factory.create_server_link(FakeNode(), 'downlink', lambda raw: None, Logger(), '다운링크')
    assert link.kwargs['url'].endswith('/ws/robot?robot_code=R-05&role=downlink')
    assert link.kwargs['reconnect_delay_sec'] == 3.0
