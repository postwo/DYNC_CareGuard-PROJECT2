# ROS 2 링크 계약 (로봇 agent ↔ 중앙 서버 브리지)

로봇 agent 가 `CAREGUARD_LINK=ros2` 로 뜨면 서버로 WebSocket 을 열지 않고 아래 DDS 토픽만 쓴다.
중앙 서버 PC 의 `careguard_server_bridge`(pc_careguard/ros2_bridge)가 같은 `ROS_DOMAIN_ID` 에 참여해
이 토픽들을 FastAPI 의 기존 `/ws/robot?robot_code=..&role=..` 로 **그대로** 중계한다.
메시지 본문(JSON)은 WebSocket 때와 1바이트도 다르지 않다 — `robot_ws.py` 의 메시지 표가 그대로 적용된다.

## 토픽 (모두 `std_msgs/String`, 본문은 JSON)

`<ns>` = `robot.code` 를 ROS 이름 규칙에 맞춘 값: 영숫자·`_` 외 문자는 `_`, 소문자. (`R-05` → `r_05`)

| 토픽 | 방향 | 발행 주기 | 본문 |
|---|---|---|---|
| `/careguard/<ns>/to_server/uplink` | 로봇 → 서버 | 이벤트 | uplink_node 가 WS 로 보내던 메시지 (`position`, `battery_state`, `camera_frame`, `slam_scan_update` …) |
| `/careguard/<ns>/to_server/downlink` | 로봇 → 서버 | 이벤트 | downlink_node 가 보내던 메시지 (`nav_status`, `patrol_progress`, `patrol_done`, `medication_done` …) |
| `/careguard/<ns>/to_robot` | 서버 → 로봇 | 이벤트 | 서버 명령 (`patrol_start`, `patrol_stop`, `nav_prepare`, `set_initial_pose` …). downlink 역할만 구독 |
| `/careguard/<ns>/link_heartbeat` | 로봇 → 브리지 | 1 s | `{"role": "uplink"\|"downlink", "robot_code": "R-05"}` |
| `/careguard/<ns>/server_heartbeat` | 브리지 → 로봇 | 1 s | `{"uplink": bool, "downlink": bool}` — 역할별 서버 WS 연결 여부 |
| `/careguard/bridge/status` | 브리지 | 2 s | 도메인별 중계 통계 `{domain_id, robots: {code: {uplink: {...}, downlink: {...}}}}` |

## 연결 규칙

1. 브리지는 **그 역할의 `link_heartbeat` 가 3 초 안에 들어왔을 때만** 서버 WS 를 연다.
   로봇이 꺼지면 WS 가 닫혀 서버가 기존과 똑같이 `robot_status(online=false)` 를 대시보드에 보낸다.
2. 로봇 쪽 `Ros2Link.is_connected` 는 `server_heartbeat[role] == true` 이고 그 하트비트가 3 초 안에 왔을 때만 참이다.
   연결 전 `send()` 는 조용히 버린다(WebSocket 의 `ServerLink.send` 와 같은 의미).
3. JSON 객체가 아닌 본문은 브리지가 버린다(서버 `receive_json()` 이 연결을 끊지 않도록).
4. 서버가 미등록 `robot_code` 로 거절(4404 / HTTP 403)하면 브리지는 30 초 뒤 다시 시도한다.
5. 서버 쪽 송신 큐가 200 개를 넘으면 오래된 메시지부터 버린다(느린 Wi-Fi 에서 카메라 프레임 적체 방지).

## 도메인

로봇마다 `ROS_DOMAIN_ID` 가 다를 수 있다(예: R-05/R-06 = 50, LIMO R-07 = 40).
브리지는 `GET /robots` 의 `ros_domain_id` 로 로봇을 묶어 도메인마다 rclpy `Context` 를 하나씩 만든다.
`ros_domain_id` 가 비어 있는 로봇은 브리지 프로세스의 `ROS_DOMAIN_ID`(기본 0)를 쓴다.

## 바뀌지 않는 것

- REST 호출(웨이포인트·맵 다운로드, 영상·맵 업로드, 임계값 조회)은 그대로 HTTP 다.
- 서버(`pc_careguard/server`) 코드는 바뀌지 않는다. 기본값 `CAREGUARD_LINK=ws` 면 로봇 동작도 기존과 같다.
