"""app/ws/audio_relay.py

로봇 ↔ 대시보드 실시간 음성 통화용 오디오 바이트 릴레이. robot_ws.py/dashboard_ws.py의
JSON 상태·명령 채널과는 완전히 분리된 별도 WebSocket 4개로 순수 바이너리(Opus로
인코딩된 20ms 프레임)만 주고받는다 — 이 모듈은 디코딩을 전혀 하지 않고 받은 바이트를
그대로 다음 소켓에 넘기기만 한다(서버 CPU 부담을 릴레이 몇 줄 수준으로 유지하기 위함).

robot_code 하나당 필요한 소켓들(로봇 마이크 발신 1개, 그걸 듣는 대시보드 N개, 대시보드
마이크 발신 N개, 그걸 재생하는 로봇 1개)을 AudioRoom 하나에 모아 관리한다 — 로봇 하나가
통화 도중 갖는 상태를 한곳에 묶어두면, 4개의 병렬 dict를 robot_code로 매번 맞춰 찾는
것보다 어디서 뭘 만지는지 보기 쉽다.
"""
import asyncio
import logging
from typing import Dict, Optional, Set

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from ..core.errors import SOCKET_SEND_ERRORS
from ..services.robot_registry import robot_exists

logger = logging.getLogger(__name__)

# 4001: 같은 로봇의 새 연결이 이 자리를 차지해 밀려났다는 뜻(재연결 시 옛 소켓 정리용).
CLOSE_SUPERSEDED = 4001

router = APIRouter()


class AudioRoom:
    """로봇 한 대의 통화 세션이 쓰는 소켓 전부(업링크 발신/수신, 다운링크 발신/수신).

    로봇 쪽 소켓(uplink_sender, downlink_receiver)은 robot_code당 하나만 유지한다 —
    robot_ws.py의 _connections와 같은 이유(같은 코드로 재연결하면 이전 걸 덮어쓴다).
    대시보드 쪽은 같은 로봇을 여러 세션이 동시에 듣거나 말할 수 있어야 하므로 set으로
    관리한다.
    """

    def __init__(self) -> None:
        self.uplink_sender: Optional[WebSocket] = None
        self.uplink_listeners: Set[WebSocket] = set()
        self.downlink_senders: Set[WebSocket] = set()
        self.downlink_receiver: Optional[WebSocket] = None

    async def fanout_uplink(self, data: bytes) -> None:
        """로봇 마이크에서 온 프레임을 이 로봇을 듣고 있는 모든 대시보드에 동시에
        보낸다 — asyncio.gather로 병렬 전송해서, 느린(또는 끊긴) 클라이언트 하나가
        다른 클라이언트로의 전송을 지연시키지 않게 한다."""
        await self._fanout(self.uplink_listeners, data)

    async def relay_downlink(self, data: bytes) -> None:
        """대시보드 마이크에서 온 프레임을 이 로봇의 다운링크 수신 소켓 하나로
        그대로 넘긴다(로봇은 한 대뿐이라 fan-out이 필요 없다)."""
        if self.downlink_receiver is None:
            return
        try:
            await self.downlink_receiver.send_bytes(data)
        except SOCKET_SEND_ERRORS as exc:
            logger.info("로봇 다운링크 소켓이 끊겨 정리한다: %s", exc)
            self.downlink_receiver = None

    async def claim_uplink_sender(self, websocket: WebSocket) -> None:
        await self._replace("uplink_sender", websocket)

    async def claim_downlink_receiver(self, websocket: WebSocket) -> None:
        await self._replace("downlink_receiver", websocket)

    async def _replace(self, slot: str, websocket: WebSocket) -> None:
        """로봇 쪽 소켓 자리를 새 연결로 바꾼다. 옛 소켓을 그냥 덮어쓰면 서버 쪽에 닫히지
        않은 소켓이 남으므로, 밀려난 소켓은 명시적으로 닫는다."""
        previous = getattr(self, slot)
        setattr(self, slot, websocket)
        if previous is not None and previous is not websocket:
            try:
                await previous.close(code=CLOSE_SUPERSEDED)
            except SOCKET_SEND_ERRORS:
                pass  # 이미 끊긴 소켓이면 닫을 것도 없다

    def is_empty(self) -> bool:
        return (
            self.uplink_sender is None
            and self.downlink_receiver is None
            and not self.uplink_listeners
            and not self.downlink_senders
        )

    @staticmethod
    async def _fanout(targets: Set[WebSocket], data: bytes) -> None:
        if not targets:
            return

        async def _send_one(ws: WebSocket) -> None:
            try:
                await ws.send_bytes(data)
            except SOCKET_SEND_ERRORS as exc:
                logger.info("끊긴 오디오 청취 소켓 제거: %s", exc)
                targets.discard(ws)

        await asyncio.gather(*(_send_one(ws) for ws in list(targets)))


class AudioRoomRegistry:
    """robot_code -> AudioRoom. 통화가 한 번도 없었던 로봇은 조회 시점에 새로 만든다.
    소켓이 모두 빠져 비어버린 room은 release()가 치워서 로봇 코드가 늘어도 메모리가 새지 않는다."""

    def __init__(self) -> None:
        self._rooms: Dict[str, AudioRoom] = {}

    def room(self, robot_code: str) -> AudioRoom:
        return self._rooms.setdefault(robot_code, AudioRoom())

    def release(self, robot_code: str) -> None:
        """소켓이 빠진 뒤 호출 — room이 완전히 비었으면 제거한다."""
        room = self._rooms.get(robot_code)
        if room is not None and room.is_empty():
            del self._rooms[robot_code]


_registry = AudioRoomRegistry()


@router.websocket("/ws/robot/{robot_code}/audio/uplink")
async def robot_audio_uplink(websocket: WebSocket, robot_code: str):
    """로봇이 마이크로 캡처한 Opus 프레임을 올려보내는 소켓."""
    if not await asyncio.to_thread(robot_exists, robot_code):
        await websocket.close(code=4404)
        return

    await websocket.accept()
    # accept 뒤에 room을 얻는다 — 그 사이 await가 끼면 다른 연결의 release()가 빈 room을
    # 치워버려 이 소켓이 버려진 room에 등록되는 경합이 생긴다.
    room = _registry.room(robot_code)
    await room.claim_uplink_sender(websocket)
    try:
        while True:
            data = await websocket.receive_bytes()
            await room.fanout_uplink(data)
    except WebSocketDisconnect:
        pass
    finally:
        if room.uplink_sender is websocket:
            room.uplink_sender = None
        _registry.release(robot_code)


@router.websocket("/ws/dashboard/{robot_code}/audio/uplink")
async def dashboard_audio_uplink(websocket: WebSocket, robot_code: str):
    """대시보드가 로봇 마이크 소리를 듣는 소켓. 대시보드는 이 소켓으로 아무것도
    안 보낸다 — 연결 유지 목적의 수신 대기만 한다."""
    await websocket.accept()
    # accept 뒤에 room을 얻는다 — 그 사이 await가 끼면 다른 연결의 release()가 빈 room을
    # 치워버려 이 소켓이 버려진 room에 등록되는 경합이 생긴다.
    room = _registry.room(robot_code)
    room.uplink_listeners.add(websocket)
    try:
        while True:
            await websocket.receive_bytes()
    except WebSocketDisconnect:
        pass
    finally:
        room.uplink_listeners.discard(websocket)
        _registry.release(robot_code)


@router.websocket("/ws/dashboard/{robot_code}/audio/downlink")
async def dashboard_audio_downlink(websocket: WebSocket, robot_code: str):
    """대시보드가 자기 마이크로 캡처한 Opus 프레임을 보내는 소켓."""
    await websocket.accept()
    # accept 뒤에 room을 얻는다 — 그 사이 await가 끼면 다른 연결의 release()가 빈 room을
    # 치워버려 이 소켓이 버려진 room에 등록되는 경합이 생긴다.
    room = _registry.room(robot_code)
    room.downlink_senders.add(websocket)
    try:
        while True:
            data = await websocket.receive_bytes()
            await room.relay_downlink(data)
    except WebSocketDisconnect:
        pass
    finally:
        room.downlink_senders.discard(websocket)
        _registry.release(robot_code)


@router.websocket("/ws/robot/{robot_code}/audio/downlink")
async def robot_audio_downlink(websocket: WebSocket, robot_code: str):
    """로봇이 대시보드 음성을 받아 스피커로 재생하기 위해 여는 소켓. 로봇은 이
    소켓으로 아무것도 안 보낸다 — 연결 유지 목적의 수신 대기만 한다."""
    if not await asyncio.to_thread(robot_exists, robot_code):
        await websocket.close(code=4404)
        return

    await websocket.accept()
    # accept 뒤에 room을 얻는다 — 그 사이 await가 끼면 다른 연결의 release()가 빈 room을
    # 치워버려 이 소켓이 버려진 room에 등록되는 경합이 생긴다.
    room = _registry.room(robot_code)
    await room.claim_downlink_receiver(websocket)
    try:
        while True:
            await websocket.receive_bytes()
    except WebSocketDisconnect:
        pass
    finally:
        if room.downlink_receiver is websocket:
            room.downlink_receiver = None
        _registry.release(robot_code)
