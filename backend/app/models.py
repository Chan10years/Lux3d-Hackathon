"""API 请求模型（Pydantic v2）。响应模型见 docs/API.md 示例。"""

from __future__ import annotations

from typing import Optional

from pydantic import BaseModel, Field

from .config import (
    DEFAULT_RESERVATION_TTL_SECONDS,
    DEFAULT_ROOM_TTL_SECONDS,
)


class RegisterRoomRequest(BaseModel):
    """房间登记请求。

    protocol_version 为协议兼容版本（参与兼容性校验）；
    display_version 为普通展示版本（仅展示，不参与校验）。
    """

    provider: str = Field(min_length=1, max_length=128, description="会话提供方")
    session_id: str = Field(min_length=1, max_length=256, description="准确会话标识")
    connection: str = Field(min_length=1, max_length=512, description="连接描述")
    protocol_version: str = Field(min_length=1, max_length=32, description="协议兼容版本")
    idempotency_key: str = Field(
        min_length=8,
        max_length=256,
        description="客户端秘密幂等凭证：仅持相同凭证的重复登记可取回管理令牌",
    )
    display_version: Optional[str] = Field(
        default=None, max_length=32, description="普通展示版本（仅展示）"
    )
    ttl_seconds: int = Field(
        default=DEFAULT_ROOM_TTL_SECONDS, ge=30, le=86400, description="有效期秒数"
    )


class RenewRoomRequest(BaseModel):
    ttl_seconds: int = Field(default=DEFAULT_ROOM_TTL_SECONDS, ge=30, le=86400)


class JoinReservationRequest(BaseModel):
    player_name: Optional[str] = Field(default=None, max_length=64)
    protocol_version: str = Field(min_length=1, max_length=32, description="客户端协议兼容版本")
    ttl_seconds: int = Field(
        default=DEFAULT_RESERVATION_TTL_SECONDS, ge=10, le=600,
        description="预留保留秒数",
    )
