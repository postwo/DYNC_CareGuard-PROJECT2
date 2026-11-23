"""로봇 1대 · 역할 1개(uplink/downlink) 단위의 ROS 2 ↔ FastAPI WebSocket 중계(rclpy 의존 없음).

브리지는 로봇 "대신" FastAPI 의 기존 /ws/robot?robot_code=..&role=.. 엔드포인트에 접속한다.
서버 입장에서는 로봇 agent 가 직접 붙은 것과 구별되지 않으므로 서버 코드·메시지 형식은 그대로다.

연결을 여는 조건은 "로봇의 그 역할 노드가 살아 있다" 하나뿐이다(link_heartbeat 가
heartbeat_timeout_sec 안에 들어왔는가). 로봇이 꺼지면 브리지가 WS 를 닫아 서버가 기존과 똑같이
robot_status(online=False)를 대시보드에 브로드캐스트한다 — 브리지가 대신 붙어 있다고 해서
꺼진 로봇이 "온라인"으로 보이는 일이 없도록 하는 게 이 클래스의 핵심 규칙이다.
"""
from __future__ import annotations

import asyncio
import json
import logging
import time
from typing import Callable, Optional
from urllib.parse import quote

import websockets

logger = logging.getLogger(__name__)

# 서버 robot_ws.py 가 미등록 robot_code 에 쓰는 close code
CLOSE_UNKNOWN_ROBOT = 4404
SUPERVISE_PERIOD_SEC = 0.5
RETRY_DELAY_SEC = 3.0
UNKNOWN_ROBOT_RETRY_SEC = 30.0
SEND_QUEUE_MAX = 200


def _close_code(exc: Exception) -> Optional[int]:
    """websockets 버전 차이 흡수(10+: exc.rcvd.code, 9.x: exc.code)."""
    rcvd = getattr(exc, "rcvd", None)
    if rcvd is not None:
        return getattr(rcvd, "code", None)
    return getattr(exc, "code", None)


def _http_status(exc: Exception) -> Optional[int]:
    """핸드셰이크 거절 상태 코드(InvalidStatus.response.status_code / InvalidStatusCode.status_code)."""
    response = getattr(exc, "response", None)
    if response is not None and hasattr(response, "status_code"):
        return response.status_code
    return getattr(exc, "status_code", None)


def is_json_object(raw: str) -> bool:
    """서버는 receive_json() 으로 받는다 — JSON 객체가 아닌 텍스트가 가면 그 연결이 끊긴다."""
    try:
        return isinstance(json.loads(raw), dict)
    except (TypeError, ValueError):
        return False


class RoleChannel:
    def __init__(self, server_ws_url: str, robot_code: str, role: str,
                 on_server_message: Optional[Callable[[str], None]] = None,
                 heartbeat_timeout_sec: float = 3.0,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self.robot_code = robot_code
        self.role = role
        self.url = (f"{server_ws_url.rstrip('/')}/ws/robot"
                    f"?robot_code={quote(robot_code)}&role={quote(role)}")
        self._on_server_message = on_server_message
        self._timeout = heartbeat_timeout_sec
        self._clock = clock
        self._last_heartbeat: Optional[float] = None
        self._ws = None
        self._queue: Optional[asyncio.Queue] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._next_attempt_at = 0.0
        self.forwarded = 0
        self.dropped = 0
        self.received = 0

    # ---- ROS 스레드에서 호출 -----------------------------------------------------
    def note_heartbeat(self) -> None:
        self._last_heartbeat = self._clock()

    def forward(self, raw: str) -> None:
        """로봇 → 서버 메시지. WS 가 안 열려 있으면 버린다(로봇 쪽 ServerLink.send 와 같은 의미)."""
        if not is_json_object(raw):
            self.dropped += 1
            logger.warning("%s/%s: JSON 객체가 아닌 메시지 버림: %.80r", self.robot_code, self.role, raw)
            return
        loop, q = self._loop, self._queue
        if loop is None or q is None or not self.connected:
            self.dropped += 1
            return
        loop.call_soon_threadsafe(self._enqueue, raw)

    @property
    def connected(self) -> bool:
        return self._ws is not None

    def robot_alive(self) -> bool:
        return self._last_heartbeat is not None and self._clock() - self._last_heartbeat <= self._timeout

    def stats(self) -> dict:
        return {"connected": self.connected, "robot_alive": self.robot_alive(),
                "forwarded": self.forwarded, "received": self.received, "dropped": self.dropped}

    # ---- asyncio 스레드 ----------------------------------------------------------
    def _enqueue(self, raw: str) -> None:
        if self._queue is None:
            return
        if self._queue.qsize() >= SEND_QUEUE_MAX:
            # 와이파이가 느려 쌓이면 오래된 것(대개 카메라 프레임·위치)부터 버린다.
            self._queue.get_nowait()
            self.dropped += 1
        self._queue.put_nowait(raw)

    async def run(self) -> None:
        self._loop = asyncio.get_running_loop()
        self._queue = asyncio.Queue()
        while True:
            if self.robot_alive() and self._clock() >= self._next_attempt_at:
                await self._session()
            await asyncio.sleep(SUPERVISE_PERIOD_SEC)

    async def _session(self) -> None:
        try:
            async with websockets.connect(self.url, max_size=None) as ws:
                self._ws = ws
                self._drain_queue()
                logger.info("%s/%s: 서버 WS 연결", self.robot_code, self.role)
                await self._pump(ws)
        except websockets.ConnectionClosed as exc:
            if _close_code(exc) == CLOSE_UNKNOWN_ROBOT:
                logger.error("%s: 서버에 등록되지 않은 robot_code (대시보드에서 로봇 등록 필요)", self.robot_code)
                self._next_attempt_at = self._clock() + UNKNOWN_ROBOT_RETRY_SEC
            else:
                self._next_attempt_at = self._clock() + RETRY_DELAY_SEC
        except (OSError, websockets.WebSocketException, asyncio.TimeoutError) as exc:
            if _http_status(exc) == 403:
                # Starlette 는 accept 전에 close(4404) 하면 핸드셰이크를 HTTP 403 으로 거절한다.
                logger.error("%s: 서버가 연결을 거절(403) — 대시보드에 등록된 robot_code 인지 확인", self.robot_code)
                self._next_attempt_at = self._clock() + UNKNOWN_ROBOT_RETRY_SEC
                return
            logger.warning("%s/%s: 서버 WS 연결 실패(%s), %.0fs 후 재시도",
                           self.robot_code, self.role, exc, RETRY_DELAY_SEC)
            self._next_attempt_at = self._clock() + RETRY_DELAY_SEC
        finally:
            if self._ws is not None:
                logger.info("%s/%s: 서버 WS 종료", self.robot_code, self.role)
            self._ws = None

    async def _pump(self, ws) -> None:
        sender = asyncio.ensure_future(self._send_loop(ws))
        receiver = asyncio.ensure_future(self._receive_loop(ws))
        watchdog = asyncio.ensure_future(self._watch_robot(ws))
        try:
            done, _ = await asyncio.wait({sender, receiver, watchdog}, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                exc = task.exception()
                if exc is not None:
                    raise exc
        finally:
            for task in (sender, receiver, watchdog):
                task.cancel()
            await ws.close()

    async def _send_loop(self, ws) -> None:
        while True:
            raw = await self._queue.get()
            await ws.send(raw)
            self.forwarded += 1

    async def _receive_loop(self, ws) -> None:
        async for raw in ws:
            self.received += 1
            if self._on_server_message is not None:
                self._on_server_message(raw if isinstance(raw, str) else raw.decode("utf-8", "replace"))

    async def _watch_robot(self, ws) -> None:
        """로봇 하트비트가 끊기면 WS 를 닫는다 → 서버가 오프라인을 브로드캐스트한다."""
        while self.robot_alive():
            await asyncio.sleep(SUPERVISE_PERIOD_SEC)
        logger.warning("%s/%s: 로봇 하트비트 끊김 — 서버 WS 를 닫는다", self.robot_code, self.role)

    def _drain_queue(self) -> None:
        # 연결 전 쌓인 메시지는 버린다(대부분 이미 낡은 위치·카메라 프레임).
        while self._queue is not None and not self._queue.empty():
            self._queue.get_nowait()


class RobotRelay:
    """로봇 1대의 uplink/downlink 채널 묶음."""

    def __init__(self, server_ws_url: str, robot_code: str,
                 on_command: Callable[[str], None],
                 heartbeat_timeout_sec: float = 3.0,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self.robot_code = robot_code
        self.channels = {
            "uplink": RoleChannel(server_ws_url, robot_code, "uplink", None, heartbeat_timeout_sec, clock),
            # 서버 명령은 downlink 연결로만 온다(robot_registry.send_command).
            "downlink": RoleChannel(server_ws_url, robot_code, "downlink", on_command, heartbeat_timeout_sec, clock),
        }

    def note_heartbeat(self, raw: str) -> None:
        try:
            role = json.loads(raw).get("role")
        except (TypeError, ValueError, AttributeError):
            return
        channel = self.channels.get(role)
        if channel is not None:
            channel.note_heartbeat()

    def forward(self, role: str, raw: str) -> None:
        channel = self.channels.get(role)
        if channel is not None:
            channel.forward(raw)

    def server_state(self) -> dict:
        """로봇 쪽 Ros2Link.is_connected 가 보는 값 — 역할별 서버 WS 연결 여부."""
        return {role: channel.connected for role, channel in self.channels.items()}

    def stats(self) -> dict:
        return {role: channel.stats() for role, channel in self.channels.items()}

    async def run(self) -> None:
        await asyncio.gather(*(channel.run() for channel in self.channels.values()))
