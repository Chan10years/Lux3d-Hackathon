"""SQLite 存储层：连接管理、Schema、事务。

并发模型：
- 每个操作打开独立连接，写事务使用 BEGIN IMMEDIATE 串行化，
  保证「开始对局与加入预留互斥」「并发登记短码唯一」等不变量。
- WAL 模式 + busy_timeout，降低读写相互阻塞。
"""

from __future__ import annotations

import sqlite3
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

SCHEMA = """
CREATE TABLE IF NOT EXISTS rooms (
    id               TEXT PRIMARY KEY,
    short_code       TEXT NOT NULL UNIQUE,
    provider         TEXT NOT NULL,
    session_id       TEXT NOT NULL,
    connection       TEXT NOT NULL,
    protocol_version TEXT NOT NULL,
    display_version  TEXT,
    max_players      INTEGER NOT NULL DEFAULT 4 CHECK (max_players = 4),
    status           TEXT NOT NULL
                     CHECK (status IN ('active','match_started','revoked','expired')),
    manage_token     TEXT NOT NULL,
    idempotency_key  TEXT NOT NULL,
    created_at_ms    INTEGER NOT NULL,
    expires_at_ms    INTEGER NOT NULL
);

-- 登记幂等键：同一 (provider, session_id) 只允许一个「活着」的房间。
-- 撤销 / 过期后状态离开 active/match_started，键自动释放，可重新登记。
CREATE UNIQUE INDEX IF NOT EXISTS idx_rooms_provider_session_live
    ON rooms(provider, session_id)
    WHERE status IN ('active', 'match_started');

CREATE INDEX IF NOT EXISTS idx_rooms_expires ON rooms(expires_at_ms);

-- 登记历史：同 (provider, session_id, idempotency_key) 永久指向最初创建的房间，
-- 即使房间已撤销/过期，凭证重试也返回原房间，保证登记重试的完整幂等。
CREATE TABLE IF NOT EXISTS registrations (
    provider        TEXT NOT NULL,
    session_id      TEXT NOT NULL,
    idempotency_key TEXT NOT NULL,
    room_id         TEXT NOT NULL REFERENCES rooms(id),
    short_code      TEXT NOT NULL,
    created_at_ms   INTEGER NOT NULL,
    PRIMARY KEY (provider, session_id, idempotency_key)
);

CREATE TABLE IF NOT EXISTS reservations (
    id             TEXT PRIMARY KEY,
    room_id        TEXT NOT NULL REFERENCES rooms(id),
    player_name    TEXT,
    status         TEXT NOT NULL
                   CHECK (status IN ('reserved','confirmed','released','expired')),
    created_at_ms  INTEGER NOT NULL,
    expires_at_ms  INTEGER NOT NULL,
    confirmed_at_ms INTEGER
);

CREATE INDEX IF NOT EXISTS idx_reservations_room ON reservations(room_id, status);
"""


def connect(db_path: Path | str) -> sqlite3.Connection:
    """打开一个独立 SQLite 连接（autocommit，事务显式控制）。"""
    conn = sqlite3.connect(str(db_path), timeout=30.0, isolation_level=None)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    conn.execute("PRAGMA busy_timeout=15000")
    return conn


def init_schema(db_path: Path | str) -> None:
    """创建/升级 Schema（幂等，可在服务启动时反复执行）。"""
    Path(db_path).parent.mkdir(parents=True, exist_ok=True)
    conn = connect(db_path)
    try:
        conn.executescript(SCHEMA)
    finally:
        conn.close()


@contextmanager
def immediate_transaction(db_path: Path | str) -> Iterator[sqlite3.Connection]:
    """BEGIN IMMEDIATE 写事务：拿写锁后执行业务，异常回滚。"""
    conn = connect(db_path)
    try:
        conn.execute("BEGIN IMMEDIATE")
        try:
            yield conn
            conn.execute("COMMIT")
        except BaseException:
            try:
                conn.execute("ROLLBACK")
            except sqlite3.Error:
                pass
            raise
    finally:
        conn.close()


@contextmanager
def read_connection(db_path: Path | str) -> Iterator[sqlite3.Connection]:
    """只读连接（WAL 下不阻塞写）。"""
    conn = connect(db_path)
    try:
        yield conn
    finally:
        conn.close()
