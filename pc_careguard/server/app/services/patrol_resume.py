"""app/services/patrol_resume.py

SOS 이동에 밀려난(paused) 순찰의 재개 관리.
"""
import logging
from typing import Awaitable, Callable, Dict, Optional

from ..core import db
from ..ws import dashboard_ws

logger = logging.getLogger(__name__)

CommandSender = Callable[[str, dict], Awaitable[bool]]


class PatrolResumer:
    """
    - pending: robot_code -> SOS로 밀려난 patrol_run.id. dispatch_drive_to_point가 SOS 이동을
      보내기 직전에 등록하고, 그 이동의 patrol_done을 받는 순간 재개한다. run_id=0인 "이동"은
      patrol_run 행이 없어 patrol_done만으로는 "방금 그 SOS였는지" 알 수 없으므로 DB가 아니라
      이 in-memory 표식으로 정확히 한 번만 재개를 트리거한다.
    - offsets: patrol_run.id -> 재개 시점의 waypoints_done. 재개 goal은 남은 웨이포인트만 담아
      보내므로 로봇이 보고하는 인덱스는 그 목록 기준이다 — 누적 진행률은 이 오프셋을 더해야 한다.
    """

    def __init__(self, send_command: CommandSender) -> None:
        self._send_command = send_command
        self._pending: Dict[str, int] = {}
        self._offsets: Dict[int, int] = {}

    def register_pending(self, robot_code: str, run_id: int) -> None:
        self._pending[robot_code] = run_id

    def clear_pending(self, robot_code: str) -> None:
        """순찰을 사용자가 취소하면 SOS 이동이 끝나도 이어서 돌면 안 된다."""
        self._pending.pop(robot_code, None)

    def take_pending(self, robot_code: str) -> Optional[int]:
        """표식을 한 번 쓰고 지운다(중복 재개 방지)."""
        return self._pending.pop(robot_code, None)

    def absolute_progress(self, run_id: Optional[int], reported: Optional[int]) -> Optional[int]:
        if reported is None:
            return None
        return reported + self._offsets.get(run_id, 0)

    def finish(self, run_id: Optional[int]) -> None:
        self._offsets.pop(run_id, None)

    def reset(self) -> None:
        self._pending.clear()
        self._offsets.clear()

    async def resume(self, run_id: int) -> None:
        """paused 순찰을 이어서 돌린다. 이미 끝낸 웨이포인트는 건너뛰고 남은 것만 새 goal로 보낸다.
        override는 주지 않는다 — 이 시점엔 patrol_task가 SOS 이동을 막 끝내 비어 있다."""
        plan = self._prepare(run_id)
        if plan is None:
            return  # 이미 취소됐거나 다른 경로로 정리됨
        robot_code, command = plan
        if command is not None:
            await self._send_command(robot_code, command)
        await dashboard_ws.broadcast({"type": "patrol_run_updated", "robot_code": robot_code})

    def _prepare(self, run_id: int):
        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute(
                """
                SELECT pr.robot_id, pr.route_id, pr.waypoints_done, pr.waypoints_total,
                       r.code AS robot_code, rt.map_id, rt.steps
                FROM patrol_run pr
                JOIN robot r ON r.id = pr.robot_id
                JOIN patrol_route rt ON rt.id = pr.route_id
                WHERE pr.id = %s AND pr.status = 'paused'
                """,
                (run_id,),
            )
            row = cur.fetchone()
            if row is None:
                return None

            remaining_ids = (row["steps"] or [])[row["waypoints_done"]:]
            if not remaining_ids:
                # SOS가 마지막 웨이포인트 직후에 끼어든 경우 — 남은 게 없으니 완료 처리.
                cur.execute("UPDATE patrol_run SET status = 'completed', ended_at = NOW() WHERE id = %s", (run_id,))
                conn.commit()
                return row["robot_code"], None

            cur.execute("SELECT id, code, pos_x, pos_y, yaw FROM waypoint WHERE id = ANY(%s)", (remaining_ids,))
            by_id = {w["id"]: w for w in cur.fetchall()}
            waypoints = [by_id[wid] for wid in remaining_ids if wid in by_id]

            cur.execute("UPDATE patrol_run SET status = 'running' WHERE id = %s", (run_id,))
            conn.commit()

        self._offsets[run_id] = row["waypoints_done"]
        command = {
            "type": "patrol_start",
            "run_id": run_id,
            "waypoints": [{"code": w["code"], "x": w["pos_x"], "y": w["pos_y"], "yaw": w["yaw"]} for w in waypoints],
            "map_id": row["map_id"],
        }
        return row["robot_code"], command


class PatrolRunStore:
    """patrol_run 진행 상태 갱신."""

    def update(self, run_id: Optional[int], *, waypoints_done: Optional[int] = None,
               status: Optional[str] = None, ended: bool = False) -> None:
        if run_id is None:
            return
        sets, params = [], []
        if waypoints_done is not None:
            sets.append("waypoints_done = %s")
            params.append(waypoints_done)
        if status is not None:
            sets.append("status = %s")
            params.append(status)
        if ended:
            sets.append("ended_at = NOW()")
        if not sets:
            return
        params.append(run_id)
        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute(f"UPDATE patrol_run SET {', '.join(sets)} WHERE id = %s", params)
            conn.commit()
