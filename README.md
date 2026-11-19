# 异界：溯源信标 · Lux3D Hackathon

本分支公开《异界：溯源信标》（早期代号《画个锤子》）的战斗、程序化信标、GLB 武器导入与同步、局域网联机及房号索引服务源码。

**这是不含素材的源码交付，不是克隆后即可运行的完整游戏。** 地图、角色、动画、UI 蓝图、材质、音效、字体、视频和生成模型均不随本次提交发布。此前远端已有的验证原型仍保留原位置。

| 目录 | 内容 |
| --- | --- |
| `unreal/Source/` | 战斗、信标、8 分钟对局、房号/网络、GLB 导入及武器同步 |
| `unreal/Plugins/TrioCombat/Source/` | 战斗、HUD 与编辑器工具源码 |
| `unreal/Plugins/glTFRuntime/` | glTFRuntime 插件源码及 MIT 许可证 |
| `unreal/Config/` | 去除本机令牌后的共享配置，保留项目资源路径作为集成参考 |
| `unreal/scripts/` | 局域网发现、生成服务网关、独立 C++ 测试入口 |
| `unreal/tests/` | 战斗规则、地图投影与本地网络测试 |
| `backend/` | FastAPI + SQLite 房号索引服务、API 文档及测试 |
| `test/1028_Lux3D_assumption_test/` | 仓库已有的早期验证原型 |

## Unreal 源码

工程入口：`unreal/Try_GLB_Put_Into.uproject`。目标环境是 Windows x64、Unreal Engine 5.8（开发环境记录为 5.8.4）和 Visual Studio C++ 工具链。模块使用 `BuildSettingsVersion.V7`，不保证较早引擎兼容。

自带插件源码为 glTFRuntime（本地版本标识 `20260826`）与 TrioCombat；其余依赖由 UE 提供。公开工程描述中已去除开发机专用的 MCP 工具插件。安装对应引擎和 C++ 工具后，生成项目文件并编译 `Try_GLB_Put_IntoEditor`。这只验证代码构建；运行完整体验还需要自行取得授权素材，按 [集成说明](docs/SOURCE_ONLY.md) 配置资源。

GLB 导入与装备主要位于 `WeaponGrip.cpp`、`WeaponForgeSubsystem.cpp`、`WeaponSyncComponent.cpp`。生成 API 的客户端保留；生成后台、SDK 运行包、模型权重、账号密钥和付费服务均不在此次交付中。不要把房号服务当作生成服务。

## 房号服务

```powershell
cd backend
python -m venv .venv
.\.venv\Scripts\python -m pip install -r requirements.lock
.\.venv\Scripts\python -m uvicorn app.main:app --host 127.0.0.1 --port 8901
```

API 及部署说明见 [backend/README.md](backend/README.md) 和 [backend/docs/API.md](backend/docs/API.md)。需要局域网访问时在受信任的局域网内绑定对应地址并配置防火墙。运行数据库默认生成在 `backend/data/`，不应提交。

## 独立验证

```powershell
# 按上文创建 backend/.venv 并安装依赖后，在仓库根目录运行：
cd backend
.\.venv\Scripts\python -m pytest tests -q
cd ..
.\backend\.venv\Scripts\python -m unittest discover -s unreal/tests -p "test_*.py" -v
# Windows C++ 工具链（自动使用 vswhere 查找安装位置）：
.\unreal\scripts\Test-Cpp.cmd
```

上述测试不调用云端模型生成接口，也不需要付费账号。代码发布范围及本次实际验证结果见 [发布记录](docs/SOURCE_ONLY.md)。

## 许可证与依赖

glTFRuntime 来自 [rdeioris/glTFRuntime](https://github.com/rdeioris/glTFRuntime)，按 [MIT License](unreal/Plugins/glTFRuntime/LICENSE) 保留版权和许可；本次复用现有工程中的插件版本，没有新增运行依赖。Unreal Engine 遵循 Epic 的许可条款，仓库不分发引擎。项目自有代码的整体开源许可证尚未指定；本次公开不为第三方素材或 SDK 授予再分发许可。依赖清单见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
