# Lux3D Room Index Service（backend）

本机房间索引服务：为《画个锤子》提供房间登记、按短码查询、续期、撤销、加入预留（确认/释放）与开始对局的协调。技术栈：Python 3.13 + FastAPI + SQLite（WAL）+ 本地文件存储，无任何云服务/凭据依赖。接口文档见 [docs/API.md](docs/API.md)。

## 目录结构

```
backend/
  requirements.txt        直接依赖声明
  requirements.lock       完整依赖锁定（含全部传递依赖精确版本）
  docs/API.md             /v1 接口文档（UE 对接）
  app/
    main.py               FastAPI 应用工厂（uvicorn 入口）
    config.py             配置（数据库/存储根目录/TTL 默认值）
    clock.py              可注入时钟（服务端时间决定有效期）
    db.py                 SQLite 连接、Schema、BEGIN IMMEDIATE 事务
    short_codes.py        6 位数字短码生成
    room_service.py       房间生命周期核心业务
    models.py             Pydantic 请求模型
    routes/               HTTP 路由（rooms / health）
    modules/              预留模块边界：generation_tasks / model_files
  tests/                  pytest 测试（隔离数据库 + 可控时钟）
  .venv/                  虚拟环境（运行时创建，不入库）
  data/                   运行时产物：rooms.db、files/（不入库）
```

## Windows 安装

```bat
cd /d "path\to\Lux3d-Hackathon\backend"
python --version                      rem 需要 Python 3.11+（在 3.13 验证）
python -m venv .venv
.venv\Scripts\python -m pip install -r requirements.lock
```

`requirements.lock` 由 `pip freeze` 生成，锁定全部直接与传递依赖的精确版本（fastapi 0.115.8 / pydantic 2.10.6 / starlette 0.45.3 / httpx 0.28.1 / pytest 8.3.4 等），Windows 下安装结果可复现；`requirements.txt` 仅作直接依赖清单参考，请勿用于可复现安装。

## 启动服务

```bat
cd /d "path\to\Lux3d-Hackathon\backend"
.venv\Scripts\python -m uvicorn app.main:app --host 127.0.0.1 --port 8901
```

- 健康检查：`curl http://127.0.0.1:8901/v1/health`
- 交互式文档：`http://127.0.0.1:8901/docs`
- 数据库文件默认 `backend\data\rooms.db`（自动创建）；重启服务后有效房间自动恢复。

## 运行测试

```bat
cd /d "path\to\Lux3d-Hackathon\backend"
.venv\Scripts\python -m pytest tests -v
```

测试特性：

- 每个用例使用 `tmp_path` 隔离数据库，互不污染；
- 时间由 `FakeClock` 注入，可精确验证过期/续期/预留期限；
- 并发用例使用多线程 + 独立 SQLite 连接，真实触发事务竞争；
- 后台清理任务在测试中禁用，`sweep()` 由用例显式调用以保持时间可控。

覆盖场景：并发登记唯一性、短码碰撞重试、重复请求幂等、并发加入容量争抢（≤4）、开始对局与加入预留竞争、过期续期拒绝、撤销后不可加入且短码不重用、无管理令牌修改被拒、协议版本不兼容、服务重启后有效房间恢复、过期清理。

## 快速自检（可选）

```bat
.venv\Scripts\python -c "from fastapi.testclient import TestClient; from app.main import create_app; c=TestClient(create_app(db_path='data/_smoke.db', sweep_interval_seconds=None)); print(c.get('/v1/health').json())"
```
