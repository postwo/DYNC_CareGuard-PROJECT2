"""careguard_turtlebot_agent/errors.py

에이전트 전용 예외. "서버에서 뭔가를 가져오다 실패했다"를 호출부가 `except Exception`으로
뭉뚱그리지 않고 의미 있는 타입으로 구분해 처리하게 한다 — 타입이 좁아야 진짜 버그(KeyError,
AttributeError 등)가 "조회 실패" 로그 뒤에 숨지 않는다.
"""


class AgentError(Exception):
    """에이전트 도메인 예외의 공통 부모."""


class ServerRequestError(AgentError):
    """서버 REST 호출이 네트워크·HTTP·응답 형식 문제로 실패했다."""


class WaypointLookupError(ServerRequestError):
    """충전 스테이션 등 웨이포인트를 서버에서 얻지 못했다."""


class MapDownloadError(ServerRequestError):
    """활성 맵 이미지·메타데이터를 받거나 로컬에 저장하지 못했다."""
