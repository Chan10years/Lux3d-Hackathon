"""房间生命周期与加入预留的 /v1 路由。"""

from __future__ import annotations

from typing import Optional

from fastapi import APIRouter, Request, Response

from ..errors import ApiError, ErrorCode
from ..models import (
    JoinReservationRequest,
    RegisterRoomRequest,
    RenewRoomRequest,
)
from ..room_service import RoomService

router = APIRouter(prefix="/v1")


def _service(request: Request) -> RoomService:
    return request.app.state.room_service  # type: ignore[no-any-return]


def _require_manage_token(request: Request) -> str:
    token = request.headers.get("x-manage-token")
    if not token:
        raise ApiError(
            ErrorCode.FORBIDDEN, "missing X-Manage-Token header"
        )
    return token


@router.post("/rooms", tags=["rooms"])
def register_room(
    body: RegisterRoomRequest, request: Request, response: Response
) -> dict:
    """登记房间；幂等重试返回 200 + 同一短码/令牌，新建返回 201。"""
    status, payload = _service(request).register_room(**body.model_dump())
    response.status_code = status
    return payload


@router.get("/rooms/{short_code}", tags=["rooms"])
def get_room(
    short_code: str,
    request: Request,
    protocol_version: Optional[str] = None,
) -> dict:
    """按短码查询房间（不含管理令牌）。

    可选 query 参数 protocol_version：提供时做兼容性校验。
    """
    return _service(request).get_room(
        short_code, client_protocol_version=protocol_version
    )


@router.post("/rooms/{short_code}/renew", tags=["rooms"])
def renew_room(short_code: str, body: RenewRoomRequest, request: Request) -> dict:
    """房主续期；过期/撤销房间不可复活。"""
    token = _require_manage_token(request)
    return _service(request).renew_room(short_code, token, ttl_seconds=body.ttl_seconds)


@router.delete("/rooms/{short_code}", tags=["rooms"])
def revoke_room(short_code: str, request: Request) -> dict:
    """房主撤销房间（幂等）。撤销后短码保留，不会立即重新分配。"""
    token = _require_manage_token(request)
    return _service(request).revoke_room(short_code, token)


@router.post("/rooms/{short_code}/start", tags=["rooms"])
def start_match(short_code: str, request: Request) -> dict:
    """房主开始对局（幂等）；与加入预留事务互斥，开始后关闭加入。"""
    token = _require_manage_token(request)
    return _service(request).start_match(short_code, token)


@router.post("/rooms/{short_code}/reservations", status_code=201, tags=["reservations"])
def create_reservation(
    short_code: str, body: JoinReservationRequest, request: Request
) -> dict:
    """创建有期限的加入预留；满员/版本不兼容/已开始分别报错。"""
    return _service(request).create_reservation(
        short_code,
        player_name=body.player_name,
        protocol_version=body.protocol_version,
        ttl_seconds=body.ttl_seconds,
    )


@router.post("/reservations/{reservation_id}/confirm", tags=["reservations"])
def confirm_reservation(reservation_id: str, request: Request) -> dict:
    """幂等确认预留，占用转为已连接。"""
    return _service(request).confirm_reservation(reservation_id)


@router.post("/reservations/{reservation_id}/release", tags=["reservations"])
def release_reservation(reservation_id: str, request: Request) -> dict:
    """幂等释放预留，归还占位。"""
    return _service(request).release_reservation(reservation_id)
