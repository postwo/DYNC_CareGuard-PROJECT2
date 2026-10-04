import threading

import pytest

from careguard_turtlebot_agent import patrol_controller
from careguard_turtlebot_agent.errors import WaypointLookupError
from careguard_turtlebot_agent.patrol_controller import PatrolController


class Logger:
    def __init__(self):
        self.errors = []

    def info(self, msg): pass
    def warn(self, msg): pass
    def error(self, msg): self.errors.append(msg)


class FakeVoice:
    def speak(self, text): pass


class FakeNav2:
    def __init__(self, ready=True, navigate=lambda x, y, yaw: True):
        self.ready = ready
        self._navigate = navigate
        self.visited = []

    def ensure_running(self, map_id, x, y, yaw):
        return self.ready

    def go_to_pose_and_wait(self, x, y, yaw, cancel_requested):
        self.visited.append((x, y, yaw))
        return self._navigate(x, y, yaw)


@pytest.fixture
def sent():
    return []


def make(sent, nav2=None):
    controller = PatrolController(nav2 or FakeNav2(), FakeVoice(), sent.append, Logger())
    return controller


def finish(controller):
    controller._thread.join(timeout=5)
    assert not controller._thread.is_alive()


@pytest.fixture(autouse=True)
def charge_waypoint(monkeypatch):
    monkeypatch.setattr(patrol_controller, 'fetch_charge_waypoint', lambda map_id: (0.0, 0.0, 0.0))


def patrol(run_id=7, waypoints=None, map_id=1):
    return {'type': 'patrol_start', 'run_id': run_id, 'map_id': map_id,
            'waypoints': waypoints if waypoints is not None else [{'code': 'WP1', 'x': 1, 'y': 2, 'yaw': 0.5}]}


def test_successful_patrol_reports_progress_then_completed(sent):
    controller = make(sent)
    controller.start(patrol())
    finish(controller)
    assert sent == [
        {'type': 'patrol_progress', 'run_id': 7, 'waypoints_done': 1},
        {'type': 'patrol_done', 'run_id': 7, 'status': 'completed'},
    ]
    assert not controller.is_active


def test_unexpected_exception_while_driving_is_reported_as_failed(sent):
    """주행 도중 예외가 나도 서버가 알아야 한다 — 아니면 patrol_run이 'running'으로 고아가 된다."""
    def explode(x, y, yaw):
        raise RuntimeError('nav2 api broke')
    controller = make(sent, FakeNav2(navigate=explode))
    controller.start(patrol())
    finish(controller)
    assert sent == [{'type': 'patrol_done', 'run_id': 7, 'status': 'failed'}]
    assert not controller.is_active  # 다음 순찰을 받을 수 있어야 한다


def test_malformed_waypoint_is_rejected_before_driving(sent):
    nav2 = FakeNav2()
    controller = make(sent, nav2)
    controller.start(patrol(waypoints=[{'code': 'WP1', 'x': 'not-a-number', 'y': 2}]))
    assert sent == [{'type': 'patrol_done', 'run_id': 7, 'status': 'failed'}]
    assert nav2.visited == [] and not controller.is_active


def test_waypoint_missing_coordinates_is_rejected(sent):
    controller = make(sent)
    controller.start(patrol(waypoints=[{'code': 'WP1'}]))
    assert sent == [{'type': 'patrol_done', 'run_id': 7, 'status': 'failed'}]


def test_charge_lookup_failure_is_reported(sent, monkeypatch):
    def fail(map_id):
        raise WaypointLookupError('server down')
    monkeypatch.setattr(patrol_controller, 'fetch_charge_waypoint', fail)
    controller = make(sent)
    controller.start(patrol())
    assert sent == [{'type': 'patrol_done', 'run_id': 7, 'status': 'failed'}]


def test_nav2_not_ready_is_reported(sent):
    controller = make(sent, FakeNav2(ready=False))
    controller.start(patrol())
    assert sent == [{'type': 'patrol_done', 'run_id': 7, 'status': 'failed'}]


def test_request_while_busy_is_reported_so_server_run_is_not_orphaned(sent):
    started = threading.Event()
    release = threading.Event()

    def slow(x, y, yaw):
        started.set()
        release.wait(5)
        return True

    controller = make(sent, FakeNav2(navigate=slow))
    controller.start(patrol(run_id=1))
    assert started.wait(5)
    controller.start(patrol(run_id=2))  # 이미 주행 중
    assert {'type': 'patrol_done', 'run_id': 2, 'status': 'failed'} in sent
    release.set()
    finish(controller)


def test_run_id_zero_rejection_sends_nothing(sent):
    """run_id=0(배터리 복귀)은 서버에 patrol_run 행이 없어 알릴 대상이 없다."""
    controller = make(sent, FakeNav2(ready=False))
    controller.start({'type': 'patrol_start', 'run_id': 0, 'map_id': 1, 'waypoints': [], 'return_to_charging': True})
    assert sent == []


def test_cancel_reports_cancelled(sent):
    release = threading.Event()
    started = threading.Event()

    def slow(x, y, yaw):
        started.set()
        release.wait(5)
        return False

    controller = make(sent, FakeNav2(navigate=slow))
    controller.start(patrol())
    assert started.wait(5)
    controller.stop({})
    release.set()
    finish(controller)
    assert sent[-1] == {'type': 'patrol_done', 'run_id': 7, 'status': 'cancelled'}
