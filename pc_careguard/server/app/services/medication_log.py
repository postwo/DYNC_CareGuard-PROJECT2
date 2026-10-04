"""app/services/medication_log.py

로봇의 medication_done 보고를 medication_log에 기록하고, 미수령이면 care_event를 만든다.
scheduler.py::_medication_tick()이 오늘자 'delivered' 기록 유무로 "미수령"을 판단하는데,
그 기록을 남기는 프로덕션 코드가 이 클래스다.
"""
import json
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Optional

from ..core import db

# DeliverMedication 액션 결과 -> medication_log.result CHECK 제약 값.
#  timeout   사람을 못 찾음 -> not_found
#  jammed    약통은 열렸는데 RETRIEVAL_TIMEOUT_SEC 안에 안 꺼내감. 배출(서보)은 성공했으니
#            hw_error와 다르다 -> missed (오늘 미수령 카운트에 잡히도록)
#  hw_error  서보/통신 고장 -> error
#  cancelled 사람이 껐거나 순찰 취소 -> skipped (미수령과 다름)
STATUS_TO_RESULT = {
    "delivered": "delivered",
    "timeout": "not_found",
    "jammed": "missed",
    "hw_error": "error",
    "cancelled": "skipped",
}


@dataclass(frozen=True)
class MedicationAlert:
    """미수령으로 만들어진 care_event 정보 — 호출부가 대시보드 브로드캐스트와 로봇 TTS/녹화 명령에 쓴다."""
    event_uid: str
    event_id: int
    event_type: str
    severity: str
    summary: str
    occurred_at: str
    recipient_id: int


@dataclass(frozen=True)
class MedicationRecord:
    log_id: int
    alert: Optional[MedicationAlert]


class MedicationRecorder:
    def record(self, robot_code: str, schedule_id: int, slot_no: int, status: str) -> Optional[MedicationRecord]:
        """결과를 기록한다. 로봇이 등록 해제됐거나 대상자를 특정할 수 없으면 None."""
        result = STATUS_TO_RESULT.get(status, "error")
        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute("SELECT id FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
            robot = cur.fetchone()
            if robot is None:
                return None
            robot_id = robot["id"]

            recipient_id = self._resolve_recipient(cur, robot_id, schedule_id)
            if recipient_id is None:
                return None

            if not schedule_id and slot_no:
                schedule_id = self._schedule_for_slot(cur, recipient_id, slot_no) or schedule_id

            log_id = self._insert_log(cur, schedule_id, recipient_id, robot_id, slot_no, result)
            alert = None
            if result == "missed":
                alert = self._raise_missed_event(cur, robot_code, robot_id, recipient_id,
                                                 schedule_id, slot_no, log_id)
            conn.commit()
        return MedicationRecord(log_id=log_id, alert=alert)

    @staticmethod
    def _resolve_recipient(cur, robot_id: int, schedule_id: int) -> Optional[int]:
        """로봇은 recipient를 모른다(액션 goal엔 schedule_id/slot_no뿐). schedule_id로 거꾸로
        찾고, 0(P3 수동 재안내처럼 특정 회차와 안 묶인 호출)이면 로봇에 배정된 대상자 1명으로 대체한다."""
        if schedule_id:
            cur.execute("SELECT recipient_id FROM medication_schedule WHERE id = %s", (schedule_id,))
            row = cur.fetchone()
            if row is not None:
                return row["recipient_id"]
        cur.execute(
            "SELECT id FROM recipient WHERE robot_id = %s AND deregistered_at IS NULL LIMIT 1", (robot_id,)
        )
        row = cur.fetchone()
        return row["id"] if row else None

    @staticmethod
    def _schedule_for_slot(cur, recipient_id: int, slot_no: int) -> Optional[int]:
        """P3 "복약 재안내"는 schedule_id를 모른 채 온다. 그대로 두면 medication_log.schedule_id가
        NULL이라 _medication_tick의 미수령 판정이 이 행을 못 찾아 경보가 안 풀렸다 —
        recipient_id + slot_no로 활성 스케줄을 역으로 찾아 채운다."""
        cur.execute(
            "SELECT id FROM medication_schedule "
            "WHERE recipient_id = %s AND slot_no = %s AND deregistered_at IS NULL",
            (recipient_id, slot_no),
        )
        row = cur.fetchone()
        return row["id"] if row else None

    @staticmethod
    def _insert_log(cur, schedule_id, recipient_id, robot_id, slot_no, result) -> int:
        delivered = result == "delivered"
        cur.execute(
            """
            INSERT INTO medication_log (schedule_id, recipient_id, robot_id, slot_no, result, delivered_at, confirmed_by)
            VALUES (%s, %s, %s, %s, %s, %s, %s)
            RETURNING id
            """,
            (
                schedule_id or None, recipient_id, robot_id, slot_no or None, result,
                # delivered는 IR 센서가 "회수"를 직접 확인해야만 오므로 항상 센서 근거다.
                datetime.now(timezone.utc) if delivered else None,
                "sensor" if delivered else None,
            ),
        )
        return cur.fetchone()["id"]

    @staticmethod
    def _raise_missed_event(cur, robot_code: str, robot_id: int, recipient_id: int,
                            schedule_id, slot_no, log_id: int) -> MedicationAlert:
        # "오늘 recipient당 총 몇 번" 기준(스케줄이 달라도 합산). 이 INSERT와 같은 트랜잭션에서
        # 세므로 방금 넣은 행도 포함된다. 날짜가 바뀌면 created_at::date가 달라져 자동으로 0부터.
        cur.execute(
            "SELECT COUNT(*) AS n FROM medication_log "
            "WHERE recipient_id = %s AND result = 'missed' AND created_at::date = CURRENT_DATE",
            (recipient_id,),
        )
        missed_today = cur.fetchone()["n"]

        if missed_today == 1:
            event_type, severity, summary = "MED_MISSED", "attention", "약을 복용하지 않았습니다"
        else:
            event_type, severity = "MED_MISSED_STREAK", "emergency"
            summary = f"약을 {missed_today}번 복용하지 않았습니다"

        event_uid = f"EV-{robot_code}-{datetime.now(timezone.utc):%y%m%d}-{uuid.uuid4().hex[:8]}"
        cur.execute(
            """
            INSERT INTO care_event (event_uid, recipient_id, robot_id, trigger_path, event_type,
                severity, summary, evidence, occurred_at)
            VALUES (%s, %s, %s, 'C', %s, %s, %s, %s, NOW())
            RETURNING id, occurred_at
            """,
            (
                event_uid, recipient_id, robot_id, event_type, severity, summary,
                json.dumps({"schedule_id": schedule_id, "slot_no": slot_no, "missed_today": missed_today}),
            ),
        )
        event = cur.fetchone()
        cur.execute("UPDATE medication_log SET care_event_id = %s WHERE id = %s", (event["id"], log_id))
        return MedicationAlert(
            event_uid=event_uid, event_id=event["id"], event_type=event_type, severity=severity,
            summary=summary, occurred_at=event["occurred_at"].isoformat(), recipient_id=recipient_id,
        )
