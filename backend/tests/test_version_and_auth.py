"""协议版本检查与管理令牌鉴权测试。"""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from app.errors import ApiError, ErrorCode
from tests.conftest import owner_headers, register_room


def test_join_with_matching_protocol_version_succeeds(client: TestClient):
    room = register_room(client, protocol_version="1.2.0")
    resp = client.post(
        f"/v1/rooms/{room['short_code']}/reservations",
        json={"protocol_version": "1.2.0", "player_name": "p1"},
    )
    assert resp.status_code == 201


def test_join_with_incompatible_protocol_version_fails(client: TestClient):
    room = register_room(client, protocol_version="1.0.0")
    resp = client.post(
        f"/v1/rooms/{room['short_code']}/reservations",
        json={"protocol_version": "2.0.0", "player_name": "p1"},
    )
    assert resp.status_code == 409
    body = resp.json()["error"]
    assert body["code"] == "VERSION_MISMATCH"
    assert body["details"]["room_protocol_version"] == "1.0.0"
    assert body["details"]["client_protocol_version"] == "2.0.0"


def test_display_version_is_not_part_of_compatibility_check(client: TestClient):
    """协议兼容版本与普通展示版本分离：display_version 不参与校验。"""
    room = register_room(
        client, protocol_version="1.0.0", display_version="hotfix-7"
    )
    resp = client.post(
        f"/v1/rooms/{room['short_code']}/reservations",
        json={"protocol_version": "1.0.0"},
    )
    assert resp.status_code == 201
    view = client.get(f"/v1/rooms/{room['short_code']}").json()
    assert view["protocol_version"] == "1.0.0"
    assert view["display_version"] == "hotfix-7"


def test_query_can_optionally_check_protocol_version(client: TestClient):
    room = register_room(client, protocol_version="1.0.0")
    ok = client.get(
        f"/v1/rooms/{room['short_code']}?protocol_version=1.0.0"
    )
    mismatch = client.get(
        f"/v1/rooms/{room['short_code']}?protocol_version=9.9.9"
    )
    plain = client.get(f"/v1/rooms/{room['short_code']}")
    assert ok.status_code == 200
    assert mismatch.status_code == 409
    assert mismatch.json()["error"]["code"] == "VERSION_MISMATCH"
    assert plain.status_code == 200  # 不传参则不校验


def test_renew_without_or_with_wrong_token_is_forbidden(client: TestClient):
    room = register_room(client)
    missing = client.post(
        f"/v1/rooms/{room['short_code']}/renew", json={"ttl_seconds": 600}
    )
    wrong = client.post(
        f"/v1/rooms/{room['short_code']}/renew",
        json={"ttl_seconds": 600},
        headers=owner_headers("not-the-token"),
    )
    assert missing.status_code == 403
    assert missing.json()["error"]["code"] == "FORBIDDEN"
    assert wrong.status_code == 403


def test_start_requires_manage_token(client: TestClient):
    room = register_room(client)
    missing = client.post(f"/v1/rooms/{room['short_code']}/start")
    wrong = client.post(
        f"/v1/rooms/{room['short_code']}/start",
        headers=owner_headers("bogus"),
    )
    assert missing.status_code == 403
    assert wrong.status_code == 403
    view = client.get(f"/v1/rooms/{room['short_code']}").json()
    assert view["match_started"] is False  # 未授权修改未生效


def test_service_token_comparison_handles_any_unicode(service):
    """令牌比较以 UTF-8 字节进行：任意 Unicode 输入在 renew/revoke/start
    上都稳定返回 FORBIDDEN，绝不抛 TypeError 导致 500。

    （HTTP 层无法传输非 ASCII header——客户端与协议层均拒绝——因此该
    场景只能在服务层直接验证。）"""
    _, room = service.register_room(
        provider="ue-session",
        session_id="sess-unicode",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="test-secret-key-0001",
    )
    code = room["short_code"]
    bogus_tokens = ("令牌-非法-测试", "café-token-ü", "token With Spaces", "")
    for bogus in bogus_tokens:
        with pytest.raises(ApiError) as renew_exc:
            service.renew_room(code, bogus, ttl_seconds=600)
        assert renew_exc.value.code == ErrorCode.FORBIDDEN
        with pytest.raises(ApiError) as revoke_exc:
            service.revoke_room(code, bogus)
        assert revoke_exc.value.code == ErrorCode.FORBIDDEN
        with pytest.raises(ApiError) as start_exc:
            service.start_match(code, bogus)
        assert start_exc.value.code == ErrorCode.FORBIDDEN
    # 全部拒绝后房间未被修改
    view = service.get_room(code)
    assert view["status"] == "active"
    assert view["match_started"] is False


def test_query_response_never_contains_manage_token(client: TestClient):
    room = register_room(client)
    res = client.post(
        f"/v1/rooms/{room['short_code']}/reservations",
        json={"protocol_version": "1.0.0"},
    ).json()
    view = client.get(f"/v1/rooms/{room['short_code']}").json()
    confirm = client.post(f"/v1/reservations/{res['reservation_id']}/confirm").json()
    assert "manage_token" not in view
    assert "manage_token" not in confirm
    assert "manage_token" not in res
