"""app/services/slam_result.py

SLAM 완료 보고(slam_saved)를 map 행으로 저장한다.
"""
import logging
from typing import Optional

import psycopg2.extras

from ..api.v1 import media
from ..core import db
from ..core.errors import STORAGE_ERRORS

logger = logging.getLogger(__name__)

_UPSERT_MAP = """
    INSERT INTO map (robot_id, name, raw_yaml, resolution, origin_x, origin_y, origin_yaw,
                      width_px, height_px, is_active)
    VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, TRUE)
    ON CONFLICT (robot_id, name) DO UPDATE SET
        raw_yaml = EXCLUDED.raw_yaml,
        resolution = EXCLUDED.resolution,
        origin_x = EXCLUDED.origin_x,
        origin_y = EXCLUDED.origin_y,
        origin_yaw = EXCLUDED.origin_yaw,
        width_px = EXCLUDED.width_px,
        height_px = EXCLUDED.height_px,
        is_active = TRUE,
        created_at = NOW()
    RETURNING id
"""


class SlamResultSaver:
    """slam_job을 saved로 표시하고 map 행을 만든다.

    맵 이미지는 careguard_agent/media_uploader.py가 이미 POST /media/upload-map으로 올려
    files.id(file_id)가 있다 — 업로드 시점엔 map.id가 없어 임시로 entity_id=robot_id였으므로
    여기서 새 map.id로 다시 연결한다.
    """

    def save(self, message: dict) -> Optional[int]:
        job_id = message.get("job_id")
        if job_id is None:
            return None
        file_id = message.get("file_id")

        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute("SELECT robot_id, map_name FROM slam_job WHERE id = %s", (job_id,))
            job = cur.fetchone()
            if job is None:
                return None

            cur.execute("UPDATE map SET is_active = FALSE WHERE robot_id = %s", (job["robot_id"],))
            # (robot_id, name)은 UNIQUE — 같은 이름으로 재매핑하면 기존 행을 덮어쓴다. map.id가
            # 유지돼야 그 맵에 등록된 waypoint가 재저장 후에도 유효하다.
            cur.execute(_UPSERT_MAP, self._map_params(job, message))
            map_id = cur.fetchone()["id"]

            if file_id is not None:
                self._relink_image(cur, map_id, file_id)

            cur.execute(
                "UPDATE slam_job SET status = 'saved', map_id = %s, ended_at = NOW() WHERE id = %s",
                (map_id, job_id),
            )
            conn.commit()

        if file_id is not None:
            self._write_yaml_sidecar(file_id, message.get("raw_yaml", {}))
        return map_id

    @staticmethod
    def _map_params(job: dict, message: dict) -> tuple:
        return (
            job["robot_id"],
            job["map_name"],
            psycopg2.extras.Json(message.get("raw_yaml", {})),
            message.get("resolution"),
            message.get("origin_x", 0),
            message.get("origin_y", 0),
            message.get("origin_yaw", 0),
            message.get("width_px"),
            message.get("height_px"),
        )

    @staticmethod
    def _relink_image(cur, map_id: int, file_id: int) -> None:
        # 같은 map_id에 연결돼 있던 예전 이미지는 "현재 이미지" 후보에서 뺀다(안 그러면
        # GET /maps/active의 LEFT JOIN이 행을 중복시킨다). 파일은 지우지 않고 soft delete만 —
        # "파일로 등록"으로 다시 불러올 수 있어야 하기 때문이다.
        cur.execute(
            "UPDATE files SET deleted_at = NOW() "
            "WHERE entity_type = 'map_image' AND entity_id = %s AND id != %s AND deleted_at IS NULL",
            (str(map_id), file_id),
        )
        cur.execute(
            "UPDATE files SET entity_id = %s WHERE id = %s AND entity_type = 'map_image'",
            (str(map_id), file_id),
        )

    @staticmethod
    def _write_yaml_sidecar(file_id: int, raw_yaml: dict) -> None:
        # DB 커밋은 이미 끝났다 — .yaml 사이드카는 "파일로 등록" 편의 기능이라, 실패해도
        # 맵 저장 자체를 실패로 되돌리지 않고 기록만 남긴다.
        try:
            media.write_map_yaml_sidecar(file_id, raw_yaml)
        except STORAGE_ERRORS:
            logger.exception("맵 .yaml 사이드카 저장 실패 (file_id=%s)", file_id)
