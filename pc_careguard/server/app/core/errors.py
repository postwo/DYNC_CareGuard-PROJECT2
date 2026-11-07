"""app/core/errors.py

서버 전역에서 공유하는 예외 분류. "예외를 어떻게 다룰지"를 한곳에서 정해두기 위한 모듈이다.

- SOCKET_SEND_ERRORS: WebSocket 전송이 상대 쪽 사정(연결 끊김·이미 닫힘)으로 실패했을 때
  나는 예외 묶음. 이것만 "정상적인 끊김"으로 보고 로그 + 정리하고, 그 밖의 예외(코드
  버그)는 삼키지 않고 위로 올린다. 예전엔 `except Exception`으로 전부 삼켜서 실제 버그도
  조용히 사라졌다.
- MediaDecodeError: 로봇이 보낸 base64 이미지가 깨져 있을 때.
"""
import binascii

import psycopg2
from fastapi import WebSocketDisconnect

# - WebSocketDisconnect: starlette가 상대의 정상 종료를 알릴 때
# - RuntimeError: 이미 close 메시지를 보낸 소켓에 다시 send할 때(starlette)
# - OSError: uvicorn이 ClientDisconnected(OSError 하위)로 올리는 전송 계층 오류
SOCKET_SEND_ERRORS = (WebSocketDisconnect, RuntimeError, OSError)

# base64.b64decode가 잘못된 입력에 내는 예외(binascii.Error는 ValueError 하위지만 명시한다).
BASE64_DECODE_ERRORS = (binascii.Error, ValueError)

# 캡처 저장은 파일 쓰기(OSError)와 files 행 INSERT(psycopg2.Error) 두 군데서 실패할 수 있다.
STORAGE_ERRORS = (OSError, psycopg2.Error)


class CareGuardError(Exception):
    """도메인 예외의 공통 부모."""


class MediaDecodeError(CareGuardError):
    """로봇이 보낸 미디어 페이로드(base64 등)를 해석할 수 없다."""
