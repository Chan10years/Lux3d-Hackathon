# Lux3d-Hackathon

「画个锤子」Lux3D 黑客松项目。

今晚的假设验证原型、输入文档、截图和检查脚本已归档到 [test/1028_Lux3D_assumption_test](test/1028_Lux3D_assumption_test/README.md)。

在线验证台：https://huagechuizi-lux3d-lab.hazypearl32.chatgpt.site

从仓库根目录启动：

```sh
python3 -m http.server 8000 --directory test/1028_Lux3D_assumption_test/dist
```

本地 Python/FastAPI 武器制作流程、配置、API 契约与限制见 [本地运行说明](docs/local-workflow.md)。新增功能请使用该说明中的 uvicorn 启动命令；原静态启动命令只运行旧实验页面。
