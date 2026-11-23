"""careguard_server_bridge — 중앙 서버(PC)에서 도는 ROS 2 ↔ FastAPI 브리지.

로봇 agent(uplink_node/downlink_node)가 CAREGUARD_LINK=ros2 로 뜨면 서버로 WebSocket 을 열지
않고 DDS 토픽만 쓴다. 이 노드가 같은 ROS_DOMAIN_ID 에 참여해 그 토픽을 받아 FastAPI 의 기존
/ws/robot 엔드포인트로 그대로 중계하고, 서버 명령은 반대로 /to_robot 토픽으로 내려보낸다.

    로봇(R-05, domain 50)                     중앙 서버 PC
    uplink_node   ─ /careguard/r_05/to_server/uplink   ─▶ ┐
    downlink_node ─ /careguard/r_05/to_server/downlink ─▶ ├ careguard_server_bridge ─ WS ─▶ FastAPI /ws/robot
                  ◀ /careguard/r_05/to_robot           ── ┘     (domain 50 참여)      ◀ WS ─ (명령)
                  ◀ /careguard/r_05/server_heartbeat   ──

로봇마다 ROS_DOMAIN_ID 가 다를 수 있어(R-05/R-06: 50, LIMO R-07: 40 …) 도메인마다 rclpy Context
하나·노드 하나·executor 스레드 하나를 만든다. 로봇 목록과 도메인은 서버 REST(GET /robots 의
code, ros_domain_id)에서 읽고 주기적으로 다시 읽어 새로 등록된 로봇도 자동으로 붙는다.

실행:
    CAREGUARD_SERVER_URL=http://127.0.0.1:8000 ros2 run careguard_server_bridge bridge_node
    ros2 launch careguard_server_bridge bridge.launch.py robot_codes:="['R-05','R-06']"
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import threading
from typing import Dict, List, Optional

import rclpy
from rclpy.context import Context
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from std_msgs.msg import String

from .naming import robot_namespace
from .relay import RobotRelay
from .robot_directory import RobotEntry, fetch_robots, group_by_domain

HEARTBEAT_PERIOD_SEC = 1.0
STATUS_PERIOD_SEC = 2.0


class DomainBridge:
    """ROS_DOMAIN_ID 하나에 참여하는 rclpy Context + Node. 그 도메인 로봇들의 토픽을 맡는다."""

    def __init__(self, domain_id: int, loop: asyncio.AbstractEventLoop, server_ws_url: str,
                 heartbeat_timeout_sec: float) -> None:
        self.domain_id = domain_id
        self._loop = loop
        self._server_ws_url = server_ws_url
        self._heartbeat_timeout_sec = heartbeat_timeout_sec
        self.context = Context()
        rclpy.init(context=self.context, domain_id=domain_id)
        self.node = Node(f"careguard_server_bridge_d{domain_id}", context=self.context)
        self.relays: Dict[str, RobotRelay] = {}
        self._to_robot_pubs: Dict[str, object] = {}
        self._server_hb_pubs: Dict[str, object] = {}
        self._status_pub = self.node.create_publisher(String, "/careguard/bridge/status", 10)
        self.node.create_timer(HEARTBEAT_PERIOD_SEC, self._publish_server_heartbeats)
        self.node.create_timer(STATUS_PERIOD_SEC, self._publish_status)
        self._executor = SingleThreadedExecutor(context=self.context)
        self._executor.add_node(self.node)
        self._thread = threading.Thread(target=self._spin, name=f"ros-domain-{domain_id}", daemon=True)
        self._thread.start()
        self.node.get_logger().info(f"ROS_DOMAIN_ID={domain_id} 참여")

    def _spin(self) -> None:
        try:
            self._executor.spin()
        except Exception:  # noqa: BLE001 - 종료 시 context 무효화로 나는 예외
            pass

    def add_robot(self, robot_code: str) -> None:
        if robot_code in self.relays:
            return
        ns = robot_namespace(robot_code)
        to_robot = self.node.create_publisher(String, f"{ns}/to_robot", 50)
        self._to_robot_pubs[robot_code] = to_robot
        self._server_hb_pubs[robot_code] = self.node.create_publisher(String, f"{ns}/server_heartbeat", 10)

        # 서버 명령 콜백은 asyncio 스레드에서 불린다 — rclpy publish 는 스레드 안전하다.
        relay = RobotRelay(
            self._server_ws_url, robot_code,
            on_command=lambda raw, pub=to_robot: pub.publish(String(data=raw)),
            heartbeat_timeout_sec=self._heartbeat_timeout_sec,
        )
        self.relays[robot_code] = relay
        self.node.create_subscription(String, f"{ns}/link_heartbeat",
                                      lambda msg, r=relay: r.note_heartbeat(msg.data), 10)
        for role in ("uplink", "downlink"):
            self.node.create_subscription(String, f"{ns}/to_server/{role}",
                                          lambda msg, r=relay, ro=role: r.forward(ro, msg.data), 50)
        asyncio.run_coroutine_threadsafe(relay.run(), self._loop)
        self.node.get_logger().info(f"{robot_code} 중계 시작 ({ns}/*)")

    def _publish_server_heartbeats(self) -> None:
        for code, relay in self.relays.items():
            self._server_hb_pubs[code].publish(String(data=json.dumps(relay.server_state())))

    def _publish_status(self) -> None:
        payload = {"domain_id": self.domain_id,
                   "robots": {code: relay.stats() for code, relay in self.relays.items()}}
        self._status_pub.publish(String(data=json.dumps(payload)))

    def shutdown(self) -> None:
        self._executor.shutdown()
        self.node.destroy_node()
        rclpy.shutdown(context=self.context)


class ServerBridge:
    def __init__(self) -> None:
        self.server_http_url = os.environ.get("CAREGUARD_SERVER_URL", "http://127.0.0.1:8000").rstrip("/")
        self.server_ws_url = self.server_http_url.replace("https://", "wss://").replace("http://", "ws://")
        self.default_domain = int(os.environ.get("ROS_DOMAIN_ID", "0"))
        self.robot_codes = [c for c in os.environ.get("CAREGUARD_BRIDGE_ROBOTS", "").split(",") if c.strip()]
        self.heartbeat_timeout_sec = float(os.environ.get("CAREGUARD_BRIDGE_HEARTBEAT_TIMEOUT_SEC", "3.0"))
        self.refresh_sec = float(os.environ.get("CAREGUARD_BRIDGE_REFRESH_SEC", "30.0"))
        self.loop = asyncio.new_event_loop()
        self.domains: Dict[int, DomainBridge] = {}
        self._log = logging.getLogger("careguard_server_bridge")

    def _ensure_robots(self, robots: List[RobotEntry]) -> None:
        for domain_id, codes in group_by_domain(robots, self.default_domain).items():
            bridge = self.domains.get(domain_id)
            if bridge is None:
                bridge = DomainBridge(domain_id, self.loop, self.server_ws_url, self.heartbeat_timeout_sec)
                self.domains[domain_id] = bridge
            for code in codes:
                bridge.add_robot(code)

    async def _refresh_forever(self) -> None:
        while True:
            robots: Optional[List[RobotEntry]] = await asyncio.to_thread(
                fetch_robots, self.server_http_url, self.robot_codes, self._log)
            if robots:
                self._ensure_robots(robots)
            await asyncio.sleep(self.refresh_sec if self.domains else 3.0)

    def run(self) -> None:
        asyncio.set_event_loop(self.loop)
        self._log.info("서버 %s 에 중계 시작 (기본 ROS_DOMAIN_ID=%d)", self.server_http_url, self.default_domain)
        try:
            self.loop.run_until_complete(self._refresh_forever())
        except KeyboardInterrupt:
            pass
        finally:
            for bridge in self.domains.values():
                bridge.shutdown()


def main() -> None:
    logging.basicConfig(level=os.environ.get("CAREGUARD_BRIDGE_LOG_LEVEL", "INFO"),
                        format="%(asctime)s %(levelname)s [%(name)s] %(message)s")
    ServerBridge().run()


if __name__ == "__main__":
    main()
