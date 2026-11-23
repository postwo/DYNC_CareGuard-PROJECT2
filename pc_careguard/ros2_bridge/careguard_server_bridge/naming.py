"""토픽 이름 규칙 — 로봇 쪽 careguard_*_agent/ros2_link.py::robot_namespace 와 반드시 같아야 한다."""
import re


def robot_namespace(robot_code: str) -> str:
    """ROS 토픽 이름에는 '-' 를 쓸 수 없어 영숫자·'_' 외 문자를 '_' 로 바꾸고 소문자로 맞춘다."""
    name = re.sub(r'[^A-Za-z0-9_]', '_', robot_code).lower()
    if not name or name[0].isdigit():
        name = 'r_' + name
    return f'/careguard/{name}'
