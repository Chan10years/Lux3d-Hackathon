"""持久化恢复与过期清理机制测试。"""

from __future__ import annotations

from fastapi.testclient import TestClient

from app.clock import FakeClock
from app.main import create_app
from app.room_service import RoomService
from tests.conftest import owner_headers, register_room


def test_valid_room_survives_service_restart(db_path, clock):
    """重启后有效房间数据可恢复（SQLite 持久化）。"""
    app_first = create_app(db_path=db_path, clock=clock, sweep_interval_seconds=None)
    with TestClient(app_first) as client_first:
        room = register_room(client_first)
        code = room["short_code"]
        token = room["manage_token"]

    # 模拟进程重启：全新 app 实例 + 新时钟（时间仅小幅前进，房间仍有效）
    clock_restart = FakeClock(start_ms=clock() + 5_000)
    app_second = create_app(
        db_path=db_path, clock=clock_restart, sweep_interval_seconds=None
    )
    with TestClient(app_second) as client_second:
        resp = client_second.get(f"/v1/rooms/{code}")
        assert resp.status_code == 200
        body = resp.json()
        assert body["short_code"] == code
        assert body["connection"] == room["connection"]
        assert body["protocol_version"] == room["protocol_version"]
        # 管理令牌仍可用于房主操作
        renew = client_second.post(
            f"/v1/rooms/{code}/renew",
            json={"ttl_seconds": 600},
            headers=owner_headers(token),
        )
        assert renew.status_code == 200


def test_query_rejects_expired_room_immediately(client: TestClient, clock):
    """服务端时间决定有效期：即使未清理，过期房间也不可查询。"""
    room = register_room(client, ttl_seconds=60)
    clock.advance(61)
    resp = client.get(f"/v1/rooms/{room['short_code']}")
    assert resp.status_code == 410
    assert resp.json()["error"]["code"] == "ROOM_EXPIRED"


def test_sweep_marks_expired_rooms_and_frees_idempotent_key(db_path, clock):
    """过期清理：sweep 落终态、释放幂等键，但短码保留为墓碑。"""
    service = RoomService(db_path=db_path, clock=clock)
    _, room = service.register_room(
        provider="ue-session",
        session_id="sess-0001",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="test-secret-key-0001",
        ttl_seconds=60,
    )
    clock.advance(61)
    result = service.sweep()
    assert result["rooms_expired"] == 1

    # 旧凭证重试：登记历史永久幂等，返回原过期房间
    status, replay = service.register_room(
        provider="ue-session",
        session_id="sess-0001",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="test-secret-key-0001",
    )
    assert status == 200
    assert replay["room_id"] == room["room_id"]
    assert replay["status"] == "expired"

    # 房主主动重开（新凭证）：新建房间，短码不重用
    status2, fresh = service.register_room(
        provider="ue-session",
        session_id="sess-0001",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="test-secret-key-0002",
    )
    assert status2 == 201
    assert fresh["short_code"] != room["short_code"]  # 旧短码不被重用

    # 旧短码查询依旧返回 ROOM_EXPIRED（墓碑防误入）
    from app.errors import ApiError, ErrorCode

    try:
        service.get_room(room["short_code"])
        raise AssertionError("expected ROOM_EXPIRED")
    except ApiError as exc:
        assert exc.code == ErrorCode.ROOM_EXPIRED


def test_sweep_expires_stale_reservations(service, clock):
    """清理机制同时把过期预留落为 expired 终态。"""
    _, room = service.register_room(
        provider="ue-session",
        session_id="sess-0001",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="test-secret-key-0001",
    )
    res = service.create_reservation(
        room["short_code"], player_name="p1", protocol_version="1.0.0", ttl_seconds=30
    )
    clock.advance(31)
    result = service.sweep()
    assert result["reservations_expired"] == 1
    # 过期预留不可确认
    from app.errors import ApiError, ErrorCode

    try:
        service.confirm_reservation(res["reservation_id"])
        raise AssertionError("expected RESERVATION_EXPIRED")
    except ApiError as exc:
        assert exc.code == ErrorCode.RESERVATION_EXPIRED


def test_sweep_spares_active_rooms(service, clock):
    """清理不影响仍有效的房间。"""
    _, room = service.register_room(
        provider="ue-session",
        session_id="sess-0001",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="test-secret-key-0001",
        ttl_seconds=600,
    )
    clock.advance(60)
    result = service.sweep()
    assert result["rooms_expired"] == 0
    view = service.get_room(room["short_code"])
    assert view["status"] == "active"


def test_sweep_expires_started_room_and_frees_idempotent_key(service, clock):
    """开始对局的房间到期后同样被清理，幂等键释放，短码保留为墓碑。"""
    _, room = service.register_room(
        provider="ue-session",
        session_id="sess-start",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="k-start-0001",
        ttl_seconds=60,
    )
    service.start_match(room["short_code"], room["manage_token"])
    clock.advance(61)
    result = service.sweep()
    assert result["rooms_expired"] == 1
    # 旧凭证重试返回原（已开始过期的）房间；新凭证创建新房间
    status, replay = service.register_room(
        provider="ue-session",
        session_id="sess-start",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="k-start-0001",
    )
    assert status == 200
    assert replay["room_id"] == room["room_id"]
    assert replay["status"] == "expired"
    status2, fresh = service.register_room(
        provider="ue-session",
        session_id="sess-start",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="k-start-0002",
    )
    assert status2 == 201
    assert fresh["short_code"] != room["short_code"]  # 旧短码不被重用
    # 旧短码查询返回 ROOM_EXPIRED（墓碑防误入）
    from app.errors import ApiError, ErrorCode

    try:
        service.get_room(room["short_code"])
        raise AssertionError("expected ROOM_EXPIRED")
    except ApiError as exc:
        assert exc.code == ErrorCode.ROOM_EXPIRED


def test_expired_registration_replay_reports_expired_without_sweep(service, clock):
    """sweep 未运行时（如服务重启后首次清理之前），旧凭证重试也必须按
    服务端时间返回 expired 终态，与短码查询语义一致。"""
    _, room = service.register_room(
        provider="ue-session",
        session_id="sess-replay",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="k-replay-0001",
        ttl_seconds=60,
    )
    clock.advance(61)  # 房间到期，但未调用 sweep
    status, replay = service.register_room(
        provider="ue-session",
        session_id="sess-replay",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="k-replay-0001",
    )
    assert status == 200
    assert replay["room_id"] == room["room_id"]
    assert replay["status"] == "expired"
    # 与短码查询语义一致
    from app.errors import ApiError, ErrorCode

    try:
        service.get_room(room["short_code"])
        raise AssertionError("expected ROOM_EXPIRED")
    except ApiError as exc:
        assert exc.code == ErrorCode.ROOM_EXPIRED


def test_register_lazy_expires_started_room_without_sweep(service, clock):
    """即使 sweep 未运行，登记也会把过期的 match_started 房间转为 expired，
    同会话新凭证可立即创建新房间。"""
    _, room = service.register_room(
        provider="ue-session",
        session_id="sess-lazy",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="k-lazy-0001",
        ttl_seconds=60,
    )
    service.start_match(room["short_code"], room["manage_token"])
    clock.advance(61)
    status, fresh = service.register_room(
        provider="ue-session",
        session_id="sess-lazy",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="k-lazy-0002",
    )
    assert status == 201
    assert fresh["short_code"] != room["short_code"]
