"""查询、续期、撤销生命周期测试。"""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from app.errors import ApiError, ErrorCode
from tests.conftest import owner_headers, register_room


def test_query_room_returns_public_info_without_token(client: TestClient):
    room = register_room(client)
    resp = client.get(f"/v1/rooms/{room['short_code']}")
    assert resp.status_code == 200
    body = resp.json()
    assert body["short_code"] == room["short_code"]
    assert body["connection"] == room["connection"]
    assert body["host_occupied"] == 1
    assert body["reserved_count"] == 0
    assert body["connected_count"] == 0
    assert body["seats_available"] == 3
    assert "manage_token" not in body  # 查询响应绝不泄露令牌


def test_query_unknown_short_code(client: TestClient):
    resp = client.get("/v1/rooms/999999")
    assert resp.status_code == 404
    assert resp.json()["error"]["code"] == "ROOM_NOT_FOUND"


def test_renew_extends_expiry(client: TestClient):
    room = register_room(client)
    resp = client.post(
        f"/v1/rooms/{room['short_code']}/renew",
        json={"ttl_seconds": 1200},
        headers=owner_headers(room["manage_token"]),
    )
    assert resp.status_code == 200
    body = resp.json()
    # 服务端时间决定有效期：续期 = 当前服务端时间 + ttl（重新计时）
    assert body["expires_at_ms"] == room["created_at_ms"] + 1200 * 1000


def test_renew_rejected_after_expiry(client: TestClient, clock):
    room = register_room(client, ttl_seconds=60)
    clock.advance(61)  # 超过有效期
    resp = client.post(
        f"/v1/rooms/{room['short_code']}/renew",
        json={"ttl_seconds": 600},
        headers=owner_headers(room["manage_token"]),
    )
    assert resp.status_code == 410
    assert resp.json()["error"]["code"] == "ROOM_EXPIRED"


def test_renew_rejected_after_revoke(client: TestClient):
    room = register_room(client)
    client.delete(
        f"/v1/rooms/{room['short_code']}", headers=owner_headers(room["manage_token"])
    )
    resp = client.post(
        f"/v1/rooms/{room['short_code']}/renew",
        json={"ttl_seconds": 600},
        headers=owner_headers(room["manage_token"]),
    )
    assert resp.status_code == 410
    assert resp.json()["error"]["code"] == "ROOM_REVOKED"


def test_revoke_requires_token(client: TestClient):
    room = register_room(client)
    resp = client.delete(f"/v1/rooms/{room['short_code']}")
    assert resp.status_code == 403
    assert resp.json()["error"]["code"] == "FORBIDDEN"
    resp = client.delete(
        f"/v1/rooms/{room['short_code']}", headers=owner_headers("wrong-token")
    )
    assert resp.status_code == 403


def test_revoke_then_query_is_revoked(client: TestClient):
    room = register_room(client)
    resp = client.delete(
        f"/v1/rooms/{room['short_code']}", headers=owner_headers(room["manage_token"])
    )
    assert resp.status_code == 200
    assert resp.json()["status"] == "revoked"
    resp = client.get(f"/v1/rooms/{room['short_code']}")
    assert resp.status_code == 410
    assert resp.json()["error"]["code"] == "ROOM_REVOKED"


def test_revoke_is_idempotent_with_valid_token(client: TestClient):
    room = register_room(client)
    headers = owner_headers(room["manage_token"])
    first = client.delete(f"/v1/rooms/{room['short_code']}", headers=headers)
    second = client.delete(f"/v1/rooms/{room['short_code']}", headers=headers)
    assert first.status_code == 200
    assert second.status_code == 200
    assert second.json()["already_revoked"] is True


def test_revoke_replay_returns_original_room_and_new_key_creates_new_room(
    client: TestClient,
):
    """撤销后：旧凭证重试永久返回原房间（终态），新凭证才创建新房间。"""
    room = register_room(client)
    client.delete(
        f"/v1/rooms/{room['short_code']}", headers=owner_headers(room["manage_token"])
    )
    # 旧凭证重放：返回原撤销房间，不创建新房间
    replay = register_room(client, session_id="sess-0001")
    assert replay["idempotent"] is True
    assert replay["room_id"] == room["room_id"]
    assert replay["short_code"] == room["short_code"]
    assert replay["status"] == "revoked"
    assert replay["manage_token"] == room["manage_token"]
    # 主动重新登记（新凭证）：创建新房间，短码不与旧房间重复
    fresh = register_room(
        client, session_id="sess-0001", idempotency_key="new-secret-key-0002"
    )
    assert fresh["room_id"] != room["room_id"]
    assert fresh["short_code"] != room["short_code"]


def test_service_layer_errors_are_stable(service):
    """服务层直接抛出稳定错误码（并发测试复用同一定义）。"""
    with pytest.raises(ApiError) as exc:
        service.get_room("000000")
    assert exc.value.code == ErrorCode.ROOM_NOT_FOUND
