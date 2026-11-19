# Room Index Service API（v1）

本机房间索引服务接口文档，供 UE 客户端对接。服务只负责房间生命周期与加入协调；**最终准入与实时战斗权威始终在 UE 侧**。

- Base URL：`http://127.0.0.1:8901`
- API 前缀：`/v1`（版本化；不兼容变更将引入 `/v2`）
- 数据格式：JSON（UTF-8）
- 错误格式：统一 `{"error": {"code", "message", "details"}}`，错误码稳定可枚举
- 时间字段：均为 Unix epoch 毫秒（`*_ms`）
- 所有有效期由**服务端时间**判定，客户端时钟不参与

---

## 1. 核心概念与语义

| 概念 | 说明 |
| --- | --- |
| 房间 | 由「会话提供方 provider + 会话标识 session_id」唯一登记的对局容器 |
| 短码 short_code | 6 位数字（000000–999999），**服务端产生**，玩家凭此查询房间 |
| 管理令牌 manage_token | 随机令牌，仅登记响应（含幂等重试）返回；房主续期/撤销/开始对局必带 |
| 协议兼容版本 protocol_version | 参与兼容性校验：加入/查询校验时必须与房间登记值**完全一致** |
| 展示版本 display_version | 普通展示用途，**不参与**任何校验 |
| 房主占位 host_occupied | 房主固定占用 1 席 |
| 等待连接 reserved_count | 已创建、未过期、未确认的加入预留数量 |
| 已连接人数 connected_count | 已 confirm 的预留数量 |
| 容量 max_players | 固定 4；占用 = host_occupied + reserved_count + connected_count ≤ 4 |
| 加入预留 | 有期限（默认 60s）的占位凭证；confirm 幂等确认、release 幂等释放 |
| 房间状态 | `active` → `match_started`（开始对局）/ `revoked`（撤销）/ `expired`（过期清理） |

**互斥保证**：开始对局与加入预留使用同一 SQLite 写事务（BEGIN IMMEDIATE）串行化；开始后新加入一律拒绝，开始前已获得的预留仍可确认。短码唯一由数据库 UNIQUE 约束 + 事务内碰撞重试保证；撤销/过期房间的短码行保留为墓碑，**不会立即重新分配**。

---

## 2. 稳定错误码

| code | HTTP | 场景 |
| --- | --- | --- |
| `INVALID_REQUEST` | 400 | 请求体缺失/非法字段、越界参数 |
| `FORBIDDEN` | 403 | 缺失或错误的 X-Manage-Token（无权修改）；登记幂等凭证与既有活房间不匹配 |
| `ROOM_NOT_FOUND` | 404 | 短码不存在 |
| `RESERVATION_NOT_FOUND` | 404 | 预留 ID 不存在 |
| `VERSION_MISMATCH` | 409 | 协议兼容版本不一致 |
| `ROOM_FULL` | 409 | 容量已满（4 人） |
| `MATCH_ALREADY_STARTED` | 409 | 对局已开始，加入关闭 |
| `RESERVATION_ALREADY_RELEASED` | 409 | 已释放的预留不能确认 |
| `ROOM_EXPIRED` | 410 | 房间已过期（续期不可复活） |
| `ROOM_REVOKED` | 410 | 房间已撤销 |
| `RESERVATION_EXPIRED` | 410 | 预留已过期，需重新预留 |
| `SHORT_CODE_ALLOCATION_FAILED` | 503 | 短码分配重试后仍失败（理论上极罕见） |

错误响应示例：

```json
{
  "error": {
    "code": "ROOM_FULL",
    "message": "room capacity reached",
    "details": {
      "host_occupied": 1,
      "reserved_count": 0,
      "connected_count": 3,
      "max_players": 4,
      "seats_available": 0
    }
  }
}
```

---

## 3. 端点

### 3.1 GET /v1/health

健康检查。

响应 `200`：

```json
{ "status": "ok", "service": "room-index", "api_version": "v1", "app_version": "1.0.0" }
```

### 3.2 POST /v1/rooms — 房间登记

请求：

```json
{
  "provider": "ue-session",
  "session_id": "sess-0001",
  "connection": "127.0.0.1:7777",
  "protocol_version": "1.0.0",
  "idempotency_key": "b3f1a9c0-4d2e-4f8a-9c7b-1e2d3a4b5c6d",
  "display_version": "demo-1028",
  "ttl_seconds": 3600
}
```

| 字段 | 必填 | 约束 |
| --- | --- | --- |
| provider | 是 | 1–128 字符，会话提供方 |
| session_id | 是 | 1–256 字符，准确会话标识 |
| connection | 是 | 1–512 字符，连接描述（如 ip:port） |
| protocol_version | 是 | 1–32 字符，协议兼容版本 |
| idempotency_key | 是 | 8–256 字符，**客户端生成并持有的秘密幂等凭证**（建议 UUID；勿使用可公开推导的值） |
| display_version | 否 | ≤32 字符，展示版本 |
| ttl_seconds | 否 | 30–86400，默认 3600 |

响应 `201`（新建）：

```json
{
  "room_id": "5f0c...",
  "short_code": "483920",
  "provider": "ue-session",
  "session_id": "sess-0001",
  "connection": "127.0.0.1:7777",
  "protocol_version": "1.0.0",
  "display_version": "demo-1028",
  "max_players": 4,
  "status": "active",
  "created_at_ms": 1700000000000,
  "expires_at_ms": 1700003600000,
  "manage_token": "qF3...随机令牌...",
  "idempotent": false
}
```

响应 `200`（幂等重试）：服务端**永久保存登记请求与房间的对应关系**。同一 `provider + session_id + idempotency_key` 的重试总是返回最初创建的房间（同一 `room_id / short_code / manage_token`，`"idempotent": true`）——即使该房间已撤销或过期，也返回其当前终态（`status: "revoked"` / `"expired"`）。**凭证与任何既有活房间不符时返回 `403 FORBIDDEN`，不会返回管理令牌**。房主主动重开房间请使用**新的 idempotency_key**，服务会创建新房间并保留旧短码为墓碑。客户端网络重试可安全无限重复调用。

错误：`INVALID_REQUEST`、`FORBIDDEN`（幂等凭证不匹配）、`SHORT_CODE_ALLOCATION_FAILED`。

### 3.3 GET /v1/rooms/{short_code} — 按短码查询

可选 query：`?protocol_version=1.0.0`（提供时执行兼容性校验）。

响应 `200`（**不含 manage_token**）：

```json
{
  "room_id": "5f0c...",
  "short_code": "483920",
  "status": "active",
  "provider": "ue-session",
  "session_id": "sess-0001",
  "connection": "127.0.0.1:7777",
  "protocol_version": "1.0.0",
  "display_version": "demo-1028",
  "max_players": 4,
  "host_occupied": 1,
  "reserved_count": 1,
  "connected_count": 1,
  "seats_available": 1,
  "match_started": false,
  "created_at_ms": 1700000000000,
  "expires_at_ms": 1700003600000
}
```

错误：`ROOM_NOT_FOUND`、`ROOM_EXPIRED`、`ROOM_REVOKED`、`VERSION_MISMATCH`。

### 3.4 POST /v1/rooms/{short_code}/renew — 续期（房主）

Header：`X-Manage-Token: <manage_token>`

请求：

```json
{ "ttl_seconds": 1800 }
```

响应 `200`：

```json
{
  "room_id": "5f0c...",
  "short_code": "483920",
  "status": "active",
  "expires_at_ms": 1700001800000,
  "occupancy": {
    "host_occupied": 1, "reserved_count": 0, "connected_count": 0,
    "max_players": 4, "seats_available": 3
  }
}
```

语义：`新 expires_at = 服务端当前时间 + ttl_seconds`。**过期或撤销的房间续期直接失败，不可复活**。`match_started` 状态允许续期（对局进行中保持房间记录）。

错误：`FORBIDDEN`、`ROOM_NOT_FOUND`、`ROOM_EXPIRED`、`ROOM_REVOKED`。

### 3.5 DELETE /v1/rooms/{short_code} — 撤销（房主，幂等）

Header：`X-Manage-Token: <manage_token>`

响应 `200`：

```json
{ "room_id": "5f0c...", "short_code": "483920", "status": "revoked", "already_revoked": false }
```

语义：撤销为终态；重复撤销（带正确令牌）返回 `already_revoked: true`；撤销后旧短码永久保留为墓碑，不会被分配给新房间。

错误：`FORBIDDEN`、`ROOM_NOT_FOUND`。

### 3.6 POST /v1/rooms/{short_code}/reservations — 加入预留

请求：

```json
{ "player_name": "Player2", "protocol_version": "1.0.0", "ttl_seconds": 60 }
```

| 字段 | 必填 | 约束 |
| --- | --- | --- |
| player_name | 否 | ≤64 字符 |
| protocol_version | 是 | 必须与房间 protocol_version 完全一致 |
| ttl_seconds | 否 | 10–600，默认 60（预留保留期限） |

响应 `201`：

```json
{
  "reservation_id": "9b1a...",
  "room_id": "5f0c...",
  "short_code": "483920",
  "player_name": "Player2",
  "status": "reserved",
  "created_at_ms": 1700000005000,
  "expires_at_ms": 1700000065000,
  "occupancy": {
    "host_occupied": 1, "reserved_count": 1, "connected_count": 0,
    "max_players": 4, "seats_available": 2
  }
}
```

校验顺序：房间存在 → 未撤销/未过期 → 未开始对局 → 版本兼容 → 容量未满。
错误：`ROOM_NOT_FOUND`、`ROOM_REVOKED`、`ROOM_EXPIRED`、`MATCH_ALREADY_STARTED`、`VERSION_MISMATCH`、`ROOM_FULL`。

### 3.7 POST /v1/reservations/{reservation_id}/confirm — 确认预留（幂等）

无请求体。响应 `200`：

```json
{
  "reservation_id": "9b1a...",
  "room_id": "5f0c...",
  "player_name": "Player2",
  "status": "confirmed",
  "created_at_ms": 1700000005000,
  "expires_at_ms": 1700000065000,
  "idempotent": false,
  "occupancy": {
    "host_occupied": 1, "reserved_count": 0, "connected_count": 1,
    "max_players": 4, "seats_available": 2
  }
}
```

语义：预留占位转为已连接；重复确认返回相同结果（`idempotent: true`），不重复占位。预留过期后确认返回 `RESERVATION_EXPIRED`；**房间过期（或已撤销）后其预留同样不可确认**，分别返回 `ROOM_EXPIRED` / `ROOM_REVOKED`。开始对局前已获得的预留，在开始后仍可确认。

错误：`RESERVATION_NOT_FOUND`、`RESERVATION_EXPIRED`、`RESERVATION_ALREADY_RELEASED`、`ROOM_EXPIRED`、`ROOM_REVOKED`。

### 3.8 POST /v1/reservations/{reservation_id}/release — 释放预留（幂等）

无请求体。响应 `200`：

```json
{
  "reservation_id": "9b1a...",
  "room_id": "5f0c...",
  "player_name": "Player2",
  "status": "released",
  "created_at_ms": 1700000005000,
  "expires_at_ms": 1700000065000,
  "idempotent": false,
  "occupancy": {
    "host_occupied": 1, "reserved_count": 0, "connected_count": 0,
    "max_players": 4, "seats_available": 3
  }
}
```

语义：无论预留处于 reserved / confirmed / 已过期，释放都返回 `200`（幂等，可安全重试），席位立即归还。仅 `RESERVATION_NOT_FOUND` 报错。

### 3.9 POST /v1/rooms/{short_code}/start — 开始对局（房主，幂等）

Header：`X-Manage-Token: <manage_token>`。无请求体。

响应 `200`：

```json
{
  "room_id": "5f0c...",
  "short_code": "483920",
  "status": "match_started",
  "already_started": false,
  "occupancy": {
    "host_occupied": 1, "reserved_count": 0, "connected_count": 3,
    "max_players": 4, "seats_available": 0
  }
}
```

语义：与加入预留**事务互斥**；开始后新加入请求一律 `MATCH_ALREADY_STARTED`。重复开始返回 `already_started: true`。UE 侧仍负责最终准入与战斗权威。

错误：`FORBIDDEN`、`ROOM_NOT_FOUND`、`ROOM_EXPIRED`、`ROOM_REVOKED`。

---

## 4. UE 客户端对接时序

### 4.1 房主：幂等登记

```
UE(房主)                     本服务
   |-- 本地生成秘密 idempotency_key 并持久保存
   |-- POST /v1/rooms -------->|  201 {short_code, manage_token}
   |   （网络超时/断线重试，携带相同 idempotency_key）
   |-- POST /v1/rooms -------->|  200 同一 short_code + manage_token (idempotent=true)
   |<-- 本地持久保存 manage_token 与 idempotency_key
```

- 登记请求携带固定 `provider + session_id + idempotency_key`；响应丢失可无限重试，结果幂等。
- `idempotency_key` 是取回令牌的唯一凭证：凭证不符的重复登记会被拒绝且不泄露令牌。
- 登记历史永久保存：原房间撤销/过期后，旧凭证重试返回原房间终态；重开新房间必须使用新凭证。
- `manage_token` 必须本地持久化：查询接口不返回令牌；丢失令牌须凭 `idempotency_key` 重试登记找回。
- 对局进行中每隔 `ttl/3` 调用 renew 保持房间不过期。

### 4.2 玩家：加入 → 确认 / 释放

```
UE(玩家)                     本服务                     UE(房主)
   |-- GET /v1/rooms/{code}?protocol_version=x -->|  200（可选做版本预检）
   |-- POST .../reservations ---->|  201 {reservation_id, expires_at}
   |   （在 expires_at 前完成 UE 侧准入协商）
   |-- POST .../confirm --------->|  200 {status: confirmed}
   |   （重复 confirm 幂等，安全重试）
   |   （玩家退出或超时）
   |-- POST .../release --------->|  200（幂等，席位归还）
```

- 客户端拿 `reservation_id` 后须在 `expires_at_ms` 前完成 confirm；过期需重新预留。
- 容量以服务端事务为准：并发预留不会突破 4 人；`ROOM_FULL` 应提示玩家稍后重试。
- `MATCH_ALREADY_STARTED` 表示对局已开始，客户端应走观战/重试流程。

---

## 5. 过期清理机制

- 后台任务默认每 60s 执行一次 sweep（服务内建，无需调用）。
- sweep 将「过期房间」「过期预留」落为 `expired` 终态；过期判定完全由服务端时间驱动。
- 房间行与短码**永久保留**（墓碑）：撤销/过期房间的短码不会被重新分配给新房间，防止误入。
- `expired` 房间释放登记幂等键，同一 `provider + session_id` 可重新登记新房间。

## 6. 模块边界（预留，本批次未实现）

- `app/modules/generation_tasks/`：未来武器生成任务模块的挂载点。
- `app/modules/model_files/`：未来模型文件管理模块的挂载点。
- 文件存储根目录：`Settings.storage_root`（默认 `backend/data/files/`），与数据库文件（`backend/data/rooms.db`）职责分离。
