"""可注入时钟。

服务端时间（clock）唯一决定房间的有效期与预留期限，测试可注入
FakeClock 以获得可控时间，生产使用 system_clock_ms。
"""

from __future__ import annotations

import time
from typing import Callable

# Clock 返回 Unix epoch 毫秒
Clock = Callable[[], int]


def system_clock_ms() -> int:
    """生产默认时钟：当前系统时间的 epoch 毫秒。"""
    return int(time.time() * 1000)


class FakeClock:
    """测试用可控时钟，可手动推进时间。"""

    def __init__(self, start_ms: int = 1_700_000_000_000) -> None:
        self._now_ms = int(start_ms)

    def __call__(self) -> int:
        return self._now_ms

    def advance(self, seconds: float = 0.0, ms: int = 0) -> None:
        """向前推进指定时间。"""
        self._now_ms += int(seconds * 1000) + int(ms)

    def set(self, ms: int) -> None:
        """直接设置当前时间。"""
        self._now_ms = int(ms)
