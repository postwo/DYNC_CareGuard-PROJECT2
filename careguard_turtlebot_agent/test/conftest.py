"""ROS·Nav2 없이 PatrolController의 상태 기계를 시험하기 위한 스텁.

patrol_controller는 nav2_manager를 import하고, nav2_manager는 rclpy·geometry_msgs·
nav2_simple_commander를 import한다. 이 시험은 Nav2Manager를 가짜 객체로 갈아 끼우므로 진짜 모듈이
없을 때만 빈 스텁으로 채운다(ROS 환경에서는 진짜 모듈이 이미 있어 아무것도 바뀌지 않는다).
"""
import sys
import types


def _stub(name, **attrs):
    try:
        __import__(name)
    except ImportError:
        module = types.ModuleType(name)
        module.__dict__.update(attrs)
        sys.modules[name] = module


class _Any:
    def __init__(self, *args, **kwargs):
        pass


_stub('rclpy')
_stub('rclpy.node', Node=_Any)
_stub('geometry_msgs')
_stub('geometry_msgs.msg', PoseStamped=_Any, PoseWithCovarianceStamped=_Any)
_stub('nav2_simple_commander')
_stub('nav2_simple_commander.robot_navigator', BasicNavigator=_Any, TaskResult=_Any)
