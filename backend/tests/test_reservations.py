"""加入预留、确认、释放、容量与开始对局测试。"""

from __future__ import annotations

from fastapi.testclient import TestClient

from tests.conftest import owner_headers, register_room


def _reserve(client: TestClient, code: str, session: str, **overrides) -> dict:
    body = {"protocol_version": "1.0.0", "player_name": session}
    body.update(overrides)
    return client.post(f"/v1/rooms/{code}/reservations", json=body)


def _confirm(client: TestClient, reservation_id: str):
    return client.post(f"/v1/reservations/{reservation_id}/confirm")


def _release(client: TestClient, reservation_id: str):
    return client.post(f"/v1/reservations/{reservation_id}/release")


def test_reserve_reports_capacity_semantics(client: TestClient):
    room = register_room(client)
    resp = _reserve(client, room["short_code"], "p1")
    assert resp.status_code == 201
    body = resp.json()
    assert body["status"] == "reserved"
    # 房主占 1 席 + 1 个等待连接 = 2/4
    assert body["occupancy"] == {
        "host_occupied": 1,
        "reserved_count": 1,
        "connected_count": 0,
        "max_players": 4,
        "seats_available": 2,
    }
    assert body["expires_at_ms"] > body["created_at_ms"]  # 有期限的预留


def test_reserve_then_confirm_becomes_connected(client: TestClient):
    room = register_room(client)
    res = _reserve(client, room["short_code"], "p1").json()
    resp = _confirm(client, res["reservation_id"])
    assert resp.status_code == 200
    body = resp.json()
    assert body["status"] == "confirmed"
    assert body["occupancy"]["connected_count"] == 1
    assert body["occupancy"]["reserved_count"] == 0
    assert body["occupancy"]["seats_available"] == 2


def test_confirm_is_idempotent(client: TestClient):
    room = register_room(client)
    res = _reserve(client, room["short_code"], "p1").json()
    first = _confirm(client, res["reservation_id"])
    second = _confirm(client, res["reservation_id"])
    assert first.status_code == 200
    assert second.status_code == 200
    assert second.json()["idempotent"] is True
    assert second.json()["occupancy"]["connected_count"] == 1  # 不重复占位


def test_room_full_when_four_seats_taken(client: TestClient):
    room = register_room(client)
    # 房主 1 席 + 3 名成员 confirm 占满
    ids = []
    for i in range(3):
        res = _reserve(client, room["short_code"], f"p{i}").json()
        assert _confirm(client, res["reservation_id"]).status_code == 200
        ids.append(res["reservation_id"])
    resp = _reserve(client, room["short_code"], "late")
    assert resp.status_code == 409
    assert resp.json()["error"]["code"] == "ROOM_FULL"
    assert resp.json()["error"]["details"]["seats_available"] == 0


def test_release_frees_seat_and_is_idempotent(client: TestClient):
    room = register_room(client)
    res = _reserve(client, room["short_code"], "p1").json()
    _confirm(client, res["reservation_id"])
    first = _release(client, res["reservation_id"])
    second = _release(client, res["reservation_id"])
    assert first.status_code == 200 and second.status_code == 200
    assert second.json()["idempotent"] is True
    # 释放后席位归还，可以再次预留
    again = _reserve(client, room["short_code"], "p2")
    assert again.status_code == 201
    assert again.json()["occupancy"]["connected_count"] == 0


def test_expired_reservation_frees_seat_and_blocks_confirm(client: TestClient, clock):
    room = register_room(client)
    res = _reserve(client, room["short_code"], "p1", ttl_seconds=30).json()
    clock.advance(31)  # 预留过期
    resp = _confirm(client, res["reservation_id"])
    assert resp.status_code == 410
    assert resp.json()["error"]["code"] == "RESERVATION_EXPIRED"
    # 过期席位归还他人
    assert _reserve(client, room["short_code"], "p2").status_code == 201


def test_confirm_rejected_after_room_expiry(client: TestClient, clock):
    """房间过期后，其预留不得再确认为已连接（即使预留自身未到期）。"""
    room = register_room(client, ttl_seconds=30)
    res = _reserve(client, room["short_code"], "p1", ttl_seconds=600).json()
    clock.advance(31)  # 房间到期，预留仍"有效"
    resp = _confirm(client, res["reservation_id"])
    assert resp.status_code == 410
    assert resp.json()["error"]["code"] == "ROOM_EXPIRED"


def test_reserve_on_started_room_is_rejected(client: TestClient):
    room = register_room(client)
    resp = client.post(
        f"/v1/rooms/{room['short_code']}/start",
        headers=owner_headers(room["manage_token"]),
    )
    assert resp.status_code == 200
    assert resp.json()["status"] == "match_started"
    late = _reserve(client, room["short_code"], "late")
    assert late.status_code == 409
    assert late.json()["error"]["code"] == "MATCH_ALREADY_STARTED"


def test_existing_reservation_can_confirm_after_start(client: TestClient):
    """开始对局关闭「新加入」，但开始前已获得的预留仍可确认。"""
    room = register_room(client)
    res = _reserve(client, room["short_code"], "early").json()
    client.post(
        f"/v1/rooms/{room['short_code']}/start",
        headers=owner_headers(room["manage_token"]),
    )
    resp = _confirm(client, res["reservation_id"])
    assert resp.status_code == 200
    assert resp.json()["status"] == "confirmed"


def test_start_is_idempotent(client: TestClient):
    room = register_room(client)
    headers = owner_headers(room["manage_token"])
    first = client.post(f"/v1/rooms/{room['short_code']}/start", headers=headers)
    second = client.post(f"/v1/rooms/{room['short_code']}/start", headers=headers)
    assert first.status_code == 200 and second.status_code == 200
    assert second.json()["already_started"] is True


def test_query_reflects_started_state(client: TestClient):
    room = register_room(client)
    res = _reserve(client, room["short_code"], "p1").json()
    _confirm(client, res["reservation_id"])
    client.post(
        f"/v1/rooms/{room['short_code']}/start",
        headers=owner_headers(room["manage_token"]),
    )
    body = client.get(f"/v1/rooms/{room['short_code']}").json()
    assert body["match_started"] is True
    assert body["status"] == "match_started"
    assert body["connected_count"] == 1
