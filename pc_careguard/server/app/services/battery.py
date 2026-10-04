"""app/services/battery.py

배터리 처리 3단계를 각각 한 클래스가 맡는다.

  BatteryFilter      원본 % 떨림을 EMA + 표시 히스테리시스로 다듬는다(상태: 로봇별 필터값).
  BatteryAlertPolicy 임계값과 비교해 normal/attention/return_needed 상태를 판정한다(순수 로직, DB 없음).
  BatteryMonitor     위 둘을 묶고 DB(system_event)·알림 브로드캐스트를 담당한다.

배경(2026-09-12~14, R-07 LIMO 실측): 모터 부하에 따른 전압 처짐·회복으로 %가 82→84→82,
27↔31로 튄다. 필터 없이 경계만 비교하면 알림과 "충전소 복귀" 명령이 수십 초마다 반복됐다.
"""
import logging
import math
import time
from typing import Dict, Optional, Tuple

from ..core import db
from ..ws import dashboard_ws

logger = logging.getLogger(__name__)

STATE_NORMAL = "normal"
STATE_ATTENTION = "attention"
STATE_RETURN_NEEDED = "return_needed"

DEFAULT_ATTENTION_PCT = 30
DEFAULT_RETURN_PCT = 28


class BatteryFilter:
    """배터리 % 떨림 방지(aurix_medirun RosBatteryTelemetryProvider와 같은 2단계 방식).

    1. 경과 시간(dt) 기반 EMA — 고정 alpha가 아니라 dt로 계산해, 메시지 주기가 달라도
       "시간당 같은 정도"로 반응하게 한다.
    2. 표시 정수는 필터값이 (반올림 0.5 + 히스테리시스 0.75)%p 이상 벗어날 때만 바꾼다.
    """

    TAU_SEC = 5.0
    HYSTERESIS = 0.75

    def __init__(self) -> None:
        self._filtered: Dict[str, Tuple[float, float]] = {}  # code -> (EMA값, 계산 시각)
        self._displayed: Dict[str, int] = {}

    def smooth(self, robot_code: str, raw_percent: float, now: Optional[float] = None) -> int:
        now = time.monotonic() if now is None else now
        filtered, last_at = self._filtered.get(robot_code, (raw_percent, now))
        dt = max(0.0, now - last_at)
        alpha = 1.0 - math.exp(-dt / self.TAU_SEC)
        filtered += alpha * (raw_percent - filtered)
        self._filtered[robot_code] = (filtered, now)

        displayed = self._displayed.get(robot_code)
        if displayed is None or abs(filtered - displayed) >= 0.5 + self.HYSTERESIS:
            displayed = round(filtered)
        self._displayed[robot_code] = displayed
        return displayed

    def displayed(self) -> Dict[str, int]:
        return dict(self._displayed)

    def forget(self, robot_code: str) -> None:
        self._filtered.pop(robot_code, None)
        self._displayed.pop(robot_code, None)


class BatteryAlertPolicy:
    """임계값 판정. 나빠지는 방향(정상→주의→복귀)은 즉시 반영하고, 좋아지는 방향은
    임계값을 RECOVERY_MARGIN_PCT만큼 더 넘어야 인정한다(경계 근처 플래핑 방지)."""

    RECOVERY_MARGIN_PCT = 5

    def evaluate(self, previous: str, percentage: float, attention_pct: int, return_pct: int) -> str:
        if previous == STATE_RETURN_NEEDED and percentage <= return_pct + self.RECOVERY_MARGIN_PCT:
            return STATE_RETURN_NEEDED
        if percentage <= return_pct:
            return STATE_RETURN_NEEDED
        if previous == STATE_ATTENTION and percentage <= attention_pct + self.RECOVERY_MARGIN_PCT:
            return STATE_ATTENTION
        if percentage <= attention_pct:
            return STATE_ATTENTION
        return STATE_NORMAL


class BatteryMonitor:
    # 임계값 확인은 DB 조회라, 이 주기보다 자주 하면 이벤트 루프가 막혀 서버 전체가 멈춘다
    # (db.get_connection이 풀 없이 매번 새 TCP 연결을 연다 — 실제로 확인됨).
    CHECK_INTERVAL_SEC = 5.0

    def __init__(self, battery_filter: Optional[BatteryFilter] = None,
                 policy: Optional[BatteryAlertPolicy] = None) -> None:
        self.filter = battery_filter or BatteryFilter()
        self.policy = policy or BatteryAlertPolicy()
        self._alert_state: Dict[str, str] = {}
        self._last_check: Dict[str, float] = {}

    # ---- 호출부(비동기 라우터)가 쓰는 API ---------------------------------------

    def smooth(self, robot_code: str, raw_percent: float) -> int:
        return self.filter.smooth(robot_code, raw_percent)

    def due_for_threshold_check(self, robot_code: str, now: Optional[float] = None) -> bool:
        """CHECK_INTERVAL_SEC가 지났으면 True를 돌려주고 마지막 확인 시각을 갱신한다."""
        now = time.monotonic() if now is None else now
        if now - self._last_check.get(robot_code, 0.0) < self.CHECK_INTERVAL_SEC:
            return False
        self._last_check[robot_code] = now
        return True

    def alert_state(self, robot_code: str) -> str:
        return self._alert_state.get(robot_code, STATE_NORMAL)

    def latest(self) -> Dict[str, int]:
        return self.filter.displayed()

    def reset(self, robot_code: str) -> None:
        """테스트·로봇 등록 해제용."""
        self.filter.forget(robot_code)
        self._alert_state.pop(robot_code, None)
        self._last_check.pop(robot_code, None)

    # ---- DB를 쓰는 동기 API(asyncio.to_thread로 호출) ---------------------------

    def check_thresholds(self, robot_code: str, percentage: float) -> Optional[int]:
        """임계값을 넘는 "순간"에만 system_event를 한 번 남긴다.

        상태가 새로 return_needed가 된 순간에는 그 로봇의 활성 맵 id를 돌려준다 — 호출부가
        빈 웨이포인트 + return_to_charging=True 순찰 명령을 보내 충전소 복귀를 시작시킨다.
        활성 맵이 없으면 None(명령을 보내지 않는다).
        """
        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
            robot = cur.fetchone()
            if robot is None:
                return None
            robot_id = robot["id"]

            attention_pct, return_pct = self._load_thresholds(cur, robot_id)
            previous = self.alert_state(robot_code)
            current = self.policy.evaluate(previous, percentage, attention_pct, return_pct)
            self._alert_state[robot_code] = current
            if current == previous:
                return None

            # "정상 회복"은 로그를 남기지 않는다(2026-09-14 사용자 요청). 상태 추적은 위에서
            # 이미 갱신했으니 히스테리시스 판정에는 영향이 없다.
            if current == STATE_NORMAL:
                return None

            self._record_event(cur, robot_id, current, percentage, attention_pct, return_pct)
            conn.commit()
            # to_thread 스레드엔 이벤트 루프가 없어 broadcast_threadsafe를 쓴다.
            dashboard_ws.broadcast_threadsafe({"type": "system_event_created"})

            if current != STATE_RETURN_NEEDED:
                return None
            return self._active_map_id(cur, robot_id)

    @staticmethod
    def _load_thresholds(cur, robot_id: int) -> Tuple[int, int]:
        cur.execute(
            "SELECT battery_attention_pct, battery_return_pct FROM robot_threshold WHERE robot_id = %s",
            (robot_id,),
        )
        row = cur.fetchone()
        if row is None:
            return DEFAULT_ATTENTION_PCT, DEFAULT_RETURN_PCT
        return row["battery_attention_pct"], row["battery_return_pct"]

    @staticmethod
    def _record_event(cur, robot_id: int, state: str, percentage: float,
                      attention_pct: int, return_pct: int) -> None:
        if state == STATE_ATTENTION:
            event_type, severity = "battery_attention", "warn"
            message = f"배터리 {percentage:.0f}% — 주의 임계값({attention_pct}%) 이하"
        else:
            event_type, severity = "battery_return_needed", "error"
            message = f"배터리 {percentage:.0f}% — 복귀 임계값({return_pct}%) 이하, 충전 복귀 시작"
        cur.execute(
            "INSERT INTO system_event (robot_id, event_type, severity, message) VALUES (%s, %s, %s, %s)",
            (robot_id, event_type, severity, message),
        )

    @staticmethod
    def _active_map_id(cur, robot_id: int) -> Optional[int]:
        cur.execute(
            "SELECT id FROM map WHERE robot_id = %s AND is_active = TRUE ORDER BY created_at DESC LIMIT 1",
            (robot_id,),
        )
        row = cur.fetchone()
        return row["id"] if row else None


battery_monitor = BatteryMonitor()
