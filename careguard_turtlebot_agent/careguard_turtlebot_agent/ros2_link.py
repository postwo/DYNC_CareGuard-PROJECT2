"""careguard_turtlebot_agent/ros2_link.py

로봇 ↔ 중앙 서버 통신을 WebSocket 대신 ROS 2(DDS) 토픽으로 하는 링크.
server_link.ServerLink 와 같은 인터페이스(start / send / is_connected, on_message(raw))라서
uplink_node·downlink_node 는 link_factory.create_server_link() 로 받기만 하면 된다.

CAREGUARD_LINK=ros2 일 때만 쓰인다(기본값 ws — 기존 동작 그대로).
서버 쪽 상대편은 pc_careguard/ros2_bridge(careguard_server_bridge 노드)다. 브리지가 같은
ROS_DOMAIN_ID 에 참여해 이 토픽들을 FastAPI 의 기존 /ws/robot 엔드포인트로 그대로 중계하므로
서버 코드와 메시지 형식은 하나도 바뀌지 않는다.

토픽 계약(shared/api_contract/ros2_link.md 와 동일, std_msgs/String JSON):
  /careguard/<ns>/to_server/<role>   로봇 → 서버  (role = uplink | downlink)
  /careguard/<ns>/to_robot           서버 → 로봇  (downlink 역할만 구독)
  /careguard/<ns>/link_heartbeat     로봇 → 브리지 {"role": ..., "robot_code": ...} 1초 주기
  /careguard/<ns>/server_heartbeat   브리지 → 로봇 {"uplink": bool, "downlink": bool} 1초 주기
  <ns> = robot.code 를 ROS 이름 규칙에 맞게 바꾼 값(R-05 → r_05)

브리지는 link_heartbeat 가 살아 있을 때만 그 역할의 WebSocket 을 서버에 열어 둔다 — 로봇이
꺼지면 3초 안에 WS 가 닫혀 대시보드의 "온라인" 배지가 기존과 똑같이 꺼진다.
"""
import json
import queue
import re
import threading
import time
import traceback
from typing import Callable

from std_msgs.msg import String

HEARTBEAT_PERIOD_SEC = 1.0
SERVER_HEARTBEAT_TIMEOUT_SEC = 3.0
ROLES = ('uplink', 'downlink')


def robot_namespace(robot_code: str) -> str:
    """ROS 토픽 이름에는 '-' 를 쓸 수 없어 영숫자·'_' 외 문자를 '_' 로 바꾸고 소문자로 맞춘다."""
    name = re.sub(r'[^A-Za-z0-9_]', '_', robot_code).lower()
    if not name or name[0].isdigit():
        name = 'r_' + name
    return f'/careguard/{name}'


class Ros2Link:
    def __init__(self, node, robot_code: str, role: str, on_message: Callable[[str], None], logger,
                 role_label: str = '', clock: Callable[[], float] = time.monotonic):
        if role not in ROLES:
            raise ValueError(f'role 은 {ROLES} 중 하나여야 합니다: {role!r}')
        self._node = node
        self._robot_code = robot_code
        self._role = role
        self._on_message = on_message
        self._logger = logger
        self._role_label = role_label or role
        self._clock = clock
        self._server_state: dict = {}
        self._server_seen_at = None
        self._was_connected = False
        # 서버 명령 처리는 별도 스레드에서 순서대로 한다 — WS 모드에서도 명령 콜백은
        # asyncio 스레드에서 돌아 rclpy executor 를 막지 않았다(patrol_start 처럼 오래 걸리는
        # 핸들러가 Nav2 콜백을 굶기지 않도록 같은 성질을 유지한다).
        self._inbox: 'queue.Queue[str]' = queue.Queue()
        self._worker = threading.Thread(target=self._drain_inbox, daemon=True)

        ns = robot_namespace(robot_code)
        self._out_pub = node.create_publisher(String, f'{ns}/to_server/{role}', 50)
        self._heartbeat_pub = node.create_publisher(String, f'{ns}/link_heartbeat', 10)
        node.create_subscription(String, f'{ns}/server_heartbeat', self._on_server_heartbeat, 10)
        if role == 'downlink':
            node.create_subscription(String, f'{ns}/to_robot', self._on_to_robot, 50)
        node.create_timer(HEARTBEAT_PERIOD_SEC, self._beat)

    # ---- ServerLink 와 같은 인터페이스 -------------------------------------------
    @property
    def is_connected(self) -> bool:
        if self._server_seen_at is None:
            return False
        if self._clock() - self._server_seen_at > SERVER_HEARTBEAT_TIMEOUT_SEC:
            return False
        return bool(self._server_state.get(self._role))

    def start(self):
        self._worker.start()
        self._logger.info(f'ROS 2 {self._role_label} 링크 시작: {robot_namespace(self._robot_code)}')
        self._beat()

    def send(self, payload: dict):
        """서버 쪽 WS 가 안 열려 있으면 조용히 버린다(ServerLink 와 동일 — 상태는 다음 주기에 다시 온다)."""
        if not self.is_connected:
            return
        self._out_pub.publish(String(data=json.dumps(payload)))

    # ---- 내부 ----------------------------------------------------------------
    def _beat(self):
        self._heartbeat_pub.publish(String(data=json.dumps({'role': self._role, 'robot_code': self._robot_code})))
        connected = self.is_connected
        if connected != self._was_connected:
            self._was_connected = connected
            if connected:
                self._logger.info(f'서버 {self._role_label} 연결됨(ROS 2 브리지)')
            else:
                self._logger.warn(f'서버 {self._role_label} 연결 끊김(ROS 2 브리지 응답 없음)')

    def _on_server_heartbeat(self, msg: String):
        try:
            state = json.loads(msg.data)
        except ValueError:
            return
        if isinstance(state, dict):
            self._server_state = state
            self._server_seen_at = self._clock()

    def _on_to_robot(self, msg: String):
        self._inbox.put(msg.data)

    def _drain_inbox(self):
        while True:
            raw = self._inbox.get()
            try:
                self._on_message(raw)
            except Exception:  # noqa: BLE001 - 메시지 단위 경계(ServerLink._dispatch 와 동일)
                self._logger.error(f'{self._role_label} 메시지 처리 중 예외:\n{traceback.format_exc()}')
