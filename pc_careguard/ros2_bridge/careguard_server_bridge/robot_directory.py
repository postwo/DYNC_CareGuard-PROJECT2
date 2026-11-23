"""중계 대상 로봇 목록 — 서버 REST(GET /robots: code, ros_domain_id)에서 읽는다."""
from __future__ import annotations

import json
import urllib.request
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional


@dataclass(frozen=True)
class RobotEntry:
    code: str
    ros_domain_id: Optional[int] = None


def parse_robots(payload: object, only_codes: Iterable[str] = ()) -> List[RobotEntry]:
    wanted = {c.strip() for c in only_codes if c.strip()}
    robots: List[RobotEntry] = []
    if not isinstance(payload, list):
        return robots
    for row in payload:
        if not isinstance(row, dict) or not row.get("code"):
            continue
        code = str(row["code"])
        if wanted and code not in wanted:
            continue
        domain = row.get("ros_domain_id")
        robots.append(RobotEntry(code, int(domain) if domain is not None else None))
    return robots


def group_by_domain(robots: Iterable[RobotEntry], default_domain: int) -> Dict[int, List[str]]:
    groups: Dict[int, List[str]] = {}
    for robot in robots:
        domain = robot.ros_domain_id if robot.ros_domain_id is not None else default_domain
        groups.setdefault(domain, []).append(robot.code)
    return groups


def fetch_robots(server_http_url: str, only_codes: Iterable[str], log) -> Optional[List[RobotEntry]]:
    """서버가 아직 안 떠 있으면 None(호출부가 잠시 뒤 다시 시도한다)."""
    try:
        with urllib.request.urlopen(f"{server_http_url}/robots", timeout=5) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except (OSError, ValueError) as exc:
        log.warning("로봇 목록 조회 실패(%s/robots): %s", server_http_url, exc)
        return None
    return parse_robots(payload, only_codes)
