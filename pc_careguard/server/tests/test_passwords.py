from app.core import passwords


def test_hash_then_verify_roundtrip():
    hashed = passwords.hash_password("abcd1234")
    assert hashed != "abcd1234"
    assert passwords.verify_password("abcd1234", hashed)
    assert not passwords.verify_password("abcd12345", hashed)


def test_limit_is_counted_in_bytes_not_characters():
    assert passwords.is_within_limit("a" * 72)
    assert not passwords.is_within_limit("a" * 73)
    # 한글은 한 글자가 3바이트 — 24자면 한계, 25자면 넘는다
    assert passwords.is_within_limit("가" * 24)
    assert not passwords.is_within_limit("가" * 25)


def test_verify_returns_false_instead_of_raising_for_overlong_input():
    hashed = passwords.hash_password("abcd1234")
    assert passwords.verify_password("a1" * 40, hashed) is False


def test_verify_returns_false_for_corrupt_hash():
    assert passwords.verify_password("abcd1234", "not-a-bcrypt-hash") is False
