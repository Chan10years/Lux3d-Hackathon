"""FastAPI 应用工厂与全局错误处理。"""

from __future__ import annotations

import asyncio
import logging
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Callable, Optional

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse

from . import __version__
from .clock import system_clock_ms
from .config import Settings
from .errors import HTTP_STATUS_BY_CODE, ApiError
from .room_service import RoomService
from .routes import health, rooms

logger = logging.getLogger("room_index")


def create_app(
    db_path: Optional[Path] = None,
    storage_root: Optional[Path] = None,
    clock: Optional[Callable[[], int]] = None,
    sweep_interval_seconds: Optional[int] = 60,
    short_code_fn: Optional[Callable[[], str]] = None,
    max_short_code_attempts: int = 8,
) -> FastAPI:
    """构建 FastAPI 应用。

    sweep_interval_seconds 传 0/None 可禁用过期清理后台任务（测试用可控时间）。
    """
    settings = Settings.resolve(
        db_path=db_path,
        storage_root=storage_root,
        sweep_interval_seconds=sweep_interval_seconds,
    )
    service = RoomService(
        db_path=settings.db_path,
        clock=clock or system_clock_ms,
        short_code_fn=short_code_fn,
        max_short_code_attempts=max_short_code_attempts,
    )

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        sweep_task = None
        if settings.sweep_interval_seconds:

            async def sweeper() -> None:
                while True:
                    await asyncio.sleep(settings.sweep_interval_seconds or 60)
                    try:
                        result = service.sweep()
                        if result["rooms_expired"] or result["reservations_expired"]:
                            logger.info("expired sweep: %s", result)
                    except Exception:  # noqa: BLE001 后台清理不允许中断服务
                        logger.exception("background sweep failed")

            sweep_task = asyncio.create_task(sweeper())
        try:
            yield
        finally:
            if sweep_task is not None:
                sweep_task.cancel()
                try:
                    await sweep_task
                except asyncio.CancelledError:
                    pass

    app = FastAPI(
        title="Lux3D Room Index Service",
        description="本机房间索引服务：房间登记 / 查询 / 续期 / 撤销 / 加入预留 / 开始对局。",
        version=__version__,
        lifespan=lifespan,
    )
    app.state.settings = settings
    app.state.room_service = service

    app.include_router(health.router)
    app.include_router(rooms.router)

    @app.exception_handler(ApiError)
    async def api_error_handler(request: Request, exc: ApiError) -> JSONResponse:
        return JSONResponse(
            status_code=HTTP_STATUS_BY_CODE[exc.code],
            content={
                "error": {
                    "code": exc.code.value,
                    "message": exc.message,
                    "details": exc.details,
                }
            },
        )

    @app.exception_handler(RequestValidationError)
    async def validation_error_handler(
        request: Request, exc: RequestValidationError
    ) -> JSONResponse:
        return JSONResponse(
            status_code=400,
            content={
                "error": {
                    "code": "INVALID_REQUEST",
                    "message": "request validation failed",
                    "details": {"errors": str(exc.errors())[:2000]},
                }
            },
        )

    return app


# 生产入口：uvicorn app.main:app（默认数据库 backend/data/rooms.db）
app = create_app()
