"""app/services/robot_registry.py

로봇 WebSocket 연결과 "마지막으로 알려진 상태"를 한곳에서 관리하는 RobotRegistry.

예전엔 robot_ws.py 모듈 전역 dict 8개로 흩어져 있었다. 한 객체로 묶으면
  1) 어떤 상태가 어디서 바뀌는지 이 클래스만 보면 되고,
  2) 테스트가 모듈 private에 손대지 않고 reset()으로 상태를 정리할 수 있고,
  3) 나중에 저장소를 바꿔야 할 때(예: 프로세스 여러 개) 교체 지점이 하나가 된다.

주의: 상태가 프로세스 메모리에 있으므로 서버는 단일 프로세스로 띄운다는 가정이다.

연결을 downlink/uplink로 나눈 이유: uplink(카메라·위치를 올리기만 하는 연결)가 downlink보다
나중에 재연결되며 명령 수신 자리를 차지하면, 소켓은 살아있어 전송이 "성공"처럼 보이는데
로봇이 메시지를 안 읽어 명령이 조용히 사라졌다(2026-09). 그래서 둘을 분리한다.
"""
import logging
from typing import Dict, List, Optional

from fastapi import WebSocket

from ..core import db
from ..core.errors import SOCKET_SEND_ERRORS

logger = logging.getLogger(__name__)

ROLE_DOWNLINK = "downlink"
ROLE_UPLINK = "uplink"


def robot_exists(robot_code: str) -> bool:
    """DB에 등록(해제되지 않음)된 로봇인지. robot_code는 인증 없이 클라이언트가 지정하는
    값이라, 로봇 WS/오디오 WS 모두 최소한 이것만은 확인한다(blocking DB 호출 — to_thread로 부른다)."""
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT 1 FROM robot WHERE code = %s AND deregistered_at IS NULL", (robot_code,))
        return cur.fetchone() is not None


class RobotRegistry:
    def __init__(self) -> None:
        self._downlinks: Dict[str, WebSocket] = {}
        self._uplinks: Dict[str, WebSocket] = {}
        self._positions: Dict[str, dict] = {}
        self._nav_running: Dict[str, bool] = {}

    # ---- 연결 수명 -------------------------------------------------------------

    def _table(self, role: str) -> Dict[str, WebSocket]:
        # role을 안 보내거나 모르는 값이면 downlink로 취급한다(과거 동작과 호환, 그리고
        # "명령을 받을 수 있는 연결"로 다루는 편이 send_command가 조용히 실패하는 것보다 안전).
        return self._uplinks if role == ROLE_UPLINK else self._downlinks

    def attach(self, role: str, robot_code: str, websocket: WebSocket) -> None:
        # 같은 코드로 재연결하면 이전 소켓을 덮어쓴다(agent 재연결 시 흔한 상황).
        self._table(role)[robot_code] = websocket

    def detach(self, role: str, robot_code: str, websocket: WebSocket) -> None:
        """websocket이 아직 그 자리의 주인일 때만 비운다 — 재연결로 이미 새 소켓이
        들어와 있다면 옛 연결의 종료가 새 연결을 지우면 안 된다."""
        table = self._table(role)
        if table.get(robot_code) is websocket:
            del table[robot_code]

    @staticmethod
    def is_command_channel(role: str) -> bool:
        """"온라인" 배지는 명령이 실제로 전달될 수 있는지(=downlink)를 뜻해야 한다."""
        return role != ROLE_UPLINK

    def is_connected(self, robot_code: str) -> bool:
        return robot_code in self._downlinks

    def connected_codes(self) -> List[str]:
        return list(self._downlinks)

    async def send_command(self, robot_code: str, message: dict) -> bool:
        """downlink로 명령을 보낸다. 연결이 없거나 전송이 끊겨 실패하면 False."""
        websocket: Optional[WebSocket] = self._downlinks.get(robot_code)
        if websocket is None:
            return False
        try:
            await websocket.send_json(message)
        except SOCKET_SEND_ERRORS as exc:
            logger.warning("로봇 %s 명령 전송 실패(%s) — 연결을 정리한다: %s", robot_code, message.get("type"), exc)
            self.detach(ROLE_DOWNLINK, robot_code, websocket)
            return False
        return True

    # ---- 마지막으로 알려진 상태(신규 대시보드 hydration용) -----------------------------

    def remember_position(self, robot_code: str, position: dict) -> None:
        self._positions[robot_code] = position

    def position_of(self, robot_code: str) -> Optional[dict]:
        return self._positions.get(robot_code)

    def positions(self) -> Dict[str, dict]:
        return dict(self._positions)

    def set_nav_running(self, robot_code: str, running: bool) -> None:
        self._nav_running[robot_code] = running

    def nav_running(self, robot_code: str) -> bool:
        return self._nav_running.get(robot_code, False)

    def reset(self) -> None:
        """테스트용 — 모든 상태를 비운다."""
        self._downlinks.clear()
        self._uplinks.clear()
        self._positions.clear()
        self._nav_running.clear()


registry = RobotRegistry()
