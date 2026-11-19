"""健康检查路由。"""

from __future__ import annotations

from fastapi import APIRouter

from .. import __version__

router = APIRouter(prefix="/v1", tags=["meta"])


@router.get("/health")
def health() -> dict:
    return {"status": "ok", "service": "room-index", "api_version": "v1", "app_version": __version__}
