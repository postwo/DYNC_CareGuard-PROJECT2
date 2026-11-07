"""BatteryFilter / BatteryAlertPolicy 순수 로직 테스트 — DB·WS 없이 돈다."""
import pytest

from app.services.battery import (
    STATE_ATTENTION, STATE_NORMAL, STATE_RETURN_NEEDED, BatteryAlertPolicy, BatteryFilter, BatteryMonitor,
)


@pytest.fixture
def policy():
    return BatteryAlertPolicy()


@pytest.mark.parametrize("previous, pct, expected", [
    (STATE_NORMAL, 80, STATE_NORMAL),
    (STATE_NORMAL, 30, STATE_ATTENTION),
    (STATE_NORMAL, 28, STATE_RETURN_NEEDED),
    (STATE_NORMAL, 10, STATE_RETURN_NEEDED),
    # 나빠지는 방향은 여유분 없이 즉시 반영
    (STATE_ATTENTION, 28, STATE_RETURN_NEEDED),
    # 좋아지는 방향은 임계값 + 5%p를 넘어야 인정
    (STATE_RETURN_NEEDED, 31, STATE_RETURN_NEEDED),
    (STATE_RETURN_NEEDED, 33, STATE_RETURN_NEEDED),
    (STATE_RETURN_NEEDED, 34, STATE_NORMAL),
    (STATE_ATTENTION, 34, STATE_ATTENTION),
    (STATE_ATTENTION, 36, STATE_NORMAL),
])
def test_policy_transitions(policy, previous, pct, expected):
    assert policy.evaluate(previous, pct, attention_pct=30, return_pct=28) == expected


def test_filter_first_sample_is_displayed_as_is():
    f = BatteryFilter()
    assert f.smooth("R", 83.4, now=100.0) == 83


def test_filter_ignores_small_wobble_within_hysteresis():
    f = BatteryFilter()
    f.smooth("R", 83.0, now=0.0)
    # 1초 간격으로 82~84를 오가도 표시값은 안 흔들린다
    shown = {f.smooth("R", raw, now=float(t)) for t, raw in enumerate([84, 82, 84, 82, 84], start=1)}
    assert shown == {83}


def test_filter_follows_sustained_change():
    f = BatteryFilter()
    f.smooth("R", 80.0, now=0.0)
    shown = f.smooth("R", 60.0, now=120.0)  # 충분한 시간이 지나면 새 값을 따라간다
    assert shown == 60


def test_filter_is_per_robot():
    f = BatteryFilter()
    f.smooth("A", 90.0, now=0.0)
    assert f.smooth("B", 40.0, now=0.0) == 40
    assert f.displayed() == {"A": 90, "B": 40}


def test_monitor_threshold_check_is_throttled():
    m = BatteryMonitor()
    assert m.due_for_threshold_check("R", now=100.0) is True
    assert m.due_for_threshold_check("R", now=102.0) is False
    assert m.due_for_threshold_check("R", now=105.1) is True


def test_monitor_reset_forgets_everything():
    m = BatteryMonitor()
    m.smooth("R", 50.0)
    m.due_for_threshold_check("R", now=10.0)
    m.reset("R")
    assert m.latest() == {}
    assert m.alert_state("R") == STATE_NORMAL
    assert m.due_for_threshold_check("R", now=10.5) is True
