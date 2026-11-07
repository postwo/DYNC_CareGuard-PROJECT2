"""app/core/passwords.py

비밀번호 해시·검증을 한곳에서 한다. bcrypt는 비밀번호가 72바이트를 넘으면 ValueError를 던지는데,
예전엔 계정 등록·비밀번호 변경·로그인이 각자 bcrypt를 직접 불러서 긴 비밀번호 하나로 500이 났다
(한글은 한 글자가 3바이트라 24자만 넘어도 걸린다).
"""
import bcrypt

MAX_PASSWORD_BYTES = 72


def is_within_limit(password: str) -> bool:
    return len(password.encode()) <= MAX_PASSWORD_BYTES


def hash_password(password: str) -> str:
    """호출 전에 is_within_limit()로 길이를 확인해야 한다(넘으면 ValueError)."""
    return bcrypt.hashpw(password.encode(), bcrypt.gensalt()).decode()


def verify_password(password: str, password_hash: str) -> bool:
    """일치하면 True. 72바이트를 넘는 입력이나 깨진 해시는 예외 대신 False — 로그인 실패로 다룬다."""
    if not is_within_limit(password):
        return False
    try:
        return bcrypt.checkpw(password.encode(), password_hash.encode())
    except ValueError:
        return False
