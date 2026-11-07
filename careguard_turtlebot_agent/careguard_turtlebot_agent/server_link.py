"""careguard_turtlebot_agent/server_link.py

서버(FastAPI)와의 WebSocket 연결(재연결 루프 포함)을 관리한다 —
downlink_node.py에 몰려있던 책임을 분리했다(2026-09-15 리팩터링). 수신
메시지는 raw 문자열 그대로 on_message 콜백에 넘기고(파싱은 호출부 책임),
발신은 send()로 큐잉한다.

다운링크(명령 수신)와 업링크(상태 송신)가 같은 클래스를 쓴다 — 예전엔 uplink_node.py가
같은 재연결 루프를 따로 복붙해 갖고 있었다. 둘의 차이는 URL(role)과 재연결 간격뿐이다.
"""
import asyncio
import json
import threading
import traceback
from typing import Callable, Optional

import rclpy
import websockets

RECONNECT_DELAY_SEC = 3.0

# 재연결 대상인 "연결 문제"만 잡는다. 이 밖의 예외(코드 버그)는 루프를 죽여 드러나게 하는 대신,
# 아래 on_message 경계에서만 별도로 기록하고 연결은 유지한다.
CONNECTION_ERRORS = (OSError, websockets.WebSocketException, asyncio.TimeoutError)


class ServerLink:
    def __init__(self, url: str, on_message: Callable[[str], None], logger,
                 role_label: str = '다운링크', reconnect_delay_sec: float = RECONNECT_DELAY_SEC):
        self._url = url
        self._on_message = on_message
        self._logger = logger
        self._role_label = role_label
        self._reconnect_delay_sec = reconnect_delay_sec
        self._ws = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread = threading.Thread(target=self._run_loop, daemon=True)

    @property
    def is_connected(self) -> bool:
        return self._ws is not None

    def start(self):
        self._thread.start()

    def send(self, payload: dict):
        """연결돼 있지 않으면 조용히 버린다(상태 메시지는 다음 주기에 다시 온다)."""
        ws, loop = self._ws, self._loop
        if ws is None or loop is None:
            return
        future = asyncio.run_coroutine_threadsafe(ws.send(json.dumps(payload)), loop)
        future.add_done_callback(self._log_send_failure)

    def _log_send_failure(self, future):
        exc = future.exception()
        if exc is None:
            return
        # 전송 도중 연결이 끊긴 것은 곧 재연결 루프가 처리하므로 경고 수준이면 충분하다.
        self._logger.warn(f'{self._role_label} 메시지 전송 실패: {exc!r}')

    def _run_loop(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._connect_forever())

    async def _connect_forever(self):
        while rclpy.ok():
            try:
                async with websockets.connect(self._url) as ws:
                    self._ws = ws
                    self._logger.info(f'서버 {self._role_label} 연결됨: {self._url}')
                    async for raw in ws:
                        self._dispatch(raw)
            except CONNECTION_ERRORS as exc:
                self._logger.warn(
                    f'{self._role_label} 연결 실패, {self._reconnect_delay_sec}s 후 재시도: {exc}')
            self._ws = None
            await asyncio.sleep(self._reconnect_delay_sec)

    def _dispatch(self, raw: str):
        """메시지 하나를 처리하다 난 예외가 연결 전체를 끊지 않게 하는 경계."""
        try:
            self._on_message(raw)
        except Exception:  # noqa: BLE001 - 메시지 단위 경계, 스택트레이스를 반드시 남긴다
            self._logger.error(f'{self._role_label} 메시지 처리 중 예외:\n{traceback.format_exc()}')
