import time


def wait_until(predicate, timeout=3.0, interval=0.02):
    """predicate가 truthy를 돌려줄 때까지 폴링한다.

    로봇 WS 핸들러는 DB 작업을 스레드로 빼서(asyncio.to_thread) 이벤트 루프를 막지 않는다.
    그래서 `with websocket_connect()` 블록을 빠져나오는 순간 처리가 끝났다고 가정할 수 없다 —
    TestClient는 블록을 나갈 때 앱 태스크를 취소하므로, 결과 확인은 반드시 블록 "안"에서 한다.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(interval)
    raise AssertionError("조건이 제한 시간 안에 충족되지 않았다")


def settle(seconds=0.3):
    """"아무 일도 일어나지 않아야 한다"를 검증할 때, 처리할 시간을 준다."""
    time.sleep(seconds)
