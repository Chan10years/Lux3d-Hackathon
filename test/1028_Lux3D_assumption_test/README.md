# 1028 · Lux3D 双假设验证归档

按 1028 命名整理；实际制作与发布于 2026-10-27 晚。

## 文件清单

| 路径 | 内容 |
| --- | --- |
| `dist/index.html` | 四个页面：创作、等待体验、盲评、结果 |
| `dist/app.js` | 画板、真实 GLB 录入、盲评、等待测试、存储及数据导入导出 |
| `dist/style.css` | 简洁响应式样式 |
| `dist/model-viewer.min.js` | 本地模型查看组件 4.1.0 |
| `使用说明.md` | 操作步骤、验证口径、运行方式、当前限制 |
| `docs/画个锤子_验证工单_20261027.md` | 用户提供的原始验证工单 |
| `docs/画个锤子_审查与策划案_v1.md` | 用户提供的原始审查与策划案 |
| `evidence/mobile.png` | 手机尺寸的浏览器 QA 截图 |
| `evidence/ipad.png` | 平板尺寸的浏览器 QA 截图 |
| `checks/browser-qa.cjs` | 本轮浏览器检查脚本，需要 Playwright 与 Chromium |
| `artifacts/initial-deployment.tar.gz` | 首次发布的构建包，仅本地留存，由 .gitignore 排除 |

## 今晚完成情况

- 已发布：https://huagechuizi-lux3d-lab.hazypearl32.chatgpt.site （私有站点，使用当前 ChatGPT 账号访问）。
- 已检查：手机与平板布局、画板事件、失败录入、未完成时不下 Go 结论、20 秒揭晓、反馈保存、IndexedDB 刷新恢复。
- 已检查统计边界：18/30 达到 60%、失败保留在分母、缺评审不下结论、额外样本不能替换同主体前三次。
- 尚未完成：真实 Lux3D 生成、真实三人盲评、非队员等待体验采样；无两个假设已通过的结论。
- 浏览器检查使用的是合成 QA 记录，截图不是参赛实验结果。
- 未接通自动生成 API，也未实现 AI 鉴定；当前可通过平台生成后导入 GLB 做质量验证。

## 如何运行检查

在此目录启动 `python3 -m http.server 8000 --directory dist`，另开终端运行 `node checks/browser-qa.cjs`。
脚本使用本机 `/usr/bin/chromium`，需要可加载的 Playwright；在独立浏览器上下文中使用合成数据，不写入线上用户记录。截图会覆盖 evidence 下的 QA 截图。

部署身份配置 `.openai/hosting.json` 留在仓库根目录，静态目录已指向本归档内的 dist。现有在线版本继续使用，未重新发布。首次构建包保留旧版原始路径，属于历史快照。
