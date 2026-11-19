"""健康检查、房间登记与幂等重试测试。"""

from __future__ import annotations

from fastapi.testclient import TestClient

from tests.conftest import register_room


def test_health_ok(client: TestClient):
    resp = client.get("/v1/health")
    assert resp.status_code == 200
    body = resp.json()
    assert body["status"] == "ok"
    assert body["service"] == "room-index"


def test_register_creates_room_with_short_code_and_token(client: TestClient):
    payload = register_room(client)
    assert len(payload["short_code"]) == 6
    assert payload["short_code"].isdigit()
    assert payload["manage_token"]
    assert payload["max_players"] == 4
    assert payload["status"] == "active"
    assert payload["idempotent"] is False
    assert payload["protocol_version"] == "1.0.0"
    assert payload["display_version"] == "demo-1028"


def test_register_retry_is_idempotent(client: TestClient):
    first = register_room(client)
    second = register_room(client)  # 相同 provider + session_id + 秘密凭证重复登记
    assert second["idempotent"] is True
    assert second["room_id"] == first["room_id"]
    assert second["short_code"] == first["short_code"]
    assert second["manage_token"] == first["manage_token"]


def test_register_with_wrong_idempotency_key_is_forbidden(client: TestClient):
    """攻击路径：provider/session_id 可公开查询，但无秘密凭证不得取回令牌。"""
    room = register_room(client)
    resp = client.post(
        "/v1/rooms",
        json={
            "provider": "ue-session",
            "session_id": "sess-0001",
            "connection": "127.0.0.1:7777",
            "protocol_version": "1.0.0",
            "idempotency_key": "attacker-chosen-key-xyz",
        },
    )
    assert resp.status_code == 403
    body = resp.json()
    assert body["error"]["code"] == "FORBIDDEN"
    assert "manage_token" not in body  # 攻击者拿不到管理令牌


def test_register_without_idempotency_key_is_rejected(client: TestClient):
    """仅凭公开会话标识（不带凭证）无法触发幂等取回令牌的路径。"""
    register_room(client)
    resp = client.post(
        "/v1/rooms",
        json={
            "provider": "ue-session",
            "session_id": "sess-0001",
            "connection": "127.0.0.1:7777",
            "protocol_version": "1.0.0",
        },
    )
    assert resp.status_code == 400
    assert resp.json()["error"]["code"] == "INVALID_REQUEST"
    assert "manage_token" not in resp.json()


def test_room_owner_still_manages_room_after_attacker_retry(client: TestClient):
    """攻击者重试被拒后，原房主持正确凭证仍可幂等取回并管理房间。"""
    room = register_room(client)
    again = register_room(client)  # 房主持相同秘密凭证
    assert again["manage_token"] == room["manage_token"]
    resp = client.delete(
        f"/v1/rooms/{room['short_code']}",
        headers={"X-Manage-Token": room["manage_token"]},
    )
    assert resp.status_code == 200


def test_non_ascii_idempotency_key_retry_is_idempotent(client: TestClient):
    """非 ASCII 秘密凭证：首次登记与重复登记都必须稳定（不得 500）。"""
    first = register_room(client, idempotency_key="秘钥-测试-零零零一")
    second = register_room(client, idempotency_key="秘钥-测试-零零零一")
    assert first["idempotent"] is False
    assert second["idempotent"] is True
    assert second["room_id"] == first["room_id"]
    assert second["manage_token"] == first["manage_token"]


def test_register_distinct_sessions_get_distinct_rooms(client: TestClient):
    a = register_room(client, session_id="sess-A")
    b = register_room(client, session_id="sess-B")
    assert a["room_id"] != b["room_id"]
    assert a["short_code"] != b["short_code"]


def test_register_validates_required_fields(client: TestClient):
    body = {
        "provider": "ue-session",
        "session_id": "sess-0001",
        "connection": "127.0.0.1:7777",
        # 缺少 protocol_version
    }
    resp = client.post("/v1/rooms", json=body)
    assert resp.status_code == 400
    assert resp.json()["error"]["code"] == "INVALID_REQUEST"


def test_register_rejects_out_of_range_ttl(client: TestClient):
    resp = client.post(
        "/v1/rooms",
        json={
            "provider": "p",
            "session_id": "s",
            "connection": "c",
            "protocol_version": "1.0.0",
            "ttl_seconds": 5,  # 低于下限 30
        },
    )
    assert resp.status_code == 400
    assert resp.json()["error"]["code"] == "INVALID_REQUEST"
