"""계정 관리 API(app/core/accounts.py) + 로그인(POST /auth/login) 테스트.

계정은 실제 DB에 만들고, 테스트가 끝나면 이 테스트가 만든 계정·대상자·감사 로그를 지운다.
"""
import uuid

import bcrypt
import pytest

from app.core import db

GOOD_PASSWORD = "abcd1234"


def _unique(prefix="t"):
    return f"{prefix}-{uuid.uuid4().hex[:8]}"


def _query(sql, params=()):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(sql, params)
        return cur.fetchall()


def _execute(sql, params=()):
    with db.get_connection() as conn, conn.cursor() as cur:
        cur.execute(sql, params)
        conn.commit()


def _action_types(user_id):
    rows = _query("SELECT action_type FROM user_action_log WHERE user_id = %s ORDER BY id", (user_id,))
    return [r["action_type"] for r in rows]


@pytest.fixture
def accounts(client):
    """create(...) -> 등록 응답 JSON. 끝나면 만든 계정과 연결된 행을 모두 지운다."""
    created_ids = []
    recipient_ids = []

    class Factory:
        def create(self, role_code="operator", password=GOOD_PASSWORD, is_active=True, display_name="테스트 계정"):
            login_id = _unique("acc")
            resp = client.post("/accounts", json={
                "login_id": login_id, "password": password, "display_name": display_name,
                "role_code": role_code, "is_active": is_active,
            })
            assert resp.status_code == 201, resp.text
            body = resp.json()
            body["password"] = password
            created_ids.append(body["id"])
            return body

    factory = Factory()

    def make_recipient():
        with db.get_connection() as conn, conn.cursor() as cur:
            cur.execute("INSERT INTO recipient (code, name) VALUES (%s, %s) RETURNING id",
                        (_unique("rc")[:16], "테스트 대상자"))
            rid = cur.fetchone()["id"]
            conn.commit()
        recipient_ids.append(rid)
        return rid

    factory.recipient = make_recipient
    yield factory

    if recipient_ids:
        _execute("DELETE FROM recipient WHERE id = ANY(%s)", (recipient_ids,))
    if created_ids:
        _execute("DELETE FROM user_action_log WHERE user_id = ANY(%s)", (created_ids,))
        _execute("DELETE FROM careguard_user WHERE id = ANY(%s)", (created_ids,))


def _operator_of(recipient_id):
    return _query("SELECT operator_id FROM recipient WHERE id = %s", (recipient_id,))[0]["operator_id"]


def _login(client, account, password=None):
    return client.post("/auth/login", json={
        "login_id": account["login_id"], "password": password or account["password"],
    })


# ---- 등록 -------------------------------------------------------------------------

def test_register_returns_account_and_stores_only_a_hash(client, accounts):
    account = accounts.create(role_code="admin", display_name="관리자 A")
    assert account["role_code"] == "admin"
    assert account["display_name"] == "관리자 A"
    assert account["is_active"] is True

    stored = _query("SELECT password_hash FROM careguard_user WHERE id = %s", (account["id"],))[0]["password_hash"]
    assert stored != GOOD_PASSWORD
    assert bcrypt.checkpw(GOOD_PASSWORD.encode(), stored.encode())


@pytest.mark.parametrize("bad_password", ["abc1234", "abcdefgh", "12345678", ""])
def test_register_rejects_weak_passwords(client, bad_password):
    resp = client.post("/accounts", json={
        "login_id": _unique("weak"), "password": bad_password, "display_name": "x", "role_code": "operator",
    })
    assert resp.status_code == 400
    assert "비밀번호" in resp.json()["detail"]


def test_register_rejects_duplicate_login_id(client, accounts):
    account = accounts.create()
    resp = client.post("/accounts", json={
        "login_id": account["login_id"], "password": GOOD_PASSWORD, "display_name": "중복", "role_code": "operator",
    })
    assert resp.status_code == 409


def test_register_rejects_unknown_role(client):
    resp = client.post("/accounts", json={
        "login_id": _unique("role"), "password": GOOD_PASSWORD, "display_name": "x", "role_code": "superuser",
    })
    assert resp.status_code == 400


# ---- 로그인 -----------------------------------------------------------------------

def test_login_succeeds_with_correct_password(client, accounts):
    account = accounts.create(role_code="guardian")
    resp = _login(client, account)
    assert resp.status_code == 200
    assert resp.json() == {"id": account["id"], "login_id": account["login_id"], "role": "guardian"}
    assert _query("SELECT last_login_at FROM careguard_user WHERE id = %s", (account["id"],))[0]["last_login_at"]


def test_login_fails_with_wrong_password(client, accounts):
    account = accounts.create()
    assert _login(client, account, password="wrong1234").status_code == 401


def test_login_fails_for_unknown_user(client):
    resp = client.post("/auth/login", json={"login_id": _unique("ghost"), "password": GOOD_PASSWORD})
    assert resp.status_code == 401


def test_login_fails_for_inactive_account(client, accounts):
    account = accounts.create(is_active=False)
    assert _login(client, account).status_code == 401


def test_login_error_does_not_reveal_whether_user_exists(client, accounts):
    account = accounts.create()
    wrong_password = _login(client, account, password="wrong1234").json()
    unknown_user = client.post("/auth/login", json={"login_id": _unique("ghost"), "password": "x"}).json()
    assert wrong_password == unknown_user


# ---- 목록 -------------------------------------------------------------------------

def test_list_includes_account_with_charge_summary(client, accounts):
    account = accounts.create()
    recipient_id = accounts.recipient()
    client.put(f"/accounts/{account['id']}/recipients", json={"recipient_ids": [recipient_id]})

    listed = {a["id"]: a for a in client.get("/accounts").json()}
    assert account["id"] in listed
    assert "테스트 대상자" in listed[account["id"]]["charge_summary"]


# ---- 수정 -------------------------------------------------------------------------

def test_update_changes_display_name_and_role_and_logs_it(client, accounts):
    account = accounts.create(role_code="operator")
    resp = client.patch(f"/accounts/{account['id']}", json={
        "display_name": "바뀐 이름", "role_code": "admin", "is_active": True,
    })
    assert resp.status_code == 200
    assert resp.json()["display_name"] == "바뀐 이름"
    assert resp.json()["role_code"] == "admin"
    assert _action_types(account["id"]) == ["account_update"]


def test_update_unknown_account_returns_404(client):
    resp = client.patch("/accounts/2147483000", json={"display_name": "x", "role_code": "admin", "is_active": True})
    assert resp.status_code == 404


def test_update_unknown_role_returns_400(client, accounts):
    account = accounts.create()
    resp = client.patch(f"/accounts/{account['id']}", json={
        "display_name": "x", "role_code": "superuser", "is_active": True,
    })
    assert resp.status_code == 400


def test_deactivating_unassigns_recipients_and_blocks_login(client, accounts):
    account = accounts.create()
    recipient_id = accounts.recipient()
    client.put(f"/accounts/{account['id']}/recipients", json={"recipient_ids": [recipient_id]})
    assert _operator_of(recipient_id) == account["id"]

    resp = client.patch(f"/accounts/{account['id']}", json={
        "display_name": account["display_name"], "role_code": "operator", "is_active": False,
    })
    assert resp.status_code == 200
    assert _operator_of(recipient_id) is None
    assert "account_deactivate" in _action_types(account["id"])
    assert _login(client, account).status_code == 401


def test_reactivating_logs_activate_and_allows_login(client, accounts):
    account = accounts.create(is_active=False)
    client.patch(f"/accounts/{account['id']}", json={
        "display_name": account["display_name"], "role_code": "operator", "is_active": True,
    })
    assert _action_types(account["id"]) == ["account_activate"]
    assert _login(client, account).status_code == 200


# ---- 비밀번호 변경 ------------------------------------------------------------------

def test_change_password_switches_credentials_and_logs_it(client, accounts):
    account = accounts.create()
    resp = client.patch(f"/accounts/{account['id']}/password", json={"new_password": "newpass99"})
    assert resp.status_code == 204
    assert _login(client, account, password="newpass99").status_code == 200
    assert _login(client, account).status_code == 401  # 옛 비밀번호는 더 이상 안 된다
    assert _action_types(account["id"]) == ["password_change"]


@pytest.mark.parametrize("bad_password", ["short1", "onlyletters", "12345678"])
def test_change_password_enforces_policy(client, accounts, bad_password):
    account = accounts.create()
    resp = client.patch(f"/accounts/{account['id']}/password", json={"new_password": bad_password})
    assert resp.status_code == 400
    assert _login(client, account).status_code == 200  # 실패한 변경은 기존 비밀번호를 건드리지 않는다


def test_change_password_unknown_account_returns_404(client):
    resp = client.patch("/accounts/2147483000/password", json={"new_password": "newpass99"})
    assert resp.status_code == 404


# ---- 담당 대상자 배정 -----------------------------------------------------------------

def test_assign_recipients_replaces_previous_list(client, accounts):
    account = accounts.create()
    first, second = accounts.recipient(), accounts.recipient()

    assert client.put(f"/accounts/{account['id']}/recipients", json={"recipient_ids": [first]}).status_code == 204
    assert _operator_of(first) == account["id"]

    client.put(f"/accounts/{account['id']}/recipients", json={"recipient_ids": [second]})
    assert _operator_of(first) is None
    assert _operator_of(second) == account["id"]


def test_assign_recipients_takes_over_from_another_account(client, accounts):
    previous, current = accounts.create(), accounts.create()
    recipient_id = accounts.recipient()
    client.put(f"/accounts/{previous['id']}/recipients", json={"recipient_ids": [recipient_id]})
    client.put(f"/accounts/{current['id']}/recipients", json={"recipient_ids": [recipient_id]})
    assert _operator_of(recipient_id) == current["id"]


def test_assign_recipients_empty_list_clears_assignments(client, accounts):
    account = accounts.create()
    recipient_id = accounts.recipient()
    client.put(f"/accounts/{account['id']}/recipients", json={"recipient_ids": [recipient_id]})
    client.put(f"/accounts/{account['id']}/recipients", json={"recipient_ids": []})
    assert _operator_of(recipient_id) is None


def test_assign_recipients_unknown_account_returns_404(client):
    assert client.put("/accounts/2147483000/recipients", json={"recipient_ids": []}).status_code == 404


# ---- 삭제 -------------------------------------------------------------------------

def test_delete_soft_deletes_unassigns_and_blocks_login(client, accounts):
    account = accounts.create()
    recipient_id = accounts.recipient()
    client.put(f"/accounts/{account['id']}/recipients", json={"recipient_ids": [recipient_id]})

    assert client.post("/accounts/delete", json={"user_ids": [account["id"]]}).status_code == 204

    assert account["id"] not in {a["id"] for a in client.get("/accounts").json()}
    row = _query("SELECT deleted_at, is_active FROM careguard_user WHERE id = %s", (account["id"],))[0]
    assert row["deleted_at"] is not None and row["is_active"] is False  # 행은 남는다(soft delete)
    assert _operator_of(recipient_id) is None
    assert "account_delete" in _action_types(account["id"])
    assert _login(client, account).status_code == 401


def test_delete_with_empty_list_is_noop(client):
    assert client.post("/accounts/delete", json={"user_ids": []}).status_code == 204


def test_delete_same_account_twice_logs_once(client, accounts):
    account = accounts.create()
    client.post("/accounts/delete", json={"user_ids": [account["id"]]})
    client.post("/accounts/delete", json={"user_ids": [account["id"]]})
    assert _action_types(account["id"]).count("account_delete") == 1


# ---- 입력 한계(예전엔 DB·bcrypt 오류가 처리 안 된 채 500으로 터졌다) ------------------------------

def test_register_rejects_password_over_bcrypt_limit(client):
    for password in ("a1" * 40, "가나다라" * 6 + "a1"):  # 80바이트 / 한글이라 바이트가 72를 넘는 경우
        resp = client.post("/accounts", json={
            "login_id": _unique("long"), "password": password, "display_name": "x", "role_code": "operator",
        })
        assert resp.status_code == 400, resp.text
        assert "너무 깁니다" in resp.json()["detail"]


def test_change_password_rejects_password_over_bcrypt_limit(client, accounts):
    account = accounts.create()
    resp = client.patch(f"/accounts/{account['id']}/password", json={"new_password": "a1" * 40})
    assert resp.status_code == 400
    assert _login(client, account).status_code == 200  # 기존 비밀번호 그대로


def test_login_with_overlong_password_is_a_plain_401(client, accounts):
    account = accounts.create()
    assert _login(client, account, password="a1" * 40).status_code == 401


@pytest.mark.parametrize("field, value", [
    ("login_id", "u" * 65), ("login_id", ""), ("display_name", "n" * 65), ("display_name", ""),
])
def test_register_rejects_out_of_range_text_fields(client, field, value):
    body = {"login_id": _unique("fld"), "password": GOOD_PASSWORD, "display_name": "x", "role_code": "operator"}
    body[field] = value
    assert client.post("/accounts", json=body).status_code == 422


def test_update_rejects_too_long_display_name(client, accounts):
    account = accounts.create()
    resp = client.patch(f"/accounts/{account['id']}", json={
        "display_name": "n" * 65, "role_code": "operator", "is_active": True,
    })
    assert resp.status_code == 422
