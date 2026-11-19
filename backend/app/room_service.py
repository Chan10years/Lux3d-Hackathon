"""房间索引核心业务服务。

所有写操作在 BEGIN IMMEDIATE 事务内完成，保证：
- 短码唯一（UNIQUE 约束 + 事务内碰撞重试）；
- 登记 (provider, session_id) 幂等；
- 加入预留不突破四人容量（房主占 1 席）；
- 开始对局与加入预留事务互斥；
- 服务端时间唯一决定有效期，续期不复活过期/撤销房间。

语义约定（与 docs/API.md 保持一致）：
- 房主占位（host_occupied）：房主固定占用 1 席；
- 等待连接（reserved_count）：已创建且未过期的 reserved 预留数；
- 已连接人数（connected_count）：已 confirm 的预留数；
- 占用 = host_occupied + reserved_count + connected_count <= max_players(4)。
"""

from __future__ import annotations

import secrets
import sqlite3
import uuid
from typing import Any, Callable, Dict, Optional

from .clock import system_clock_ms
from .config import (
    DEFAULT_RESERVATION_TTL_SECONDS,
    DEFAULT_ROOM_TTL_SECONDS,
    MAX_PLAYERS,
)
from .db import immediate_transaction, init_schema, read_connection
from .errors import ApiError, ErrorCode
from .short_codes import new_short_code

_MAX_SHORT_CODE_ATTEMPTS = 8

LIVE_STATUSES = ("active", "match_started")


class RoomService:
    """线程安全：每个方法独立开连接，可多线程并发调用。"""

    def __init__(
        self,
        db_path,
        clock: Optional[Callable[[], int]] = None,
        short_code_fn: Optional[Callable[[], str]] = None,
        max_short_code_attempts: int = _MAX_SHORT_CODE_ATTEMPTS,
    ) -> None:
        self.db_path = db_path
        self.clock = clock or system_clock_ms
        self.short_code_fn = short_code_fn or new_short_code
        self.max_short_code_attempts = max(1, int(max_short_code_attempts))
        init_schema(self.db_path)

    # ------------------------------------------------------------------
    # 房间登记（幂等）
    # ------------------------------------------------------------------
    def register_room(
        self,
        *,
        provider: str,
        session_id: str,
        connection: str,
        protocol_version: str,
        idempotency_key: str,
        display_version: Optional[str] = None,
        ttl_seconds: int = DEFAULT_ROOM_TTL_SECONDS,
        now_ms: Optional[int] = None,
    ) -> tuple[int, Dict[str, Any]]:
        """登记房间。

        返回 (http_status, payload)：201 新建；200 幂等重试（仅当
        idempotency_key 与既有活房间一致时，返回同一房间、同一短码与
        同一管理令牌；凭证不符一律 FORBIDDEN，杜绝凭公开会话标识取回令牌）。

        服务端时间在取得事务锁之后读取，避免锁等待跨越过期边界时用
        陈旧时间做出错误判定；now_ms 参数仅供测试注入可控时间。
        """
        room_id = uuid.uuid4().hex
        manage_token = secrets.token_urlsafe(24)

        with immediate_transaction(self.db_path) as conn:
            # 取得写锁后才读取服务端时间（锁等待期间时间可能跨越过期边界）
            now = now_ms if now_ms is not None else self.clock()
            expires_at = now + int(ttl_seconds) * 1000
            for _ in range(self.max_short_code_attempts):
                # 1) 登记历史：同 (provider, session, key) 永久幂等，即使原房间
                #    已撤销/过期，也返回原房间及其当前终态（含原令牌）。
                prior = self._find_registration(
                    conn,
                    provider=provider,
                    session_id=session_id,
                    idempotency_key=idempotency_key,
                )
                if prior is not None:
                    row = self._fetch_room_by_id(conn, prior["room_id"])
                    if (
                        row["status"] in ("active", "match_started")
                        and row["expires_at_ms"] <= now
                    ):
                        # 到期未清理（如重启后首次 sweep 之前）：按服务端时间
                        # 落为 expired 终态，保证与短码查询语义一致
                        conn.execute(
                            "UPDATE rooms SET status = 'expired' WHERE id = ?",
                            (row["id"],),
                        )
                        row = self._fetch_room_by_id(conn, prior["room_id"])
                    return 200, self._registered_payload(row, idempotent=True)

                # 2) 活房间守卫：无历史凭证时，不允许凭公开会话标识取回他人
                #    令牌，也不允许并发同键创建第二个活房间。
                existing = self._find_live_room(
                    conn, provider=provider, session_id=session_id
                )
                if existing is not None:
                    if existing["expires_at_ms"] <= now:
                        # 活幂等键但已过期（active 或 match_started）：
                        # 就地落为 expired，释放键后继续新建
                        conn.execute(
                            "UPDATE rooms SET status = 'expired' WHERE id = ?",
                            (existing["id"],),
                        )
                    elif secrets.compare_digest(
                        str(existing["idempotency_key"]).encode("utf-8"),
                        str(idempotency_key).encode("utf-8"),
                    ):
                        # 防御分支：历史缺失但凭证匹配（如旧版本数据）→
                        # 补记历史并幂等返回
                        self._record_registration(
                            conn,
                            room=existing,
                            provider=provider,
                            session_id=session_id,
                            idempotency_key=idempotency_key,
                            created_at_ms=existing["created_at_ms"],
                        )
                        return 200, self._registered_payload(existing, idempotent=True)
                    else:
                        raise ApiError(
                            ErrorCode.FORBIDDEN,
                            "idempotency key does not match the existing live room",
                        )

                code = self.short_code_fn()
                try:
                    conn.execute(
                        """
                        INSERT INTO rooms (
                            id, short_code, provider, session_id, connection,
                            protocol_version, display_version, max_players,
                            status, manage_token, idempotency_key,
                            created_at_ms, expires_at_ms
                        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'active', ?, ?, ?, ?)
                        """,
                        (
                            room_id,
                            code,
                            provider,
                            session_id,
                            connection,
                            protocol_version,
                            display_version,
                            MAX_PLAYERS,
                            manage_token,
                            idempotency_key,
                            now,
                            expires_at,
                        ),
                    )
                except sqlite3.IntegrityError as exc:
                    msg = str(exc)
                    if "rooms.short_code" in msg:
                        continue  # 短码碰撞：事务内重试新短码
                    if "rooms.provider" in msg:
                        continue  # 并发同键登记：回到循环头重查幂等
                    raise
                row = conn.execute(
                    "SELECT * FROM rooms WHERE id = ?", (room_id,)
                ).fetchone()
                self._record_registration(
                    conn,
                    room=row,
                    provider=provider,
                    session_id=session_id,
                    idempotency_key=idempotency_key,
                    created_at_ms=now,
                )
                return 201, self._registered_payload(row, idempotent=False)

        raise ApiError(
            ErrorCode.SHORT_CODE_ALLOCATION_FAILED,
            "failed to allocate a unique short code after retries",
        )

    # ------------------------------------------------------------------
    # 查询
    # ------------------------------------------------------------------
    def get_room(
        self,
        short_code: str,
        *,
        client_protocol_version: Optional[str] = None,
        now_ms: Optional[int] = None,
    ) -> Dict[str, Any]:
        """按短码查询房间公开信息；响应永不包含管理令牌。"""
        with read_connection(self.db_path) as conn:
            now = now_ms if now_ms is not None else self.clock()
            room = self._fetch_room(conn, short_code)
            self._assert_usable(room, now)
            if client_protocol_version is not None:
                self._assert_protocol(room, client_protocol_version)
            occupancy = self._compute_occupancy(conn, room, now)
            return {
                "room_id": room["id"],
                "short_code": room["short_code"],
                "status": room["status"],
                "provider": room["provider"],
                "session_id": room["session_id"],
                "connection": room["connection"],
                "protocol_version": room["protocol_version"],
                "display_version": room["display_version"],
                "max_players": room["max_players"],
                "host_occupied": occupancy["host_occupied"],
                "reserved_count": occupancy["reserved_count"],
                "connected_count": occupancy["connected_count"],
                "seats_available": occupancy["seats_available"],
                "match_started": room["status"] == "match_started",
                "created_at_ms": room["created_at_ms"],
                "expires_at_ms": room["expires_at_ms"],
            }

    # ------------------------------------------------------------------
    # 续期（不复活过期/撤销房间）
    # ------------------------------------------------------------------
    def renew_room(
        self,
        short_code: str,
        manage_token: str,
        *,
        ttl_seconds: int = DEFAULT_ROOM_TTL_SECONDS,
        now_ms: Optional[int] = None,
    ) -> Dict[str, Any]:
        with immediate_transaction(self.db_path) as conn:
            # 取得写锁后才读取服务端时间，锁等待不导致过期房间被复活
            now = now_ms if now_ms is not None else self.clock()
            room = self._fetch_room(conn, short_code)
            if room is None:
                raise ApiError(ErrorCode.ROOM_NOT_FOUND, "room short code not found")
            self._require_token(room, manage_token)
            self._assert_usable(room, now)  # 撤销/过期一律拒绝，绝不复活
            new_expires = now + int(ttl_seconds) * 1000
            conn.execute(
                "UPDATE rooms SET expires_at_ms = ? WHERE id = ?",
                (new_expires, room["id"]),
            )
            occupancy = self._compute_occupancy(conn, room, now)
            return {
                "room_id": room["id"],
                "short_code": room["short_code"],
                "status": room["status"],
                "expires_at_ms": new_expires,
                "occupancy": occupancy,
            }

    # ------------------------------------------------------------------
    # 撤销（幂等；短码保留为墓碑，不会立即重新分配）
    # ------------------------------------------------------------------
    def revoke_room(
        self,
        short_code: str,
        manage_token: str,
        *,
        now_ms: Optional[int] = None,
    ) -> Dict[str, Any]:
        # 撤销不依赖时间（过期/有效房间均可撤销为终态）；now_ms 仅为接口一致性保留
        del now_ms
        with immediate_transaction(self.db_path) as conn:
            room = self._fetch_room(conn, short_code)
            if room is None:
                raise ApiError(ErrorCode.ROOM_NOT_FOUND, "room short code not found")
            self._require_token(room, manage_token)
            if room["status"] == "revoked":
                return {
                    "room_id": room["id"],
                    "short_code": room["short_code"],
                    "status": "revoked",
                    "already_revoked": True,
                }
            conn.execute(
                "UPDATE rooms SET status = 'revoked' WHERE id = ?", (room["id"],)
            )
            return {
                "room_id": room["id"],
                "short_code": room["short_code"],
                "status": "revoked",
                "already_revoked": False,
            }

    # ------------------------------------------------------------------
    # 加入预留（与开始对局事务互斥；受四人容量约束）
    # ------------------------------------------------------------------
    def create_reservation(
        self,
        short_code: str,
        *,
        player_name: Optional[str] = None,
        protocol_version: str,
        ttl_seconds: int = DEFAULT_RESERVATION_TTL_SECONDS,
        now_ms: Optional[int] = None,
    ) -> Dict[str, Any]:
        reservation_id = uuid.uuid4().hex
        with immediate_transaction(self.db_path) as conn:
            # 取得写锁后才读取服务端时间，保证容量/过期判定基于锁内时刻
            now = now_ms if now_ms is not None else self.clock()
            expires_at = now + int(ttl_seconds) * 1000
            room = self._fetch_room(conn, short_code)
            if room is None:
                raise ApiError(ErrorCode.ROOM_NOT_FOUND, "room short code not found")
            self._assert_usable(room, now)
            if room["status"] == "match_started":
                raise ApiError(
                    ErrorCode.MATCH_ALREADY_STARTED,
                    "match already started; joining is closed",
                )
            self._assert_protocol(room, protocol_version)
            self._expire_stale_reservations(conn, room["id"], now)
            occupancy = self._compute_occupancy(conn, room, now)
            if occupancy["seats_available"] <= 0:
                raise ApiError(
                    ErrorCode.ROOM_FULL,
                    "room capacity reached",
                    details={"max_players": room["max_players"], **occupancy},
                )
            conn.execute(
                """
                INSERT INTO reservations (
                    id, room_id, player_name, status, created_at_ms, expires_at_ms
                ) VALUES (?, ?, ?, 'reserved', ?, ?)
                """,
                (reservation_id, room["id"], player_name, now, expires_at),
            )
            occupancy = self._compute_occupancy(conn, room, now)
            return {
                "reservation_id": reservation_id,
                "room_id": room["id"],
                "short_code": room["short_code"],
                "player_name": player_name,
                "status": "reserved",
                "created_at_ms": now,
                "expires_at_ms": expires_at,
                "occupancy": occupancy,
            }

    # ------------------------------------------------------------------
    # 预留确认（幂等）
    # ------------------------------------------------------------------
    def confirm_reservation(
        self, reservation_id: str, *, now_ms: Optional[int] = None
    ) -> Dict[str, Any]:
        with immediate_transaction(self.db_path) as conn:
            # 取得写锁后才读取服务端时间
            now = now_ms if now_ms is not None else self.clock()
            res = self._fetch_reservation(conn, reservation_id)
            room = self._fetch_room_by_id(conn, res["room_id"])
            # 确认前校验房间状态：缺失/撤销/过期（含时间过期）一律拒绝，
            # 过期房间的预留不得转为已连接；match_started 放行已有预留。
            self._assert_usable(room, now)
            if res["status"] == "confirmed":
                occupancy = self._compute_occupancy(conn, room, now)
                return self._reservation_payload(res, "confirmed", occupancy, idempotent=True)
            if res["status"] == "released":
                raise ApiError(
                    ErrorCode.RESERVATION_ALREADY_RELEASED,
                    "reservation was released and cannot be confirmed",
                )
            if res["status"] == "expired" or res["expires_at_ms"] <= now:
                if res["status"] == "reserved":
                    conn.execute(
                        "UPDATE reservations SET status = 'expired' WHERE id = ?",
                        (res["id"],),
                    )
                raise ApiError(
                    ErrorCode.RESERVATION_EXPIRED,
                    "reservation expired before confirmation",
                )
            conn.execute(
                "UPDATE reservations SET status = 'confirmed', confirmed_at_ms = ? "
                "WHERE id = ?",
                (now, res["id"]),
            )
            row = self._fetch_reservation(conn, reservation_id)
            occupancy = self._compute_occupancy(conn, room, now)
            return self._reservation_payload(row, "confirmed", occupancy, idempotent=False)

    # ------------------------------------------------------------------
    # 预留释放（幂等）
    # ------------------------------------------------------------------
    def release_reservation(
        self, reservation_id: str, *, now_ms: Optional[int] = None
    ) -> Dict[str, Any]:
        with immediate_transaction(self.db_path) as conn:
            # 取得写锁后才读取服务端时间
            now = now_ms if now_ms is not None else self.clock()
            res = self._fetch_reservation(conn, reservation_id)
            room = self._fetch_room_by_id(conn, res["room_id"])
            if res["status"] == "released":
                return self._reservation_payload(res, "released", None, idempotent=True)
            if res["status"] == "expired" or (
                res["status"] == "reserved" and res["expires_at_ms"] <= now
            ):
                conn.execute(
                    "UPDATE reservations SET status = 'expired' WHERE id = ?",
                    (res["id"],),
                )
                # 释放过期预留是幂等成功，客户端可安全重试
                fresh = self._fetch_reservation(conn, reservation_id)
                return self._reservation_payload(fresh, "released", None, idempotent=True)
            if res["status"] == "confirmed":
                occupancy_before = self._compute_occupancy(conn, room, now)
                conn.execute(
                    "UPDATE reservations SET status = 'released' WHERE id = ?",
                    (res["id"],),
                )
                occupancy = self._compute_occupancy(conn, room, now)
                payload = self._reservation_payload(
                    self._fetch_reservation(conn, reservation_id),
                    "released",
                    occupancy,
                    idempotent=False,
                )
                payload["freed_connected_seat"] = occupancy_before["connected_count"]
                return payload
            conn.execute(
                "UPDATE reservations SET status = 'released' WHERE id = ?", (res["id"],)
            )
            fresh = self._fetch_reservation(conn, reservation_id)
            occupancy = self._compute_occupancy(conn, room, now)
            return self._reservation_payload(fresh, "released", occupancy, idempotent=False)

    # ------------------------------------------------------------------
    # 开始对局（幂等；关闭加入；与加入预留事务互斥）
    # ------------------------------------------------------------------
    def start_match(
        self,
        short_code: str,
        manage_token: str,
        *,
        now_ms: Optional[int] = None,
    ) -> Dict[str, Any]:
        with immediate_transaction(self.db_path) as conn:
            # 取得写锁后才读取服务端时间（与加入预留互斥的关键判定均在锁内）
            now = now_ms if now_ms is not None else self.clock()
            room = self._fetch_room(conn, short_code)
            if room is None:
                raise ApiError(ErrorCode.ROOM_NOT_FOUND, "room short code not found")
            self._require_token(room, manage_token)
            self._assert_usable(room, now)
            if room["status"] == "match_started":
                occupancy = self._compute_occupancy(conn, room, now)
                payload = {
                    "room_id": room["id"],
                    "short_code": room["short_code"],
                    "status": "match_started",
                    "already_started": True,
                    "occupancy": occupancy,
                }
                return payload
            conn.execute(
                "UPDATE rooms SET status = 'match_started' WHERE id = ?", (room["id"],)
            )
            occupancy = self._compute_occupancy(conn, room, now)
            return {
                "room_id": room["id"],
                "short_code": room["short_code"],
                "status": "match_started",
                "already_started": False,
                "occupancy": occupancy,
            }

    # ------------------------------------------------------------------
    # 过期清理（把过期房间/预留落为终态；短码行保留作墓碑防止误入）
    # ------------------------------------------------------------------
    def sweep(self, *, now_ms: Optional[int] = None) -> Dict[str, int]:
        with immediate_transaction(self.db_path) as conn:
            now = now_ms if now_ms is not None else self.clock()
            cur_rooms = conn.execute(
                "UPDATE rooms SET status = 'expired' "
                "WHERE status IN ('active', 'match_started') AND expires_at_ms < ?",
                (now,),
            )
            cur_res = conn.execute(
                "UPDATE reservations SET status = 'expired' "
                "WHERE status = 'reserved' AND expires_at_ms < ?",
                (now,),
            )
            return {
                "rooms_expired": cur_rooms.rowcount,
                "reservations_expired": cur_res.rowcount,
            }

    # ------------------------------------------------------------------
    # 内部工具
    # ------------------------------------------------------------------
    def _fetch_room(self, conn: sqlite3.Connection, short_code: str):
        return conn.execute(
            "SELECT * FROM rooms WHERE short_code = ?", (short_code,)
        ).fetchone()

    def _fetch_room_by_id(self, conn: sqlite3.Connection, room_id: str):
        return conn.execute("SELECT * FROM rooms WHERE id = ?", (room_id,)).fetchone()

    def _find_live_room(self, conn: sqlite3.Connection, *, provider: str, session_id: str):
        return conn.execute(
            "SELECT * FROM rooms WHERE provider = ? AND session_id = ? "
            "AND status IN ('active', 'match_started')",
            (provider, session_id),
        ).fetchone()

    @staticmethod
    def _find_registration(
        conn: sqlite3.Connection,
        *,
        provider: str,
        session_id: str,
        idempotency_key: str,
    ):
        return conn.execute(
            "SELECT * FROM registrations "
            "WHERE provider = ? AND session_id = ? AND idempotency_key = ?",
            (provider, session_id, idempotency_key),
        ).fetchone()

    @staticmethod
    def _record_registration(
        conn: sqlite3.Connection,
        *,
        room: sqlite3.Row,
        provider: str,
        session_id: str,
        idempotency_key: str,
        created_at_ms: int,
    ) -> None:
        conn.execute(
            "INSERT OR REPLACE INTO registrations "
            "(provider, session_id, idempotency_key, room_id, short_code, created_at_ms) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            (
                provider,
                session_id,
                idempotency_key,
                room["id"],
                room["short_code"],
                created_at_ms,
            ),
        )

    def _fetch_reservation(self, conn: sqlite3.Connection, reservation_id: str):
        row = conn.execute(
            "SELECT * FROM reservations WHERE id = ?", (reservation_id,)
        ).fetchone()
        if row is None:
            raise ApiError(
                ErrorCode.RESERVATION_NOT_FOUND, "reservation not found"
            )
        return row

    @staticmethod
    def _require_token(room: sqlite3.Row, manage_token: str) -> None:
        # 以 UTF-8 字节比较：任意非 ASCII 输入也不会抛 TypeError，统一返回 FORBIDDEN
        supplied = str(manage_token or "").encode("utf-8")
        stored = str(room["manage_token"]).encode("utf-8")
        if not supplied or not secrets.compare_digest(stored, supplied):
            raise ApiError(
                ErrorCode.FORBIDDEN,
                "valid X-Manage-Token is required for owner operations",
            )

    @staticmethod
    def _assert_usable(room: sqlite3.Row, now: int) -> None:
        if room is None:
            raise ApiError(ErrorCode.ROOM_NOT_FOUND, "room short code not found")
        if room["status"] == "revoked":
            raise ApiError(ErrorCode.ROOM_REVOKED, "room has been revoked")
        if room["status"] == "expired" or room["expires_at_ms"] <= now:
            raise ApiError(ErrorCode.ROOM_EXPIRED, "room has expired")

    @staticmethod
    def _assert_protocol(room: sqlite3.Row, client_protocol_version: str) -> None:
        if str(client_protocol_version) != str(room["protocol_version"]):
            raise ApiError(
                ErrorCode.VERSION_MISMATCH,
                "client protocol version is incompatible with the room",
                details={
                    "room_protocol_version": room["protocol_version"],
                    "client_protocol_version": client_protocol_version,
                },
            )

    @staticmethod
    def _expire_stale_reservations(
        conn: sqlite3.Connection, room_id: str, now: int
    ) -> None:
        conn.execute(
            "UPDATE reservations SET status = 'expired' "
            "WHERE room_id = ? AND status = 'reserved' AND expires_at_ms <= ?",
            (room_id, now),
        )

    @staticmethod
    def _compute_occupancy(
        conn: sqlite3.Connection, room: sqlite3.Row, now: int
    ) -> Dict[str, int]:
        rows = conn.execute(
            "SELECT status, expires_at_ms FROM reservations "
            "WHERE room_id = ? AND status IN ('reserved', 'confirmed')",
            (room["id"],),
        ).fetchall()
        reserved = 0
        connected = 0
        for row in rows:
            if row["status"] == "confirmed":
                connected += 1
            elif row["expires_at_ms"] > now:  # 未过期预留才计入等待连接
                reserved += 1
        host_occupied = 1  # 房主固定占位
        used = host_occupied + reserved + connected
        return {
            "host_occupied": host_occupied,
            "reserved_count": reserved,
            "connected_count": connected,
            "max_players": room["max_players"],
            "seats_available": max(0, room["max_players"] - used),
        }

    @staticmethod
    def _registered_payload(room: sqlite3.Row, *, idempotent: bool) -> Dict[str, Any]:
        return {
            "room_id": room["id"],
            "short_code": room["short_code"],
            "provider": room["provider"],
            "session_id": room["session_id"],
            "connection": room["connection"],
            "protocol_version": room["protocol_version"],
            "display_version": room["display_version"],
            "max_players": room["max_players"],
            "status": room["status"],
            "created_at_ms": room["created_at_ms"],
            "expires_at_ms": room["expires_at_ms"],
            "manage_token": room["manage_token"],
            "idempotent": idempotent,
        }

    @staticmethod
    def _reservation_payload(
        res: sqlite3.Row,
        status: str,
        occupancy: Optional[Dict[str, int]],
        *,
        idempotent: bool,
    ) -> Dict[str, Any]:
        payload = {
            "reservation_id": res["id"],
            "room_id": res["room_id"],
            "player_name": res["player_name"],
            "status": status,
            "created_at_ms": res["created_at_ms"],
            "expires_at_ms": res["expires_at_ms"],
            "idempotent": idempotent,
        }
        if occupancy is not None:
            payload["occupancy"] = occupancy
        return payload
