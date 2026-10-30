# 实际验证记录 · 2026-10-30

环境：Python 3.12、Linux、Chromium，服务 127.0.0.1:8000。全部为合成测试，不是玩家实验结果。

- `WEAPON_DATA_DIR=/tmp/weapon-final-check .venv/bin/python -m pytest -q`：4 passed，0 failed；1 条 Starlette/httpx 弃用提示。
- 后端覆盖模拟成功、文件访问、装备保存、应用重启后记录与抽卡不变、真实模式未配置时失败且无武器、模型阶段故障且不重抽、非法图片、无效 GLB 和 404。
- `node tests/browser-flow.cjs`：PASS。网页提交模拟任务、GLB 在 model-viewer 实际加载、装备标记、刷新恢复、真实失败可见、390px 页面无横向溢出、无 JavaScript 异常。
- `node test/1028_Lux3D_assumption_test/checks/browser-qa.cjs`：PASS。原站画板、失败记录、未完成不下 Go 结论、实际等待 20 秒揭晓、反馈保存、IndexedDB 刷新恢复、手机及平板布局。生成的历史截图已还原，避免覆盖归档证据。
- `git diff --check`：通过。

发现并修复：应用多次启动时 Queue 绑定旧事件循环；动态 model-viewer 在组件定义前设置 src 属性导致预览未加载。网页测试还修正了穿透模型组件 Shadow DOM 误选按钮的测试定位器。

未验证：任何真实供应商生成成功、云端部署、Unity/UE 导入和物理装备、真实武器模型质量。现有真实模式只提供 HTTP 桥接协议；需要供应商适配器及凭据后才能完整联调。
