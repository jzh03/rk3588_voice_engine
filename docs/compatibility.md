# SDK 兼容性依据

依据核对日期：2026-10-16。本文件集中记录官方要求与留存来源；实际环境见 [环境记录](environment.md)，实施任务见 [执行计划](../EXECUTION_PLAN.md#3-技术方案与兼容性关口)。

## 官方要求与本机差距

来源：[瑞芯微 RKLLM SDK 1.3.1 中文手册](https://github.com/airockchip/rknn-llm/blob/main/doc/Rockchip_RKLLM_SDK_CN_1.3.1.pdf)。

| 条款 | 官方说明摘要 | 本项目结论 |
|---|---|---|
| 第 12 页 §2.2 | runtime 与对应的 rkllm.h 配套链接、部署 | 不单独更换库而保留不匹配的头文件 |
| 第 12 页 §2.3 | 推荐 GCC 10.2 工具链，不使用低于 10.2 的版本 | 核查板端原生编译器与链接依赖，不涉及安装 WSL |
| 第 12 页 §2.4 | 要求检查 RKNPU 驱动为 v0.9.8，低于 v0.9.8 时更新 | 本板实测 0.9.7，低于要求，不能列为满足 RKLLM 1.3.1 官方前提 |
| 第 13 页 §2.4 续 | 驱动适配涉及 kernel-5.10 / kernel-6.1，参考版本 5.10.209 / 6.1.84，并给出驱动包与构建步骤 | 当前内核 6.1.0-1025-rockchip 需要匹配的内核来源、补丁或更新包，以及恢复方法 |

0.9.8 是 RKNPU 驱动版本，与 Ubuntu 24.04、Linux 6.1 的版本号不同。官方门槛已经明确，剩余工作是本发行版的驱动适配、具体模型/runtime 的板端验收和性能测试。

RKNN/Zipformer 所选版本的要求需另查其发布资料，这份 RKLLM 手册不能替代所有 SDK 的兼容矩阵。驱动适配、备份恢复、候选分支和推理验证顺序统一见执行计划 §3.2。

## 留存证据

- [已下载的官方手册](evidence/sdk/Rockchip_RKLLM_SDK_CN_1.3.1.pdf)：61 页，3,902,489 字节。
- 下载地址：`https://raw.githubusercontent.com/airockchip/rknn-llm/main/doc/Rockchip_RKLLM_SDK_CN_1.3.1.pdf`。
- SHA-256：`b05da74d3e6b6f47d940e962266b4f3079a087e9a3eb628329743b32e1a67146`。
- 已阅读第 12～13 页完整正文；驱动 0.9.7 来自本对话此前完成的 SSH 盘点，见 [environment.md](environment.md)。

主线文件可能变化；后续复查应比较哈希或固定官方提交版本。
