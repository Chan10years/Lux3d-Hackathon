"""测试公共设施：隔离数据库（tmp_path）、可控时钟（FakeClock）、API 客户端。"""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from app.clock import FakeClock
from app.main import create_app
from app.room_service import RoomService

TEST_START_MS = 1_700_000_000_000


@pytest.fixture()
def clock() -> FakeClock:
    return FakeClock(start_ms=TEST_START_MS)


@pytest.fixture()
def db_path(tmp_path):
    return tmp_path / "rooms.db"


@pytest.fixture()
def service(db_path, clock) -> RoomService:
    """直接操作服务层的实例（供并发/碰撞等测试使用）。"""
    return RoomService(db_path=db_path, clock=clock)


@pytest.fixture()
def client(db_path, clock) -> TestClient:
    """API 测试客户端；后台过期清理任务禁用，时间完全由测试控制。"""
    app = create_app(db_path=db_path, clock=clock, sweep_interval_seconds=None)
    with TestClient(app) as test_client:
        yield test_client


def register_room(client: TestClient, **overrides) -> dict:
    """便捷登记函数，返回响应 JSON。"""
    body = {
        "provider": "ue-session",
        "session_id": "sess-0001",
        "connection": "127.0.0.1:7777",
        "protocol_version": "1.0.0",
        "idempotency_key": "test-secret-key-0001",
        "display_version": "demo-1028",
    }
    body.update(overrides)
    resp = client.post("/v1/rooms", json=body)
    assert resp.status_code in (200, 201), resp.text
    return resp.json()


def owner_headers(token: str) -> dict:
    return {"X-Manage-Token": token}
