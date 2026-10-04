"""careguard_turtlebot_agent/patrol_controller.py

patrol_start/patrol_stop 명령을 받아 웨이포인트 순찰 또는 충전소 단독 복귀
상태 기계를 돌린다 — downlink_node.py에 몰려있던 책임을 분리했다(2026-09-15
리팩터링). LIMO(careguard_mission/patrol_task.py)와 같은 역할이지만 그쪽이
액션 서버 하나로 같이 처리하는 영상 녹화(careguard_perception)는 터틀봇
워크스페이스에 없어서 빠졌고, 음성 안내(careguard_hmi)만 VoiceAnnouncer를
직접 호출하는 방식으로 재현했다(downlink_node.py 모듈 docstring 참고).
웨이포인트를 순서대로 돌다가 하나라도 실패하면 즉시 중단하고(실패해도 조용히
다음으로 넘어가면 "순찰 완료"로 잘못 기록된다), 끝나면(return_to_charging이면)
충전소로 복귀한다. 진행률은 patrol_progress/patrol_done으로 서버에 보고한다
(robot_ws.py가 DB의 patrol_run 행과 대시보드로 그대로 릴레이).
"""
import threading
import traceback
from typing import Callable, List, Optional

from .errors import WaypointLookupError
from .nav2_manager import Nav2Manager
from .voice import VoiceAnnouncer
from .waypoints import fetch_charge_waypoint


class PatrolController:
    def __init__(self, nav2: Nav2Manager, voice: VoiceAnnouncer, send: Callable[[dict], None], logger,
                 publish_recording: Optional[Callable[[dict], None]] = None):
        self._nav2 = nav2
        self._voice = voice
        self._send = send
        self._logger = logger
        # perception/video_recorder.py의 PATROL_RECORDING_TOPIC 발행 콜백(downlink_node.py가
        # 주입) — None이면(video_recorder 미배포 등) 조용히 건너뛴다.
        self._publish_recording = publish_recording or (lambda payload: None)
        self._active = False
        self._cancel_requested = threading.Event()
        self._thread: Optional[threading.Thread] = None

    @property
    def is_active(self) -> bool:
        return self._active

    @staticmethod
    def _parse_waypoints(raw: list) -> List[dict]:
        """서버가 보낸 웨이포인트를 주행에 쓸 숫자 값으로 바꾼다. 모양이 틀리면 KeyError/TypeError/
        ValueError — 주행 도중이 아니라 시작 전에 걸러내려는 것이다."""
        return [{
            'code': wp.get('code', '?'),
            'x': float(wp['x']),
            'y': float(wp['y']),
            'yaw': float(wp.get('yaw', 0.0) or 0.0),
        } for wp in raw]

    def _reject(self, run_id: int, reason: str):
        """시작하지 못한 순찰을 서버에 실패로 알린다. 안 알리면 서버의 patrol_run이 'running'으로
        고아가 된다(run_id=0인 충전 복귀·이동은 patrol_run 행이 없어 알릴 게 없다)."""
        self._logger.error(reason)
        if run_id > 0:
            self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'failed'})

    def start(self, message: dict):
        run_id = int(message.get('run_id', 0) or 0)
        waypoints = message.get('waypoints') or []
        return_to_charging = bool(message.get('return_to_charging', True))
        if not waypoints and not return_to_charging:
            # "이동"(단일 지점, return_to_charging=False) — P6 "이동" 버튼/SOS 자동
            # 출동이 이 조합을 보낸다. 여전히 스코프 밖(downlink_node.py 모듈
            # docstring 참고).
            self._logger.warn(
                '터틀봇은 아직 단일 지점 "이동"을 지원하지 않습니다 — '
                '웨이포인트 순찰 또는 배터리 자동 복귀(빈 웨이포인트+return_to_charging)만 처리합니다')
            return

        if self._active:
            self._reject(run_id, '이미 주행 중입니다 — 새 요청을 무시합니다')
            return

        map_id = int(message.get('map_id', 0) or 0)
        if map_id <= 0:
            self._reject(run_id, 'patrol_start에 유효한 map_id가 없습니다')
            return
        try:
            waypoints = self._parse_waypoints(waypoints)
        except (KeyError, TypeError, ValueError, AttributeError) as exc:
            self._reject(run_id, f'patrol_start의 웨이포인트 형식이 올바르지 않습니다: {exc!r}')
            return

        try:
            charge_x, charge_y, charge_yaw = fetch_charge_waypoint(map_id)
        except WaypointLookupError as exc:
            self._reject(run_id, f'충전 웨이포인트 조회 실패: {exc}')
            return

        if not self._nav2.ensure_running(map_id, charge_x, charge_y, charge_yaw):
            self._reject(run_id, 'Nav2 준비 실패로 주행을 시작하지 않습니다')
            return

        self._cancel_requested.clear()
        self._active = True
        if waypoints:
            self._thread = threading.Thread(
                target=self._run_patrol,
                args=(run_id, waypoints, return_to_charging, charge_x, charge_y, charge_yaw),
                daemon=True)
        else:
            self._thread = threading.Thread(
                target=self._drive_to_charge, args=(charge_x, charge_y, charge_yaw), daemon=True)
        self._thread.start()

    def stop(self, message: dict):
        if not self._active:
            self._logger.warn('취소할 진행 중인 주행이 없습니다')
            return
        self._cancel_requested.set()

    def _drive_to_charge(self, x: float, y: float, yaw: float):
        try:
            if self._cancel_requested.is_set():
                return
            self._voice.speak('충전소로 복귀합니다')
            success = self._nav2.go_to_pose_and_wait(x, y, yaw, self._cancel_requested)
            if self._cancel_requested.is_set():
                self._logger.info('충전소 복귀 주행이 취소됐습니다')
            else:
                self._logger.info(f'충전소 복귀 결과: {"성공" if success else "실패"}')
                if success:
                    self._voice.speak('충전을 시작합니다')
        except Exception:  # noqa: BLE001 - 스레드의 최상위 경계: 스택트레이스를 남기고 스레드를 끝낸다
            self._logger.error(f'충전소 복귀 중 예외로 중단됐습니다:\n{traceback.format_exc()}')
        finally:
            self._active = False

    def _run_patrol(self, run_id: int, waypoints: list, return_to_charging: bool,
                     charge_x: float, charge_y: float, charge_yaw: float):
        try:
            self._voice.speak('주행을 시작합니다')
            self._publish_recording({'action': 'start', 'run_id': run_id})
            waypoints_done = 0
            for wp in waypoints:
                if self._cancel_requested.is_set():
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'cancelled'})
                    self._logger.info('순찰이 취소됐습니다')
                    return

                success = self._nav2.go_to_pose_and_wait(wp['x'], wp['y'], wp['yaw'], self._cancel_requested)

                if self._cancel_requested.is_set():
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'cancelled'})
                    self._logger.info(f"{wp['code']} 이동 중 취소됨")
                    return
                if not success:
                    self._logger.error(f"{wp['code']} 이동 실패 — 순찰을 중단합니다")
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'failed'})
                    return

                waypoints_done += 1
                self._send({'type': 'patrol_progress', 'run_id': run_id, 'waypoints_done': waypoints_done})

            if return_to_charging:
                self._voice.speak('충전소로 복귀합니다')
                success = self._nav2.go_to_pose_and_wait(charge_x, charge_y, charge_yaw, self._cancel_requested)
                if self._cancel_requested.is_set():
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'cancelled'})
                    self._logger.info('충전소 복귀 중 취소됨')
                    return
                if not success:
                    self._logger.error('충전소 복귀 실패')
                    self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'failed'})
                    return
                self._voice.speak('충전을 시작합니다')

            self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'completed'})
            self._logger.info(f'순찰 완료: run_id={run_id}')
        except Exception:  # noqa: BLE001 - 스레드의 최상위 경계
            # 주행 도중 예상 못 한 예외(Nav2 API 오류 등)가 나도 서버에 알려야 한다 — 안 알리면 서버의
            # patrol_run이 'running'으로 고아가 되고 대시보드가 영영 "순찰 중"으로 남는다.
            self._logger.error(f'순찰 중 예외로 중단됐습니다:\n{traceback.format_exc()}')
            self._send({'type': 'patrol_done', 'run_id': run_id, 'status': 'failed'})
        finally:
            # 성공/실패/취소 어느 경로로 빠져나가든(위쪽 return 지점마다 각자 안 챙겨도)
            # 여기서 한 번만 stop을 쏘면 video_recorder.py가 반드시 녹화를 마무리한다.
            self._publish_recording({'action': 'stop', 'run_id': run_id})
            self._active = False
