"""app/services/intruder.py

로봇이 보고한 침입자(미등록 인물)·인식된 대상자 처리.
"""
import base64
import json
import logging
import uuid
from datetime import datetime
from typing import Optional
from zoneinfo import ZoneInfo

from ..api.v1 import media
from ..api.v1.event import EVENT_TYPE_LABELS
from ..core import db
from ..core.errors import BASE64_DECODE_ERRORS, STORAGE_ERRORS
from .robot_registry import RobotRegistry

logger = logging.getLogger(__name__)

KST = ZoneInfo("Asia/Seoul")


class IntruderReporter:
    """care_event(INTRUDER) 생성 — scheduler.py의 MED_MISSED 자동 생성과 같은 패턴.

    ESP32 호출 버튼(mqtt_listener.py)과 달리 이 이벤트는 로봇 스스로의 판단이라
    recipient_id를 채울 수 없다(아무도 매치 안 됐다는 뜻 그 자체이므로 NULL이 맞다).
    """

    def __init__(self, registry: RobotRegistry) -> None:
        self._registry = registry

    def report(self, robot_code: str, message: dict) -> Optional[dict]:
        """이벤트를 만들고 대시보드로 보낼 페이로드를 돌려준다. 등록 해제된 로봇이면 None."""
        now = datetime.now(KST)
        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
            robot = cur.fetchone()
            if robot is None:
                return None
            robot_id = robot["id"]

            patrol_run_id = self._running_patrol_run_id(cur, robot_id)
            position = self._registry.position_of(robot_code)
            pos_x = position["x"] if position else None
            pos_y = position["y"] if position else None

            event_uid = f"EV-{robot_code}-{now:%y%m%d}-{uuid.uuid4().hex[:8]}"
            summary = "순찰 중 미등록 인물이 감지되었습니다"
            evidence = {
                "track_id": message.get("track_id"),
                # 근접 재판별 결과인지 원거리 1회 판정인지 — 담당자가 신뢰도를 가늠하는 용도.
                "verified_at_close_range": message.get("verified_at_close_range"),
                "nearest_dist": message.get("nearest_dist"),
                "perception_run_id": message.get("run_id"),
            }
            cur.execute(
                """
                INSERT INTO care_event (event_uid, recipient_id, robot_id, patrol_run_id, trigger_path,
                    event_type, severity, summary, evidence, pos_x, pos_y, occurred_at)
                VALUES (%s, NULL, %s, %s, 'B', 'INTRUDER', 'emergency', %s, %s, %s, %s, NOW())
                RETURNING id, occurred_at
                """,
                (event_uid, robot_id, patrol_run_id, summary, json.dumps(evidence), pos_x, pos_y),
            )
            row = cur.fetchone()
            event_id, occurred_at = row["id"], row["occurred_at"]

            # 캡처 저장이 실패해도 이벤트 자체는 살린다 — 사진은 있으면 좋은 것이지 신고를 막을 이유는 아니다.
            file_id = self._save_capture(robot_code, event_id, message.get("image_base64"))
            if file_id is not None:
                evidence["capture_file_id"] = file_id
                cur.execute(
                    "UPDATE care_event SET evidence = %s WHERE id = %s", (json.dumps(evidence), event_id)
                )
            conn.commit()

        return {
            "type": "event_created",
            "event_uid": event_uid,
            "severity": "emergency",
            "event_type": "INTRUDER",
            "event_type_label": EVENT_TYPE_LABELS["INTRUDER"],
            "summary": summary,
            "occurred_at": occurred_at.isoformat(),
            "resolved": False,
            "robot_code": robot_code,
            "recipient_name": None,
        }

    @staticmethod
    def _running_patrol_run_id(cur, robot_id: int) -> Optional[int]:
        # perception 노드는 run_id를 evidence에만 참고용으로 싣는다 — DB의 patrol_run_id
        # 연결은 여기서 다시 조회해 정확히 채운다.
        cur.execute(
            "SELECT id FROM patrol_run WHERE robot_id = %s AND status = 'running' "
            "ORDER BY started_at DESC LIMIT 1",
            (robot_id,),
        )
        run = cur.fetchone()
        return run["id"] if run else None

    @staticmethod
    def _save_capture(robot_code: str, event_id: int, image_b64: Optional[str]) -> Optional[int]:
        if not image_b64:
            return None
        try:
            image_bytes = base64.b64decode(image_b64)
            return media.save_intruder_capture(robot_code, event_id, image_bytes)
        except BASE64_DECODE_ERRORS:
            logger.warning("침입자 캡처 base64가 올바르지 않아 건너뜀 (robot=%s, event=%s)", robot_code, event_id)
        except STORAGE_ERRORS:
            logger.exception("침입자 캡처 저장 실패 (robot=%s, event=%s)", robot_code, event_id)
        return None


class RecognitionCapture:
    """등록된 대상자로 확정된 순간의 캡처 저장 — 알림이 아니라 인식 기록이라 care_event는 만들지 않는다."""

    def save(self, robot_code: str, message: dict) -> None:
        recipient_id = message.get("recipient_id")
        image_b64 = message.get("image_base64")
        if recipient_id is None or not image_b64:
            return
        try:
            media.save_recognition_capture(robot_code, recipient_id, base64.b64decode(image_b64))
        except BASE64_DECODE_ERRORS:
            logger.warning("인식 캡처 base64가 올바르지 않아 건너뜀 (robot=%s)", robot_code)
        except STORAGE_ERRORS:
            logger.exception("인식 캡처 저장 실패 (robot=%s, recipient=%s)", robot_code, recipient_id)
