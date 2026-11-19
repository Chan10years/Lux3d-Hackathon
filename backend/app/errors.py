"""稳定错误码定义与统一异常。

所有业务错误通过 ApiError 抛出，由 FastAPI 全局 handler 渲染为：
{"error": {"code": "...", "message": "...", "details": {...}}}
"""

from __future__ import annotations

from enum import Enum
from typing import Any, Dict, Optional


class ErrorCode(str, Enum):
    """对外稳定错误码，新增只能追加、不能改变含义。"""

    INVALID_REQUEST = "INVALID_REQUEST"
    FORBIDDEN = "FORBIDDEN"
    ROOM_NOT_FOUND = "ROOM_NOT_FOUND"
    RESERVATION_NOT_FOUND = "RESERVATION_NOT_FOUND"
    VERSION_MISMATCH = "VERSION_MISMATCH"
    ROOM_FULL = "ROOM_FULL"
    MATCH_ALREADY_STARTED = "MATCH_ALREADY_STARTED"
    RESERVATION_ALREADY_RELEASED = "RESERVATION_ALREADY_RELEASED"
    ROOM_EXPIRED = "ROOM_EXPIRED"
    ROOM_REVOKED = "ROOM_REVOKED"
    RESERVATION_EXPIRED = "RESERVATION_EXPIRED"
    SHORT_CODE_ALLOCATION_FAILED = "SHORT_CODE_ALLOCATION_FAILED"


HTTP_STATUS_BY_CODE: Dict[ErrorCode, int] = {
    ErrorCode.INVALID_REQUEST: 400,
    ErrorCode.FORBIDDEN: 403,
    ErrorCode.ROOM_NOT_FOUND: 404,
    ErrorCode.RESERVATION_NOT_FOUND: 404,
    ErrorCode.VERSION_MISMATCH: 409,
    ErrorCode.ROOM_FULL: 409,
    ErrorCode.MATCH_ALREADY_STARTED: 409,
    ErrorCode.RESERVATION_ALREADY_RELEASED: 409,
    ErrorCode.ROOM_EXPIRED: 410,
    ErrorCode.ROOM_REVOKED: 410,
    ErrorCode.RESERVATION_EXPIRED: 410,
    ErrorCode.SHORT_CODE_ALLOCATION_FAILED: 503,
}


class ApiError(Exception):
    """携带稳定错误码的业务异常。"""

    def __init__(
        self,
        code: ErrorCode,
        message: str,
        details: Optional[Dict[str, Any]] = None,
    ) -> None:
        super().__init__(f"{code.value}: {message}")
        self.code = code
        self.message = message
        self.details = details or {}
