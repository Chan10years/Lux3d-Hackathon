"""短码生成。

短码为 6 位十进制数字（允许前导 0），由服务端产生。
并发碰撞由 rooms.short_code 的 UNIQUE 约束 + 事务内重试解决，
生成器可注入以便测试模拟碰撞。
"""

from __future__ import annotations

import secrets

SHORT_CODE_RANGE = 1_000_000  # 000000 - 999999
SHORT_CODE_LENGTH = 6


def new_short_code() -> str:
    """随机生成一个 6 位数字短码。"""
    return f"{secrets.randbelow(SHORT_CODE_RANGE):0{SHORT_CODE_LENGTH}d}"


def is_valid_short_code(value: str) -> bool:
    return (
        isinstance(value, str)
        and len(value) == SHORT_CODE_LENGTH
        and value.isdigit()
    )
