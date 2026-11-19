"""服务配置。

本机服务，全部配置在进程内解析；数据库与文件存储均位于 backend/data/
（运行时创建，已加入 .gitignore）。
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Optional

# backend/ 目录（本文件位于 backend/app/config.py）
BACKEND_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_DB_PATH = BACKEND_ROOT / "data" / "rooms.db"
DEFAULT_STORAGE_ROOT = BACKEND_ROOT / "data" / "files"

MAX_PLAYERS = 4  # 需求固定四人上限
DEFAULT_ROOM_TTL_SECONDS = 3600
DEFAULT_RESERVATION_TTL_SECONDS = 60


@dataclass(frozen=True)
class Settings:
    db_path: Path
    storage_root: Path
    # 过期清理后台任务间隔秒数；None 表示禁用（测试中禁用以便可控时间）
    sweep_interval_seconds: Optional[int]

    @classmethod
    def resolve(
        cls,
        db_path: Optional[Path] = None,
        storage_root: Optional[Path] = None,
        sweep_interval_seconds: Optional[int] = 60,
    ) -> "Settings":
        db = Path(db_path) if db_path is not None else DEFAULT_DB_PATH
        root = Path(storage_root) if storage_root is not None else DEFAULT_STORAGE_ROOT
        db.parent.mkdir(parents=True, exist_ok=True)
        root.mkdir(parents=True, exist_ok=True)
        interval = sweep_interval_seconds if sweep_interval_seconds else None
        return cls(
            db_path=db,
            storage_root=root,
            sweep_interval_seconds=interval,
        )
