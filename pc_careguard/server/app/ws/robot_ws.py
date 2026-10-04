"""app/ws/robot_ws.py

로봇 ↔ 서버 WebSocket 엔드포인트와 메시지 라우팅.

구조
  RobotMessageRouter   수신 메시지 type -> 핸들러 메서드 매핑(디스패치 테이블). 새 메시지는
                       핸들러 메서드 하나와 테이블 한 줄만 추가하면 된다.
  services/*           실제 도메인 로직(배터리·복약·침입자·SLAM·순찰 재개). 이 파일은 "받아서
                       알맞은 서비스에 넘기고 대시보드에 알리는" 역할만 한다.
  RobotRegistry        연결·마지막 상태 저장소(services/robot_registry.py).

다른 모듈(patrol.py, call.py, robots.py, dashboard_ws.py)이 쓰는 함수형 API
(send_command, is_connected 등)는 아래에 그대로 유지한다.
"""
import asyncio
import logging
from typing import Callable, Dict

from fastapi import APIRouter, Query, WebSocket, WebSocketDisconnect

from . import dashboard_ws
from ..api.v1.event import EVENT_TYPE_LABELS
from ..services.battery import battery_monitor
from ..services.intruder import IntruderReporter, RecognitionCapture
from ..services.medication_log import MedicationAlert, MedicationRecorder
from ..services.patrol_resume import PatrolResumer, PatrolRunStore
from ..services.robot_registry import registry, robot_exists
from ..services.slam_result import SlamResultSaver

logger = logging.getLogger(__name__)

router = APIRouter()

# 단순 중계 메시지: 수신 type -> (대시보드로 보낼 type, 옮길 필드들). DB에 남기지 않는다 —
# 값이 계속 바뀌는 실시간 미리보기라 저장할 이유가 없다.
_RELAY_MESSAGES: Dict[str, tuple] = {
    "slam_progress": ("slam_progress", ("area_m2", "elapsed_sec")),
    "slam_map_update": (
        "slam_map_update",
        ("width", "height", "resolution", "origin_x", "origin_y", "pgm_base64"),
    ),
    "slam_scan_update": ("slam_scan_update", ("points",)),
    "patrol_path_update": ("patrol_path_update", ("points",)),
    "camera_frame": ("camera_frame", ("jpeg_base64",)),
}


class RobotMessageRouter:
    """로봇이 올린 JSON 메시지 하나를 type에 맞는 핸들러로 보낸다."""

    def __init__(self) -> None:
        self.patrol_resumer = PatrolResumer(registry.send_command)
        self._patrol_runs = PatrolRunStore()
        self._slam = SlamResultSaver()
        self._intruders = IntruderReporter(registry)
        self._recognitions = RecognitionCapture()
        self._medications = MedicationRecorder()
        self._handlers: Dict[str, Callable] = {
            "position": self._on_position,
            "nav_status": self._on_nav_status,
            "slam_saved": self._on_slam_saved,
            "patrol_progress": self._on_patrol_progress,
            "patrol_done": self._on_patrol_done,
            "battery_state": self._on_battery_state,
            "dock_result": self._on_dock_result,
            "medication_done": self._on_medication_done,
            "intruder_detected": self._on_intruder_detected,
            "person_recognized": self._on_person_recognized,
        }
        for msg_type in _RELAY_MESSAGES:
            self._handlers[msg_type] = self._relay

    async def dispatch(self, robot_code: str, message: dict) -> None:
        handler = self._handlers.get(message.get("type"))
        if handler is None:
            return  # 모르는 type은 조용히 무시(로봇 펌웨어가 서버보다 앞서갈 수 있다)
        await handler(robot_code, message)

    # ---- 단순 중계 ----------------------------------------------------------------

    async def _relay(self, robot_code: str, message: dict) -> None:
        out_type, fields = _RELAY_MESSAGES[message["type"]]
        await dashboard_ws.broadcast(
            {"type": out_type, "robot_code": robot_code, **{f: message.get(f) for f in fields}}
        )

    # ---- 위치·상태 ------------------------------------------------------------------

    async def _on_position(self, robot_code: str, message: dict) -> None:
        # pos_std_m: LIMO uplink_node가 /amcl_pose 공분산에서 뽑아 보낸다. P6 "위치 보정" 직후
        # AMCL 수렴 판단에 쓴다. TF 기반 경로(P5 SLAM 전용)는 공분산이 없어 None으로 넘어간다.
        position = {key: message.get(key) for key in ("x", "y", "yaw", "pos_std_m")}
        registry.remember_position(robot_code, position)
        await dashboard_ws.broadcast({"type": "robot_position", "robot_code": robot_code, **position})

    async def _on_nav_status(self, robot_code: str, message: dict) -> None:
        # GET /robots가 이 캐시값을 같이 내려줘, 메시지 한 번 유실돼도 새로고침으로 복구된다.
        registry.set_nav_running(robot_code, bool(message.get("running")))
        await dashboard_ws.broadcast(
            {"type": "nav_status", "robot_code": robot_code, "running": message.get("running")}
        )

    async def _on_dock_result(self, robot_code: str, message: dict) -> None:
        # 도킹 실패는 patrol_done/nav 성공 여부와 별개로만 알린다(충전 웨이포인트엔 이미 도착).
        await dashboard_ws.broadcast(
            {"type": "dock_result", "robot_code": robot_code, "docked": bool(message.get("docked"))}
        )

    # ---- SLAM ---------------------------------------------------------------------

    async def _on_slam_saved(self, robot_code: str, message: dict) -> None:
        map_id = await asyncio.to_thread(self._slam.save, message)
        await dashboard_ws.broadcast(
            {"type": "slam_done", "robot_code": robot_code, "job_id": message.get("job_id"), "map_id": map_id}
        )

    # ---- 순찰 ---------------------------------------------------------------------

    async def _on_patrol_progress(self, robot_code: str, message: dict) -> None:
        run_id = message.get("run_id")
        done = self.patrol_resumer.absolute_progress(run_id, message.get("waypoints_done"))
        self._patrol_runs.update(run_id, waypoints_done=done)
        await dashboard_ws.broadcast(
            {"type": "patrol_progress", "robot_code": robot_code, "run_id": run_id, "waypoints_done": done}
        )

    async def _on_patrol_done(self, robot_code: str, message: dict) -> None:
        run_id = message.get("run_id")
        status = message.get("status", "completed")
        self._patrol_runs.update(run_id, status=status, ended=True)
        self.patrol_resumer.finish(run_id)
        await dashboard_ws.broadcast(
            {"type": "patrol_done", "robot_code": robot_code, "run_id": run_id, "status": status}
        )
        # SOS 이동(run_id=0)이 막 끝났고 그게 밀어낸 순찰이 있으면 이어서 재개한다.
        resume_run_id = self.patrol_resumer.take_pending(robot_code)
        if resume_run_id is not None:
            await self.patrol_resumer.resume(resume_run_id)

    # ---- 배터리 -------------------------------------------------------------------

    async def _on_battery_state(self, robot_code: str, message: dict) -> None:
        percentage = message.get("percentage")
        if percentage is None:
            return
        # 스무딩은 서버(단일 소스)에서 한다 — 화면이 늘어도 모든 대시보드가 다듬어진 값만 받는다.
        smoothed = battery_monitor.smooth(robot_code, percentage)
        # 대시보드 중계는 가벼운 broadcast 하나뿐이라 스로틀 없이 그대로 보낸다.
        await dashboard_ws.broadcast({"type": "battery_state", "robot_code": robot_code, "percentage": smoothed})

        if not battery_monitor.due_for_threshold_check(robot_code):
            return
        # DB 조회(blocking)라 스레드로 뺀다. 표시값과 같은 소스(smoothed)로 판정해야 "화면엔
        # 안 튀는데 복귀 알림만 뒤집히는" 불일치가 없다.
        return_map_id = await asyncio.to_thread(battery_monitor.check_thresholds, robot_code, smoothed)
        if return_map_id is None:
            return
        # 빈 웨이포인트 + return_to_charging=True는 로봇이 이미 "다 돌고 충전소 복귀"로 처리하는
        # 조합이라 새 명령 타입이 필요 없다. override=True — SOS처럼 진행 중인 순찰/이동도
        # 밀어내고 우선 실행한다. 단 복귀 후 원래 순찰을 재개하지는 않는다(SOS만 재개 대상).
        await send_command(robot_code, {
            "type": "patrol_start",
            "run_id": 0,
            "waypoints": [],
            "return_to_charging": True,
            "map_id": return_map_id,
            "override": True,
        })

    # ---- 복약 ---------------------------------------------------------------------

    async def _on_medication_done(self, robot_code: str, message: dict) -> None:
        # DeliverMedication 액션이 끝날 때마다(자동 순찰 경로든 P3 수동 재안내든) 온다.
        record = await asyncio.to_thread(
            self._medications.record,
            robot_code,
            message.get("schedule_id", 0),
            message.get("slot_no", 0),
            message.get("status", "hw_error"),
        )
        if record is None:
            return
        await dashboard_ws.broadcast({
            "type": "medication_done", "robot_code": robot_code,
            "schedule_id": message.get("schedule_id"), "slot_no": message.get("slot_no"),
            "status": message.get("status"),
        })
        if record.alert is not None:
            await self._announce_medication_alert(robot_code, record.alert)

    @staticmethod
    async def _announce_medication_alert(robot_code: str, alert: MedicationAlert) -> None:
        # 미수령 care_event는 이미 만들어졌다. 대시보드엔 실시간 알림, 로봇엔 TTS 음성 + 영상
        # 녹화 트리거를 내려보낸다(로봇이 지금 그 자리에 있으므로 즉시 안내).
        await dashboard_ws.broadcast({
            "type": "event_created", "event_uid": alert.event_uid,
            "severity": alert.severity, "event_type": alert.event_type,
            "event_type_label": EVENT_TYPE_LABELS[alert.event_type],
            "summary": alert.summary, "occurred_at": alert.occurred_at,
            "resolved": False, "robot_code": robot_code,
        })
        await send_command(robot_code, {
            "type": "medication_alert", "care_event_id": alert.event_id,
            "severity": alert.severity, "text": alert.summary,
        })

    # ---- 인식 ---------------------------------------------------------------------

    async def _on_intruder_detected(self, robot_code: str, message: dict) -> None:
        payload = await asyncio.to_thread(self._intruders.report, robot_code, message)
        if payload is not None:
            await dashboard_ws.broadcast(payload)

    async def _on_person_recognized(self, robot_code: str, message: dict) -> None:
        await asyncio.to_thread(self._recognitions.save, robot_code, message)


message_router = RobotMessageRouter()


# ---- 다른 모듈이 쓰는 함수형 API(기존 시그니처 유지) -------------------------------------

def is_connected(robot_code: str) -> bool:
    return registry.is_connected(robot_code)


def connected_robot_codes() -> list:
    return registry.connected_codes()


def latest_positions() -> Dict[str, dict]:
    return registry.positions()


def latest_battery() -> Dict[str, int]:
    return battery_monitor.latest()


def nav_running(robot_code: str) -> bool:
    return registry.nav_running(robot_code)


def register_pending_resume(robot_code: str, run_id: int) -> None:
    """patrol.py::dispatch_drive_to_point가 SOS 이동을 보내기 직전에 부른다."""
    message_router.patrol_resumer.register_pending(robot_code, run_id)


def clear_pending_resume(robot_code: str) -> None:
    """patrol.py::cancel_patrol_run이 부른다 — 취소된 순찰은 SOS 이후에도 재개하지 않는다."""
    message_router.patrol_resumer.clear_pending(robot_code)


async def send_command(robot_code: str, message: dict) -> bool:
    """robot_code 로봇에 명령을 보낸다. 연결이 없거나 전송에 실패하면 False."""
    return await registry.send_command(robot_code, message)


# ---- 엔드포인트 -----------------------------------------------------------------------

@router.websocket("/ws/robot")
async def robot_ws_endpoint(websocket: WebSocket, robot_code: str = Query(...), role: str = Query("downlink")):
    if not await asyncio.to_thread(robot_exists, robot_code):
        await websocket.close(code=4404)
        return

    is_command_channel = registry.is_command_channel(role)
    await websocket.accept()
    registry.attach(role, robot_code, websocket)
    if is_command_channel:
        # "온라인" 배지는 명령이 전달될 수 있는지(downlink)를 뜻한다 — uplink만 살아있는
        # (카메라는 보이는데 명령은 안 먹히는) 상태가 "온라인"으로 보이면 위험을 못 알아챈다.
        await dashboard_ws.broadcast({"type": "robot_status", "robot_code": robot_code, "online": True})

    try:
        while True:
            message = await websocket.receive_json()
            await _dispatch_safely(robot_code, message)
    except WebSocketDisconnect:
        pass
    finally:
        registry.detach(role, robot_code, websocket)
        if is_command_channel:
            await dashboard_ws.broadcast({"type": "robot_status", "robot_code": robot_code, "online": False})


async def _dispatch_safely(robot_code: str, message: dict) -> None:
    """메시지 하나의 처리 실패가 로봇 연결 전체를 끊지 않게 하는 경계.

    예전엔 핸들러가 예외를 던지면(DB 일시 장애 등) receive 루프가 통째로 종료돼 로봇이
    오프라인으로 보였다. 여기서 한 번만 잡아 스택트레이스를 남기고 다음 메시지를 계속 받는다.
    """
    try:
        await message_router.dispatch(robot_code, message)
    except Exception:  # noqa: BLE001 — 메시지 단위 경계, 아래에서 반드시 로그를 남긴다
        logger.exception("로봇 %s 메시지 처리 실패 (type=%s)", robot_code, message.get("type"))
