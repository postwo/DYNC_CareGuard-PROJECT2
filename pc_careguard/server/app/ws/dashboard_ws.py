import asyncio
import logging
from typing import Optional, Set

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from ..core.errors import SOCKET_SEND_ERRORS

logger = logging.getLogger(__name__)

router = APIRouter()

# 연결된 모든 대시보드 소켓. 로봇 위치·SLAM 진행률 등은 전부 여기로 그대로 뿌리고
# 필터링(어느 로봇 화면을 보고 있는지)은 대시보드 쪽에서 robot_code로 처리한다.
_connections: Set[WebSocket] = set()

# main.py의 startup 이벤트가 채워준다(set_event_loop 참고) — accounts.py처럼 동기(sync)
# FastAPI 경로 함수(스레드풀에서 실행됨)에서 broadcast_threadsafe()로 이 루프에 코루틴을
# 넘길 때 필요하다. robot_ws.py처럼 이미 async 경로에서 부르는 broadcast()는 이 루프가
# 필요 없다(그 요청을 처리 중인 이벤트 루프에서 바로 await하면 되므로).
_event_loop: Optional[asyncio.AbstractEventLoop] = None


def set_event_loop(loop: asyncio.AbstractEventLoop) -> None:
    global _event_loop
    _event_loop = loop


@router.websocket("/ws/dashboard")
async def dashboard_ws_endpoint(websocket: WebSocket):
    await websocket.accept()
    _connections.add(websocket)

    # 방금 접속한 대시보드는 로봇이 마침 안 움직이고 있으면 다음 position이 올 때까지
    # 화면에 위치가 하나도 안 뜬다 — robot_ws.py가 보관해둔 로봇별 마지막 위치로 한 번
    # 채워준다. robot_ws.py가 이 모듈을 import해 broadcast()를 부르므로(순환 import),
    # 모듈 최상단이 아니라 여기서만 지역 import한다.
    from . import robot_ws
    for robot_code, position in robot_ws.latest_positions().items():
        await websocket.send_json({"type": "robot_position", "robot_code": robot_code, **position})

    # 로봇 온라인 배지도 같은 이유로 hydrate한다 — robot_status는 연결/해제 "전환"
    # 시점에만 broadcast되므로, 안 하면 방금 켠 대시보드는 로봇이 이미 오프라인이어도
    # (또는 이미 온라인이어도) 다음 전환이 일어나기 전까지 정확한 상태를 모른다. 이걸
    # 놓치면 "로봇이 실제로 명령을 못 받는 상태인데 화면엔 아무 경고도 안 뜨는" 위험한
    # 상황(teleop 실패가 조용히 무시되는 것과 겹치면 특히)이 생긴다.
    for robot_code in robot_ws.connected_robot_codes():
        await websocket.send_json({"type": "robot_status", "robot_code": robot_code, "online": True})

    # 배터리도 같은 이유로 hydrate — battery_state는 로봇이 보낼 때만 broadcast되므로
    # (2026-09-12 추가), 방금 켠 대시보드는 다음 메시지가 올 때까지 배터리를 전혀 모른다.
    for robot_code, percentage in robot_ws.latest_battery().items():
        await websocket.send_json({"type": "battery_state", "robot_code": robot_code, "percentage": percentage})

    try:
        while True:
            # 대시보드가 서버로 보낼 메시지는 없다 — 연결 유지 목적으로만 수신 대기한다.
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        _connections.discard(websocket)


async def broadcast(message: dict) -> None:
    """연결된 모든 대시보드에 메시지를 보낸다. 끊긴 소켓은 제거한다(전송 오류만 "끊김"으로 본다)."""
    dead = []
    for ws in list(_connections):
        try:
            await ws.send_json(message)
        except SOCKET_SEND_ERRORS as exc:
            logger.info("끊긴 대시보드 소켓 제거: %s", exc)
            dead.append(ws)
    for ws in dead:
        _connections.discard(ws)


def broadcast_threadsafe(message: dict) -> None:
    """accounts.py 등 동기 FastAPI 경로 함수(스레드풀에서 실행)에서 broadcast()를 부르는
    용도. 그 스레드에는 실행 중인 이벤트 루프가 없어 `await broadcast(...)`를 못 쓰므로,
    서버 시작 시 저장해둔 메인 이벤트 루프에 코루틴을 안전하게 넘긴다. 결과를 기다리지
    않는다 — 로그 갱신 알림 하나 늦거나 실패해도(예: 서버 시작 직후라 루프가 아직
    없음) 그 자체가 요청을 실패시킬 이유는 아니다.
    """
    if _event_loop is None:
        return
    try:
        asyncio.run_coroutine_threadsafe(broadcast(message), _event_loop)
    except RuntimeError:
        # 서버 종료 중이라 루프가 이미 닫힌 경우 — 알림 하나 못 보낸다고 이미 커밋된 요청을 실패시키지 않는다.
        logger.warning("대시보드 알림을 보내지 못했습니다(이벤트 루프 종료): %s", message.get("type"))
