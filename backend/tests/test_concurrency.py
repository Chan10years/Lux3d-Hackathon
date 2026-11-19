"""并发测试：真实 SQLite 事务竞争（多线程独立连接）。

覆盖：并发登记唯一性、短码碰撞重试、同键幂等登记、
并发加入容量争抢、开始对局与加入预留竞争、锁等待跨过期边界。
"""

from __future__ import annotations

import threading
import time
from concurrent.futures import ThreadPoolExecutor

import pytest

from app.db import connect
from app.errors import ApiError, ErrorCode
from app.room_service import RoomService


def _register(service: RoomService, session_id: str, idempotency_key: str = "test-secret-key-0001"):
    return service.register_room(
        provider="ue-session",
        session_id=session_id,
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key=idempotency_key,
    )


def test_concurrent_registrations_get_unique_short_codes(service: RoomService):
    """并发登记唯一性：多线程登记互不相同的会话，短码两两不同。"""
    with ThreadPoolExecutor(max_workers=10) as pool:
        results = list(pool.map(lambda i: _register(service, f"s-{i}"), range(10)))
    codes = [payload["short_code"] for _, payload in results]
    assert len(codes) == 10
    assert len(set(codes)) == 10
    assert all(len(code) == 6 and code.isdigit() for code in codes)


def test_concurrent_same_key_registration_is_idempotent(service: RoomService):
    """并发同键登记：只有一个房间被创建，所有请求得到同一短码。"""
    def register():
        return _register(service, "same-session")

    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(lambda _: register(), range(8)))
    codes = {payload["short_code"] for _, payload in results}
    assert len(codes) == 1
    created = [status for status, _ in results if status == 201]
    idempotent = [status for status, _ in results if status == 200]
    assert len(created) == 1
    assert len(idempotent) == 7
    tokens = {payload["manage_token"] for _, payload in results}
    assert len(tokens) == 1


def test_short_code_collision_is_retried_within_transaction(
    service: RoomService,
):
    """短码碰撞重试：生成器先两次返回已占用短码，第三次返回唯一短码，
    服务必须通过唯一约束感知碰撞并重试成功。"""
    occupied = _register(service, "occupier")[1]["short_code"]
    fresh = "000000" if occupied != "000000" else "999999"
    sequence = [occupied, occupied, fresh]
    calls = {"count": 0}

    def scripted_generator():
        calls["count"] += 1
        return sequence.pop(0)

    service.short_code_fn = scripted_generator
    status, payload = _register(service, "new-session")
    assert status == 201
    assert payload["short_code"] == fresh
    assert payload["short_code"] != occupied
    assert calls["count"] == 3  # 前两次碰撞被事务内重试消化


def test_short_code_allocation_failure_returns_stable_error(
    service: RoomService, monkeypatch
):
    """短码分配失败：生成器永远产生已占用短码时返回明确错误。"""
    occupied = _register(service, "occupier")[1]["short_code"]
    service.short_code_fn = lambda: occupied
    with pytest.raises(ApiError) as exc:
        _register(service, "doomed-session")
    assert exc.value.code == ErrorCode.SHORT_CODE_ALLOCATION_FAILED


def test_concurrent_reserve_never_exceeds_capacity(service: RoomService):
    """并发加入容量争抢：房主占 1 席，8 个并发预留最多成功 3 个。"""
    room = _register(service, "host-session")[1]

    def try_reserve(i: int):
        try:
            res = service.create_reservation(
                room["short_code"],
                player_name=f"p{i}",
                protocol_version="1.0.0",
            )
            return ("ok", res)
        except ApiError as exc:
            return ("error", exc)

    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(try_reserve, range(8)))

    ok = [res for tag, res in results if tag == "ok"]
    errors = [exc for tag, exc in results if tag == "error"]
    assert len(ok) <= 3
    assert len(ok) + len(errors) == 8
    assert all(exc.code == ErrorCode.ROOM_FULL for exc in errors)

    # 全部确认后，已连接人数不得超过容量上限
    for res in ok:
        service.confirm_reservation(res["reservation_id"])
    view = service.get_room(room["short_code"])
    assert view["connected_count"] == len(ok)
    assert view["connected_count"] <= 3
    assert view["host_occupied"] == 1
    assert view["max_players"] == 4


def test_concurrent_start_and_reserve_are_mutually_exclusive(service: RoomService):
    """开始对局与加入预留竞争：串行化后不存在「开始后仍预留成功」的窗口外泄漏。"""
    room = _register(service, "host-session")[1]
    token = room["manage_token"]

    def try_reserve(i: int):
        try:
            res = service.create_reservation(
                room["short_code"],
                player_name=f"p{i}",
                protocol_version="1.0.0",
            )
            return ("ok", res)
        except ApiError as exc:
            return ("error", exc)

    def try_start(_):
        try:
            return ("ok", service.start_match(room["short_code"], token))
        except ApiError as exc:
            return ("error", exc)

    with ThreadPoolExecutor(max_workers=8) as pool:
        reserve_future = pool.map(try_reserve, range(6))
        start_future = pool.map(try_start, range(2))
        reserve_results = list(reserve_future)
        start_results = list(start_future)

    # 两个 start 都成功（幂等），房间进入 match_started
    assert all(tag == "ok" for tag, _ in start_results)
    view = service.get_room(room["short_code"])
    assert view["match_started"] is True

    ok_reserves = [res for tag, res in reserve_results if tag == "ok"]
    error_reserves = [exc for tag, exc in reserve_results if tag == "error"]
    assert len(ok_reserves) <= 3  # 容量仍然被尊重
    assert all(
        exc.code in (ErrorCode.MATCH_ALREADY_STARTED, ErrorCode.ROOM_FULL)
        for exc in error_reserves
    )

    # 开始之后，任何新的加入请求都必须被拒绝
    with pytest.raises(ApiError) as exc:
        service.create_reservation(
            room["short_code"], player_name="late", protocol_version="1.0.0"
        )
    assert exc.value.code == ErrorCode.MATCH_ALREADY_STARTED


def test_renew_rejected_when_expiry_crosses_while_waiting_for_lock(service, clock):
    """锁等待跨过期边界：请求进入时房间有效、取得写锁时已过期，
    续期必须基于锁内时间拒绝，不得复活过期房间。"""
    _, room = service.register_room(
        provider="ue-session",
        session_id="expiry-cross",
        connection="127.0.0.1:7777",
        protocol_version="1.0.0",
        idempotency_key="test-secret-key-0001",
        ttl_seconds=60,
    )

    acquired = threading.Event()
    release = threading.Event()

    def hold_write_lock():
        conn = connect(service.db_path)
        try:
            conn.execute("BEGIN IMMEDIATE")
            acquired.set()
            release.wait(timeout=10)
            conn.execute("COMMIT")
        finally:
            conn.close()

    holder = threading.Thread(target=hold_write_lock)
    holder.start()
    assert acquired.wait(timeout=5), "holder failed to acquire write lock"

    outcome: dict = {}

    def do_renew():
        try:
            outcome["payload"] = service.renew_room(
                room["short_code"], room["manage_token"], ttl_seconds=600
            )
        except ApiError as exc:
            outcome["error"] = exc

    worker = threading.Thread(target=do_renew)
    worker.start()
    time.sleep(0.3)  # 确保 worker 已在等待写锁
    clock.advance(120)  # 等锁期间跨越过期边界（60s TTL）
    release.set()
    holder.join(timeout=5)
    worker.join(timeout=15)

    assert "error" in outcome, "renew should fail after crossing expiry while waiting"
    assert outcome["error"].code == ErrorCode.ROOM_EXPIRED


def test_repeated_requests_are_idempotent(service: RoomService):
    """重复请求幂等：重复登记 / 确认 / 释放 / 开始均收敛到同一状态。"""
    room = _register(service, "sess")[1]
    token = room["manage_token"]

    status_a, again_a = _register(service, "sess")
    status_b, again_b = _register(service, "sess")
    assert (status_a, status_b) == (200, 200)
    assert again_a["short_code"] == again_b["short_code"] == room["short_code"]

    res = service.create_reservation(
        room["short_code"], player_name="p1", protocol_version="1.0.0"
    )
    confirm_a = service.confirm_reservation(res["reservation_id"])
    confirm_b = service.confirm_reservation(res["reservation_id"])
    assert confirm_b["idempotent"] is True
    assert confirm_b["occupancy"]["connected_count"] == 1

    release_a = service.release_reservation(res["reservation_id"])
    release_b = service.release_reservation(res["reservation_id"])
    assert release_b["idempotent"] is True

    start_a = service.start_match(room["short_code"], token)
    start_b = service.start_match(room["short_code"], token)
    assert start_a["already_started"] is False
    assert start_b["already_started"] is True
