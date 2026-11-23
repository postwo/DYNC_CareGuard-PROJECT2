from careguard_server_bridge.naming import robot_namespace
from careguard_server_bridge.robot_directory import RobotEntry, group_by_domain, parse_robots


def test_namespace_is_valid_ros_name():
    assert robot_namespace("R-05") == "/careguard/r_05"
    assert robot_namespace("05") == "/careguard/r_05"


def test_namespace_matches_robot_side():
    # 로봇 쪽 구현과 글자 하나라도 다르면 토픽이 안 만나므로 두 파일을 직접 비교한다.
    root = __import__("pathlib").Path(__file__).resolve().parents[3]
    import importlib.util
    for path in [root / "careguard_turtlebot_agent/careguard_turtlebot_agent/ros2_link.py",
                 root / "robot_careguard_ws/src/careguard_agent/careguard_agent/ros2_link.py"]:
        source = path.read_text(encoding="utf-8")
        start = source.index("def robot_namespace")
        end = source.index("return f'/careguard/{name}'", start)
        namespace = {"re": __import__("re")}
        exec(source[start:end] + "return f'/careguard/{name}'\n", namespace)  # noqa: S102 - 시험 전용
        for code in ["R-05", "R-07", "limo-1", "05"]:
            assert namespace["robot_namespace"](code) == robot_namespace(code), (path, code)
    assert importlib.util  # noqa


def test_parse_and_group():
    payload = [
        {"code": "R-05", "ros_domain_id": 50},
        {"code": "R-06", "ros_domain_id": 50},
        {"code": "R-07", "ros_domain_id": 40},
        {"code": "R-09", "ros_domain_id": None},
        {"name": "no code"},
    ]
    robots = parse_robots(payload)
    assert [r.code for r in robots] == ["R-05", "R-06", "R-07", "R-09"]
    assert group_by_domain(robots, 0) == {50: ["R-05", "R-06"], 40: ["R-07"], 0: ["R-09"]}
    assert parse_robots(payload, ["R-07"]) == [RobotEntry("R-07", 40)]
    assert parse_robots({"detail": "x"}) == []
