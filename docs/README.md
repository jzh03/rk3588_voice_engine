# 文档索引

从[项目首页](../README.md)了解功能与日常启动。

| 内容 | 入口 |
|---|---|
| 构建、配置、资产清单、开发控制与部署 | [构建与运行](build-and-run.md) |
| 耳机录放、设备等待与重连排查 | [音频诊断](audio-diagnostics.md) |
| 性能样例、固定参数与结果复算 | [性能快照](performance-snapshot.md)、[精简证据](evidence/performance-snapshot-20261027.json) |
| 部署命令和参数实现 | [部署脚本](../scripts/deploy.py) |
| 配置字段与默认值 | [默认配置](../config/default.yaml) |
| 模型和依赖身份 | [模型清单](../models/manifest.json)、[依赖清单](../deps/manifest.json) |
| 固定合成材料与生成条件 | [材料清单](../tests/fixtures/manifest.json)、[生成条件](../tests/fixtures/generator-lock.json) |

性能快照可直接用[复算脚本](../scripts/verify_performance_snapshot.py)检查时间差与完成计数。构建和运行所需模型、依赖及输入约定以对应清单为准。
