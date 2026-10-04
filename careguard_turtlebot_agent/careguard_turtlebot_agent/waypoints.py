"""careguard_turtlebot_agent/waypoints.py

서버 REST API(/waypoints)에서 충전 스테이션 좌표를 조회한다 —
downlink_node.py에 몰려있던 책임을 분리했다(2026-09-15 리팩터링).
careguard_nav/waypoint_manager.py::charging_waypoint()와 같은 조회를, 그
패키지를 가져다 쓰지 않고 직접 한다(터틀봇 워크스페이스에 careguard_nav가
없다 — downlink_node.py 모듈 docstring 참고).
"""
from typing import Tuple

import requests

from . import ws_config
from .errors import WaypointLookupError


def fetch_charge_waypoint(map_id: int) -> Tuple[float, float, float]:
    """충전 스테이션 (x, y, yaw)를 돌려준다. 실패하면 WaypointLookupError."""
    try:
        resp = requests.get(
            f'{ws_config.SERVER_HTTP_URL}/waypoints', params={'map_id': map_id}, timeout=10)
        resp.raise_for_status()
        waypoints = resp.json()
    except (requests.RequestException, ValueError) as exc:
        raise WaypointLookupError(f'웨이포인트 조회 요청 실패: {exc}') from exc

    charge = next((w for w in waypoints if w.get('kind') == 'charge'), None)
    if charge is None:
        raise WaypointLookupError(f'map_id={map_id}에 충전 스테이션 웨이포인트가 없습니다')
    try:
        return float(charge['pos_x']), float(charge['pos_y']), float(charge['yaw'])
    except (KeyError, TypeError, ValueError) as exc:
        raise WaypointLookupError(f'충전 웨이포인트 좌표 형식이 올바르지 않습니다: {charge!r}') from exc
