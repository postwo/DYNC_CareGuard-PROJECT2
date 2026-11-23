"""서버 링크 선택: CAREGUARD_LINK=ws(기본, 기존 WebSocket) | ros2(중앙 서버 ROS 2 브리지 경유).

uplink_node·downlink_node 는 이 함수 하나로 링크를 받는다 — 어느 쪽이든 start/send/is_connected
인터페이스가 같아서 노드 코드는 전송 방식을 몰라도 된다.
"""
from . import ws_config


def create_server_link(node, role: str, on_message, logger, role_label: str,
                       reconnect_delay_sec: float = None):
    if ws_config.LINK_MODE == 'ros2':
        from .ros2_link import Ros2Link
        return Ros2Link(node, ws_config.ROBOT_CODE, role, on_message, logger, role_label=role_label)

    from .server_link import RECONNECT_DELAY_SEC, ServerLink
    return ServerLink(
        url=f'{ws_config.SERVER_WS_URL}/ws/robot?robot_code={ws_config.ROBOT_CODE}&role={role}',
        on_message=on_message,
        logger=logger,
        role_label=role_label,
        reconnect_delay_sec=RECONNECT_DELAY_SEC if reconnect_delay_sec is None else reconnect_delay_sec)
