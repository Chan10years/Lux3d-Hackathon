# 异界：溯源信标 · Lux3D Hackathon

> 🏆 **最新进展（2026-11-26）**：作品《异界：溯源信标》已入围 **Aholo Lux3D Hackathon 全球 Top 50**！已完成实机战斗与正式录屏，**Windows 免安装双机局域网试玩包（2026-11-24 正式录屏版）**已发布至 GitHub Releases，可直接下载试玩。

---

## 📦 直接下载试玩版（Windows 独立双机联机包）

- **Release 下载页**：[v2026.11.24-lan-demo（2026-11-24 正式录屏版）](https://github.com/Chan10years/Lux3d-Hackathon/releases/tag/v2026.11.24-lan-demo)
- **分卷说明**：完整免安装客户端解压后约 **5.58 GiB**（核心资源包 `Try_GLB_Put_Into-Windows.ucas` 单文件 2.31 GiB），因 GitHub Releases 单文件上限为 2 GiB，已拆分为 **3 个 ZIP64 分卷**（1.80 GB + 1.80 GB + 1.66 GB）。

### 快速上手（3 步开玩）
1. 从 [Release 页面](https://github.com/Chan10years/Lux3d-Hackathon/releases/tag/v2026.11.24-lan-demo) 将以下 4 个文件下载到**同一个文件夹**：
   - `Yijie-Suoyuan-Xinbiao-LAN-20261124.zip.001`
   - `Yijie-Suoyuan-Xinbiao-LAN-20261124.zip.002`
   - `Yijie-Suoyuan-Xinbiao-LAN-20261124.zip.003`
   - `Extract-Game.bat`（一键校验分卷并自动解压脚本；也可自选 `SHA256SUMS.txt` 校验）
2. 双击 **`Extract-Game.bat`** 自动解压（或使用 **7-Zip / NanaZip / Bandizip / WinRAR** 右键解压 `.zip.001`）。
3. 进入解压后的 `异界溯源信标-双机联机包-20261124-正式录屏版` 目录，双击 **`Start-Game.cmd`** 启动游戏（两台电脑在同一局域网下可输入 6 位房号直接开房联机）。

> **安全与 API Key 说明**：公开下载包已排除本机日志/崩溃转储，并对 `Private/demo-credentials.json` 做了**脱密清空处理**。**不填写任何 Key 即可直接使用三名角色初始武器进行双机局域网对战**；如需在局内按 `F2` 体验云端 AI 手绘生图锻造武器，请在 `Private/demo-credentials.json` 中填入您自己的 `lux3d_key` 与 `seedream_key` 后重启游戏。

---

## 🚀 近期重要迭代进度（11-22 ~ 11-26）

- **11-25 ~ 11-26**：完成实机战斗与正式录屏验证，将脱密后的「2026-11-24 正式录屏版」整包压缩分卷发布至 GitHub Releases。
- **11-24（正式录屏版打包）**：
  - 能量信标表现升级：携带信标时转为脚底增益光环（零视野遮挡），远距对手仍可通过高空细光柱定位持标者；
  - 网络与 UI 体验修复：本地与局域网通信自动绕过系统/VPN 代理，修复锻造状态控件遮挡 `F1` 退房菜单的问题；
  - 战斗三项修复：消除星见雅重复刀光特效、校准莱卡 1000m 狙击弹道与近战自动索敌表现。
- **11-23**：重建主城 `Lvl_ZZZ` 全图 788 组合批场景组件包围盒（彻底修复跑动时樱花树等物件隐身但碰撞体残留的问题），修复退出锻造台时输入锁计数泄漏导致的移动/视角卡死问题。
- **11-22**：接入新版「次元锻造机」手绘界面、锻造启动/循环/揭幕过场动画及独立 3D 展台 360° 检视流程，补充可恢复异常下的「继续原任务」断点重试按钮与多英雄右手挂点兼容。

---

## 📁 源码仓库说明（`main` 分支）

本 Git 分支存放《异界：溯源信标》（早期代号《画个锤子》）截至 **2026-11-21** 的无素材源码快照（战斗、程序化信标、GLB 武器导入与同步、局域网联机及房号索引服务源码），**仓库代码树本身不含大型美术资源与模型包；可运行的完整成品请直接下载上方 Release**。此前远端已有的验证原型仍保留原位置。

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

自带插件源码为 glTFRuntime（本地版本标识 `20260826`）与 TrioCombat；其余依赖由 UE 提供。公开工程描述中已去除开发机专用的 MCP 工具插件。安装对应引擎和 C++ 工具后，生成项目文件并编译 `Try_GLB_Put_IntoEditor`。这只验证代码构建；运行完整体验需要自行取得授权素材并按 [集成说明](docs/SOURCE_ONLY.md) 配置，或直接下载上方 Release 中的编译成品。

GLB 导入与装备主要位于 `WeaponGrip.cpp`、`WeaponForgeSubsystem.cpp`、`WeaponSyncComponent.cpp`。生成 API 的客户端保留；生成后台、SDK 运行包、模型权重、账号密钥和付费服务均不在源码分支中提供。不要把房号服务当作生成服务。

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

上述测试不调用云端模型生成接口，也不需要付费账号。代码发布范围及历史检查结果见 [发布记录](docs/SOURCE_ONLY.md)。

## 许可证与依赖

glTFRuntime 来自 [rdeioris/glTFRuntime](https://github.com/rdeioris/glTFRuntime)，按 [MIT License](unreal/Plugins/glTFRuntime/LICENSE) 保留版权和许可；本次复用现有工程中的插件版本，没有新增运行依赖。Unreal Engine 遵循 Epic 的许可条款，仓库不分发引擎。项目自有代码的整体开源许可证尚未指定；本次公开不为第三方素材或 SDK 授予再分发许可。依赖清单见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
