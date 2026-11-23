# 중앙 서버 PC에서 ROS 2 브리지를 띄운다. FastAPI(uvicorn)가 먼저 떠 있어야 한다.
#
#   ros2 launch careguard_server_bridge bridge.launch.py
#   ros2 launch careguard_server_bridge bridge.launch.py server_url:=http://127.0.0.1:8000 robots:=R-05,R-06
#
# robots 를 비우면 서버에 등록된 로봇 전부를 중계한다(GET /robots, 30초마다 갱신).
# 로봇별 ROS_DOMAIN_ID 는 로봇 등록 시 입력한 값(robot.ros_domain_id)을 쓰고, 비어 있으면
# 이 프로세스의 ROS_DOMAIN_ID(기본 0)를 쓴다.
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument('server_url', default_value='http://127.0.0.1:8000'),
        DeclareLaunchArgument('robots', default_value='', description='쉼표 구분 robot.code, 비우면 전체'),
        DeclareLaunchArgument('heartbeat_timeout_sec', default_value='3.0'),
        Node(
            package='careguard_server_bridge',
            executable='bridge_node',
            output='screen',
            additional_env={
                'CAREGUARD_SERVER_URL': LaunchConfiguration('server_url'),
                'CAREGUARD_BRIDGE_ROBOTS': LaunchConfiguration('robots'),
                'CAREGUARD_BRIDGE_HEARTBEAT_TIMEOUT_SEC': LaunchConfiguration('heartbeat_timeout_sec'),
            },
        ),
    ])
