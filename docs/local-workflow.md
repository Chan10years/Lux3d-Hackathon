# 本地武器制作

## 启动

在仓库根目录运行（Python 3.10+）：

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/uvicorn backend.main:app --host 127.0.0.1 --port 8000
```

Windows 使用 `.venv\Scripts\python.exe` 和 `.venv\Scripts\uvicorn.exe`。
访问 http://127.0.0.1:8000 ，在原有创作页面下方使用“武器制作”。先画草图、填写文字，选择模拟或真实模式后提交。任务会轮询，刷新页面仍可查看。完成后预览、下载 GLB、标记装备。
接口文档：http://127.0.0.1:8000/docs 。这里只监听本机，未部署公网。

## 组织与持久化

- `backend/db.py`：SQLite repository，任务、原文、规则快照、抽卡、进度、错误、武器数据。
- `backend/storage.py`：文件存储，默认 `data/assets/<task-id>/`，保存草图、参考图和模型。
- `backend/jobs.py`：后台任务执行，与 HTTP 请求分离。
- `backend/providers.py`：模拟生成、真实 HTTP bridge 适配协议与基础文件检查。
- `backend/config.json`：概率、倍率、技能池、类型兼容、装备目标长度、上传与调用限制。

`WEAPON_DATA_DIR` 可更改数据库和文件根目录。备份整个 data 目录；运行时不要单独复制 SQLite 主文件而漏掉 WAL。原站实验记录仍存 IndexedDB；与新增的服务器制作任务分别存储，原站导出按钮不包含服务器任务。服务器任务目前保留全部记录，无分页或删除界面。

每次创建任务抽卡一次。查询、页面刷新、服务重启不重抽；重新提交是新任务，会重新抽卡。没有自动重试远端请求，避免重复计费。排队任务重启后恢复；运行中的任务重启后标失败并要求人工核查远端。已存参考图和抽卡保留。进程崩溃可能留下孤立文件。

本地单进程、单串行 worker；请不要加 `--workers` 或在生成期间热重载。后续可换托管数据库、对象存储、独立消息队列 worker。当前没有跨进程任务租约、用户认证和多租户隔离，接口仅用于本地测试。

## 真实 API 接入边界

**本次没有真实供应商调用成功的证据。** 模拟模式始终 `is_mock=true`，参考图是纯色 PNG、模型为三角形 GLB，占位素材不能用来判断武器生成质量。真实模式绝不回退模拟。

真实模式配置 `GENERATION_BRIDGE_URL` 和可选 `GENERATION_BRIDGE_TOKEN`（环境变量，不提交密钥）。此 URL 是需要另行实现的供应商桥接服务，**不能直接填写 Lux3D 官网或原始 API 地址**。当前没有内置 Lux3D SDK 适配器，也未接入真实生图/融合供应商。供应商未确定，因此定义以下稳定桥接契约，后续在此边界实现供应商鉴权、异步轮询及下载：

- `POST <base>/fusion`：输入 `prompt,weapon_type,sketch_base64`，输出 JSON `{ "description":"...", "weapon_type":"short|long" }`。
- `POST <base>/image`：输入上述字段加 `description`，输出 `{ "content_base64":"PNG/JPEG 图片的 base64" }`。
- `POST <base>/model`：输入 `image_base64,description`，输出 `{ "content_base64":"GLB 文件的 base64" }`。
- 使用 `Authorization: Bearer <token>`；每步默认 120 秒。桥接服务应等待实际完成再返回。HTTP 错误、无效 JSON/图片/模型均记录失败，不产生完整武器。

## 临时假设和待验收规则

- 图文冲突由融合供应商处理；基础长短类型暂由玩家选，AI 外观描述不改变玩法类型。
- R/SR/SSR 概率为 75%/22%/3%，攻击为 10×1/1.5/2；普通与特殊技能各选一个，仅从预设池按类型过滤。均为临时平衡数值，修改配置只影响新任务。
- 生图到 3D 按顺序；属性先抽好，当前 worker 串行执行，未实现两路并行。
- GLB 只检查头、长度、JSON、场景和网格存在；**未做完整 glTF 验证、物理尺寸测量、朝向修正、握持适配、碰撞或攻击判定**。
- 装备数据暂用米、Y 向上、Z 向前、原点握持，长短目标长度 1.6/0.8 米，仅元数据，不缩放模型。`equipment_check=requires_engine_review`。
- “装备”只持久化一个标记，不会操作 Unity/UE，也不代表模型已经适配。

## Unity / UE 调用

`POST /api/v1/tasks` multipart 字段 `prompt,mode,weapon_type,sketch` → 202 + task ID。
轮询 `GET /api/v1/tasks/{id}`，状态 `queued/running/succeeded/failed`，阶段 `fusion/image/model/validation/complete`。失败看 error；只有 succeeded 才有 weapon_id。
`GET /api/v1/weapons/{id}` 获取属性、技能、来源标记、模型相对 URL、装备元数据；拼接服务器 origin 下载 GLB。
`POST /api/v1/weapons/{id}/equip` 保存装备标记。引擎必须先检查 `is_mock` 与 `checks.equipment_check`，并自行实现 GLB 导入、坐标转换和碰撞。

## 检查

```sh
WEAPON_DATA_DIR=/tmp/weapon-test .venv/bin/python -m pytest -q
# 服务运行中；需安装 Playwright 和 Chromium
node tests/browser-flow.cjs
node test/1028_Lux3D_assumption_test/checks/browser-qa.cjs
```

测试数据应使用独立目录，浏览器检查会向当前服务器写入合成任务，原测试脚本会更新归档 QA 截图。
