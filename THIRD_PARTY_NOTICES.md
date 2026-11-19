# Third-party notices

| Dependency | Source / license | Publication scope |
| --- | --- | --- |
| glTFRuntime | https://github.com/rdeioris/glTFRuntime ; MIT, Copyright (c) 2020 Roberto De Ioris | Existing project version `20260826`, source and descriptor retained with LICENSE. Plugin Content, binaries, editor resources and the upstream asset-based test suite/fixtures are omitted. Retained source files are copied unchanged; shader/material behavior without Content has not been validated. |
| Unreal Engine | Epic Games; Unreal Engine license | Not redistributed. Project targets UE 5.8. Engine modules referenced by Build.cs require a separately installed engine. |
| FastAPI | https://github.com/fastapi/fastapi ; MIT | Dependency declaration only. |
| Starlette | https://github.com/encode/starlette ; BSD-3-Clause | Dependency declaration only. |
| Uvicorn | https://github.com/encode/uvicorn ; BSD-3-Clause | Dependency declaration only. |
| Pydantic | https://github.com/pydantic/pydantic ; MIT | Dependency declaration only. |
| HTTPX | https://github.com/encode/httpx ; BSD-3-Clause | Dependency declaration only; used by tests. |
| pytest | https://github.com/pytest-dev/pytest ; MIT | Dependency declaration only; used by tests. |

`backend/requirements.lock` records the existing backend environment, including transitive packages. No Python environment or node_modules directory is included. No Aholo SDK copy or generation-service bundle is included. `TrioCombat` is retained as project/team code, with its original author headers; no new blanket license is applied. Asset names and paths identify integration points and do not license or distribute those assets.
