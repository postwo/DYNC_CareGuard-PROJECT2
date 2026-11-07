"""ROS 없이 audio 노드의 순수 로직을 시험하기 위한 스텁.

rclpy·std_msgs는 노드 클래스를 정의할 때만 필요하고, 여기서 시험하는 세션 클래스들은 쓰지 않는다.
실제 ROS 환경(colcon test)에서는 진짜 모듈이 이미 있으므로 이 스텁은 아무것도 바꾸지 않는다.
"""
import sys
import types


def _stub(name, **attrs):
    if name in sys.modules:
        return
    try:
        __import__(name)
    except ImportError:
        module = types.ModuleType(name)
        module.__dict__.update(attrs)
        sys.modules[name] = module


class _Node:
    def __init__(self, *args, **kwargs):
        pass


_stub('rclpy')
_stub('rclpy.node', Node=_Node)
_stub('std_msgs')
_stub('std_msgs.msg', Bool=type('Bool', (), {}))
