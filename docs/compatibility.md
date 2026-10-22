# SDK 兼容性依据

依据核对日期：2026-10-21（Asia/Shanghai）。本文件集中记录官方要求、驱动适配方案与留存来源；实际环境见 [环境记录](environment.md)，实施任务见 [执行计划](../EXECUTION_PLAN.md#3-技术方案与兼容性关口)。

## P2适配与可选读音组合（2026-10-22）

固定模型、SDK、系统库及原P0 verified清单未变。正式LLM使用固定RKLLM 1.3.0及选定R的采样配置；P3通用能力提示调整后的原A04三批各19/20，旧R提示结果保持归档，见[环境 §53](environment.md#53-p3-01-完整文件链路与-a07-验收)。实际常驻、取消与生命周期证据见[环境 §48](environment.md#48-p2-模型适配端点与流式合成)；原 LLM/TTS 同进程局部实验只覆盖两模型；后续 P2-04 已完成四模型常驻 304.870 s / 44 轮，实际加载固定 RKNN 2.3.2、RKLLM 1.3.0、sherpa-onnx 1.12.14 与 ORT 1.17.1-patched，原始映射/哈希与结束路径复核一致，A06 通过。本次固定四模型文件负载无需 LLM 进程隔离，证据见[环境 §51](environment.md#51-p2-04-四模型共驻与五分钟验收)；模型/runtime 变化或加入 KWS 后需重验，原 P0 verified 清单范围不扩写。

词级“调低”补充是独立候选组合，登记在[pronunciation-candidates.json](../models/pronunciation-candidates.json)，绑定原lexicon/tokens哈希和应用规则哈希。显式CLI选择后在新run派生lexicon；原模型权重、lexicon及主manifest不变，也不扩大主manifest的verified范围。两上下文读音已确认；后续“搜索”由adapter标点上下文修正并完成正式A05复验，见[当前结果](environment.md#50-p2-03-搜索修正与-a05-复验)。诊断ORT输入捕获没有修改SDK，且不用于正式性能采样；接口与限制见[TTS文档](tts-probe.md#p2-流式队列与补充读音2026-10-22)。

本次前端策略绑定现有sherpa-onnx 1.12.14、固定tokens及FST：仅在adapter内部把中文全角逗号映射为同一comma音素的别名，避免该处独立声学推理；ASCII逗号与资产原文件不改。不是SDK二进制补丁或新词典。实际ORT输入验证音素/声调保持且blank位置正确；升级前端或词典组合需重新验证该约定。策略及证据见[TTS契约](tts-probe.md#2026-10-22-搜索修正与-a05-复验)。

## 官方要求与本机差距

**当前实测（2026-10-21）：**官方 Jammy 22.04.5 / BSP 6.1.99 运行 RKNPU 0.9.8、RKNN 2.3.2 / RKLLM 1.3.0。系统与私有 runtime/头文件、ABI、官方例程及实际加载证据已满足 P0-02；P0-03/04 与 M0 现已按分别推理的最小基线关闭，见[首个已验证组合](#p0m0-首个已验证组合2026-10-21)。历史转换未知项保留，分别成功不能证明共驻。下文早期阶段保留历史语义。


此前调查参考（保留历史版本，不作为当前库/头文件来源）：[瑞芯微 RKLLM SDK 1.3.1 中文手册](https://github.com/airockchip/rknn-llm/blob/main/doc/Rockchip_RKLLM_SDK_CN_1.3.1.pdf)。

| 条款 | 官方说明摘要 | 本项目结论 |
|---|---|---|
| 第 12 页 §2.2 | runtime 与对应的 rkllm.h 配套链接、部署 | 不单独更换库而保留不匹配的头文件 |
| 第 12 页 §2.3 | 推荐 GCC 10.2 工具链，不使用低于 10.2 的版本 | 核查板端原生编译器与链接依赖，不涉及安装 WSL |
| 第 12 页 §2.4 | 要求检查 RKNPU 驱动为 v0.9.8，低于 v0.9.8 时更新 | 当前实测 0.9.8，满足驱动前置要求；推理仍待验收 |
| 第 13 页 §2.4 续 | 驱动适配涉及 kernel-5.10 / kernel-6.1，参考版本 5.10.209 / 6.1.84，并给出驱动包与构建步骤 | 当前已运行配套官方 BSP 6.1.99，不继续混装旧系统候选内核 |

0.9.8 是 RKNPU 驱动版本，与 Ubuntu 24.04、Linux 6.1 的版本号不同。官方门槛已经明确，当前驱动前置已通过，剩余工作是具体模型/runtime 的板端验收和性能测试。

RKNN/Zipformer 所选版本的要求需另查其发布资料，这份 RKLLM 手册不能替代所有 SDK 的兼容矩阵。驱动适配、备份恢复、候选分支和推理验证顺序统一见执行计划 §3.2。

## P0-02 配套闭环复核（2026-10-21）

按原 P0-02 条件核查，固定 SDK 与官方例程、厂商驱动、头文件、ABI 和实际加载证据均齐，**P0-02 已完成**。当前 RKLLM 的直接依据是固定提交 `878f9361fd3afa7e167b7079918918f78d2c1c2a` 的 [1.3.0 英文手册](https://github.com/airockchip/rknn-llm/blob/878f9361fd3afa7e167b7079918918f78d2c1c2a/doc/Rockchip_RKLLM_SDK_EN_1.3.0.pdf)：第 13 页 §2.2 要求库/头文件配套，§2.3 要求 GCC 不低于 10.2；第 14 页 §2.4 要求 RKNPU 0.9.8。当前 GCC 11.4、glibc 2.35、实际厂商驱动 0.9.8 满足这些前置条件；BSP 6.1.99 的实际加载证据另行保留，不称为手册参考内核 6.1.84。

本地手册 `deps/model-prep/llm/upstream-1.3.0/doc/Rockchip_RKLLM_SDK_EN_1.3.0.pdf` 为 4,532,840 B，SHA-256 `29cbfcf307db952cc315a2aa8ffc2991014cc34be2262161fa604bc729e73e19`；复用既有完整校验，本次复核对应提取文本与固定 Git blob。40 份小源码匹配既有固定来源；48 项清单登记库/头文件等哈希匹配，系统两库与四头文件 6/6 对应私有版本；11 组 ELF / `ldd -r` 无缺库或未解析符号。

| 库 | 实际要求的最高符号版本 |
|---|---|
| RKNN 2.3.2 | GLIBC 2.17 / GLIBCXX 3.4.22 / CXXABI 1.3.8 |
| RKLLM 1.3.0 | GLIBC 2.29 / GLIBCXX 3.4.26 / CXXABI 1.3.11 / GOMP 4.0 |
| sherpa-onnx 1.12.14 C API | GLIBC 2.17 / GLIBCXX 3.4.18 / CXXABI 1.3.5 |
| ORT 1.17.1-patched | GLIBC 2.17 / GLIBCXX 3.4.19 / CXXABI 1.3.7 |

当前原生探针最高需要 GLIBC 2.34 / GLIBCXX 3.4.29，现机可满足；sherpa 使用同版 C API 规避已记录的旧 C++ 字符串 ABI，ASR 的非 PIC fbank 适配仅作用于其可选目标。实际库映射复用已核验的 ASR、LLM 进程记录，开发构建仍使用当前板端绝对私有路径；这不证明可搬移发布或 P2 同进程模型共驻。SDK 子任务未运行模型或编译。

证据：`output/runs/p0-sdk-20261021-144205/` 中 `report.json`、`evidence-catalog.json`、ABI/库头文件/来源核对；统一状态见[环境记录 §41](environment.md#41-p0-剩余任务核查与十问取消验证)。模型溯源、内容质量和 verified 组合由 P0-03/04 自身条件判断，不作为重新打开已满足 SDK 前置条件的理由。

## 指定 runtime 版本锁（2026-10-20）

本轮用户明确指定以下组合。通过 GitHub 官方 API 将 tag 解析为固定提交，再从该提交下载 Linux aarch64 库与同版头文件；上方 1.3.1 手册条款保留为此前调查依据，不作为本轮 1.3.0 头文件来源。

| 组件 | 固定官方来源 | 库 SHA-256 |
|---|---|---|
| RKNPU 0.9.8 | 当前运行内核 `6.1.99-rockchip-rk3588`，`/sys/module/rknpu/version` 实测 0.9.8 | 本轮未替换内核或驱动 |
| RKNN 2.3.2 | [v2.3.2](https://github.com/airockchip/rknn-toolkit2/tree/42aa1d426c0a9e0869b6374edba009f7208a1926)，commit `42aa1d426c0a9e0869b6374edba009f7208a1926`，`rknpu2/runtime/Linux/librknn_api/` | `d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8` |
| RKLLM 1.3.0 | [release-v1.3.0](https://github.com/airockchip/rknn-llm/tree/878f9361fd3afa7e167b7079918918f78d2c1c2a)，commit `878f9361fd3afa7e167b7079918918f78d2c1c2a`，`rkllm-runtime/Linux/librkllm_api/` | `6a9e4fc5324c68921c3a900340361e107af7599fe34dc8fa7759b2c5ae22a6e6` |

文件在 `deps/rknn-2.3.2/` 与 `deps/rkllm-1.3.0/`，清单为 `deps/manifest.json`；四个头文件、许可证和上游说明均记录大小与哈希。两库 `ldd -v` 无缺失依赖/版本；使用同版头文件编译 C++17 检查程序，`RTLD_NOW | RTLD_GLOBAL` 同进程加载成功，主要入口符号存在，`rkllm_createDefaultParam()` 调用成功。系统同名库安装后重新验证哈希和加载，默认 soname 解析到 `/lib/`（实路径为 `/usr/lib/` 的相同文件）。该检查未初始化模型、提交 NPU 推理或证明模型共存兼容。系统部署状态、备份和回退统一见[环境记录 §30](environment.md#30-npu-runtime-指定版本更新2026-10-20)。

## 本轮模型配套核查（2026-10-20）

| 组件 | 配套及验证边界 |
|---|---|
| Qwen3.5-2B / RK3588 / W8A8 | 社区权重与固定 revision 的 LFS SHA-256 一致；模型卡声明 Toolkit 1.3.0，实际转换输入 revision、完整校准张量未披露。官方 1.3.0 示例已编译，用户要求本轮不运行模型。[来源与限制](model-prep-llm.md) |
| Zipformer / RK3588 FP16 | Model Zoo v2.3.2 固定 commit bad6c7334531becaf90a561988519b7bec34d0ab，原始 ONNX 固定 HF revision；通过官方 ARM64 Toolkit2 2.3.2 私有环境在板端转换为 FP16。官方 C++ 文件推理成功，实际加载私有 RKNN runtime 2.3.2 / driver 0.9.8。[转换与版本记录](model-prep-asr.md) |
| Silero v5.0 / MeloTTS FP32 | 共用官方 sherpa-onnx 1.12.14 CPU aarch64 / ORT 1.17.1-patched，私有库加载和短例通过；TTS INT8 文件是上游 LFS 指针，实际使用完整 FP32 权重。[详细记录](model-prep-speech.md) |

所有模型保持 candidate。没有更换系统、内核、RKNPU 或固定 NPU runtime。各模型短例不能代替同进程共驻、流式接入及完整验收；LLM 本轮没有运行证据。[公共汇总](environment.md#35-模型准备汇总)。

## 文件探针配套实测（2026-10-20）

固定 [ASR](asr-probe.md) 与 [TTS](tts-probe.md) 文件工具已在当前 Jammy aarch64 原生编译。ASR 使用私有 RKNN 2.3.2 / driver 0.9.8；VAD 和 TTS 共用已固定的 sherpa-onnx 1.12.14 C API / ONNX Runtime 1.17.1-patched。探针在模型初始化前核对实际加载库的路径及清单哈希，并保留运行映射；没有替换系统运行库。

官方预编译 sherpa C++ wrapper 的旧 libstdc++ 字符串 ABI 与工程默认 ABI 不匹配，因此以同版 C API 封装 C++17 RAII，未改变全工程 ABI。官方 fbank 静态库未按 PIC 构建，ASR 探针单独使用 `-no-pie` 链接；此范围仅影响可选目标。两探针默认关闭，基础和独立构建不隐式依赖模型 SDK。

ASR 官方样本 5/5、分块一致性和 TTS 4 线程性能已有文件证据，但 ASR 固定集质量未达标、TTS 未试听；详细结果见[环境记录 §36](environment.md#36-固定测试集与文件模型探针)。候选状态不变。两工具没有链接 RKLLM 或初始化 LLM，也未证明全模型共驻、实时链路或取消可用。

## Qwen3.5-2B 最小配套实测（2026-10-21）

使用不变的 HanzoHuang 社区 W8A8 文件，RKLLM 1.3.0 的实际 loader 报告 `rkllm-toolkit version: 1.3.0`、`target_platform: RK3588`、`model_dtype: W8A8`、`max_context_limit: 4096`、`npu_core_num: 3`。这补上了此前仅由模型卡声明生成版本的缺口；本次请求上下文仍为 2048、输出上限 128，没有测试 4096 上下文能力。

初始化/三条中文推理/释放均返回 0；进程 maps 确认实际库为项目 `deps/rkllm-1.3.0/lib/librkllmrt.so`，SHA-256 与固定系统/私有版本锁相同。SDK 报告 runtime 1.3.0、driver 0.9.8，无新增内核日志错误。库与模型最小正常加载配套通过，原始转换 revision 和校准张量仍未知；模型继续 candidate。

SDK 警告 `rkllm_set_chat_template` 会禁用包括 `enable_thinking` 在内的内部自动模板处理。本次依靠显式完整的非思考后缀，三条未输出思考内容；不能宣称单独设置该 bool 已验证自动处理。观察到 token 248046 后正常 FINISH，但 SDK 未暴露具体 stop_reason 或模型内置 EOS 配置，完整 EOS/多轮/取消/共驻仍未验证。详见 [LLM 本次结果](model-prep-llm.md#2026-10-21-最小正常推理验证)、[环境记录 §38](environment.md#38-qwen35-2b-最小正常推理验证)。

## RKNPU 0.9.8 升级评估

**此前结论（重装前）：用户曾计划自行重装官方 Ubuntu 26.04，彼时实际镜像与 SDK 驱动兼容尚待核验。** 旧系统方案 2 第一阶段已完成，官方 BSP 6.1.99 / RKNPU 0.9.8 候选已并存安装并生成 initramfs，但从未启动；本轮只读实测仍为旧内核 / RKNPU 0.9.7。第一阶段安装通过不等于运行兼容通过，历史证据见[环境记录 §23](environment.md#23-第一阶段授权与安装执行)，候选差异见[下文](#官方-bsp-6199-离线核验与候选包)。旧方案不再作为默认下一步，不在新镜像自动复用其脚本。

菜单测试后续：用户再次完整观察启动过程，确认未看到内核选项；这更新了[第一次观察未确认](environment.md#242-旧内核重启复核与菜单观察)的状态，但不证明具体 U-Boot 根因。新机恢复与 Windows SSH 操作集中见[迁移交接](../MIGRATION_GUIDE.md)，本轮记录见[环境记录 §25](environment.md#25-重装镜像与-windows-codex-交接)。

### 重装 26.04 的交接与核验边界

2026-10-19 用户明确选择香橙派官方 Ubuntu 26.04 整镜像重装，工程自行上传 GitHub，不安排整卡备份。本轮没有接收最终 26.04 镜像或执行安装。此前公开目录列示的 `Orangepi5_1.2.4_ubuntu_resolute_desktop_gnome_linux6.18.49.7z` 与用户最终选择是否相同，仍需记录完整文件名、来源与校验值。

本轮重新读取[固定 next 配置](https://github.com/orangepi-xunlong/orangepi-build/blob/a95129bf30ea11886cc35f08ebb3e69695a26656/external/config/kernel/linux-rockchip-rk3588-next.config)，仍为 `CONFIG_DRM_ACCEL_ROCKET=m`，未列 `CONFIG_ROCKCHIP_RKNPU`。结合 [Rocket 用户态接口说明](https://www.kernel.org/doc/html/latest/accel/rocket/index.html)，不能把这项支持当作 RKLLM 1.3.1 所要求的厂商 RKNPU 0.9.8 已满足。这是对已查源码的边界判断，不是对尚未取得的最终镜像进行二进制鉴定；完整来源见[官方镜像核对](#官方固件与-ubuntu-2604-镜像核对)。

新系统先复查板型、内核、NPU 实际绑定驱动和版本；只有厂商接口及所选 SDK 的前提满足后，才验证配套 runtime/头文件、ABI 与最小模型。RKLLM、RKNN 和模型仍为待核验候选，当前资产目录没有可迁移的真实 SDK/模型。普通 C++ 构建与音频任务可独立恢复，不因驱动未满足而全部停止。

音频环境另有版本边界：旧机 WirePlumber 0.4.17 下的私有音频集成测试使用 0.4 组件/脚本，新系统若为 0.5 需验证并适配测试策略，不能把旧环境结果继承为新环境通过。[上游迁移说明](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/migration.html)；具体恢复与检查步骤统一见[迁移交接](../MIGRATION_GUIDE.md#7-构建与验证顺序)。

### 升级方式与版本锁

`modinfo rknpu` 显示 `filename: (builtin)`，本机配置为 `CONFIG_ROCKCHIP_RKNPU=y`。当前 0.9.7 随内核镜像启动，普通同名 `.ko` / DKMS 更新不能直接覆盖它，`modprobe -r` 也不能像模块那样卸载内置驱动；只更换用户态 `.so` 不改变内核驱动版本。可以取得他人构建的配套内核产物，不必在板端自行重建。

| 路线 | 核对结果 | 决策 |
|---|---|---|
| 当前 PPA 的常规内核升级 | 在线签名索引中 `linux-rockchip` / `linux-image-rockchip` 仍指向 `6.1.0.1025.25`；安装模拟无变更 | 普通 `apt upgrade` 不能解决本次驱动需求 |
| 显式选 PPA 的 1026 或源码分支的新版本 | 索引有 `6.1.0-1026.26` 镜像，但该 tag 的驱动仍为 0.9.7；`noble` 分支的驱动头文件也仍为 0.9.7 | 不能仅按内核包版本较新就认定已包含 0.9.8 |
| 同发行版源码适配 | [0.9.8 PR #36](https://github.com/Joshua-Riek/linux-rockchip/pull/36) 已关闭、未合并；其早期适配涉及 RK3576 OPP 回调 | 采用可追溯源码评估，不能视为发行版已支持 |
| 第三方独立 Noble 内核包 | 曾下载、核验并解包 [haoyangw/rockchip-bsp](https://launchpad.net/~haoyangw/+archive/ubuntu/rockchip-bsp) 的 `6.1.0-1005.5` 包，内置元数据标为 0.9.8；包和提取目录现已清理 | 基础内核为 6.1.115，缺少 Orange Pi 5 DTB 与部分当前已加载模块名称对应项，配置差异较大；不直接采用，评估摘要保留 |
| 第三方预编译 Ubuntu 镜像 | 曾提取 [darkice9x 的 master 发布](https://github.com/darkice9x/ubuntu-rockchip-modified/releases/tag/master)的 Orange Pi 5 镜像，确认为 Ubuntu 24.04.1、内核 6.1.0-1027.27 / 6.1.75，内置元数据标为 0.9.8；压缩镜像和提取目录现已清理 | 一项无线模块 MD5 不符尚未解释，未完成启动验证；不作为当前安装候选，失败差异摘要保留 |
| 该仓库的另一套升级包 | `OrangePi5Max` 发布确有 `.deb`，但标注 Orange Pi 5 Max、Ubuntu Jammy、5.10-1012，并含该板 U-Boot 包 | 与本板型号及当前内核组合不匹配，不使用这一套包 |
| 香橙派官方镜像 / BSP | 已核验 Jammy server / 6.1.99 镜像的 RKNPU 0.9.8；用户后续改选自行重装官方 26.04 | 旧第一阶段并存安装已完成、候选未启动；新 26.04 镜像实际接口/版本待核验，Rocket 不直接等于当前 SDK 兼容 |
| 更换其他发行版的内核包或系统镜像 | 当前启动、设备树和外围驱动兼容性未验证；未获迁移系统授权 | 只作候选比较，不直接混装或覆盖当前系统 |

### 免板端重编的候选路线

1. **取得同发行版配套的预编译内核包。** 可保留当前 Ubuntu 环境；须同时匹配镜像、modules、DTB、启动布局及来源。已找到独立 0.9.8 包，但本次核对的这一套缺少本板配套项，不能只因同为 Noble / arm64 就直接采用。
2. **从 Orange Pi 5 配套镜像提取（此前已选，完成第一阶段）。** 已核验官方 BSP 6.1.99 镜像并提取内核、同版本模块和本板 DTB，完成用于当前 Ubuntu 24.04 的独立候选包并存安装及本机 initramfs。旧 Noble 镜像评估摘要保留，大产物及旧排查目录已清理。候选未启动，用户现改走整镜像重装；此路线留作历史，不继续安排试启动。
3. **在已有其他主机或经授权的 CI 构建。** 仍属于自编译，但可免去板端构建资源开销；当前不假设另有 Linux 主机，不安装 WSL/虚拟机/容器，不擅自启动云端构建或推送代码。
4. **保留 0.9.7，另评估旧 SDK/模型组合。** 必须取得官方版本依据并做板端推理验证，当前没有冻结可用组合。Qwen3.5 在 RKLLM 1.3.0 才加入支持，不能把更旧的 SDK 当作当前 Qwen3.5 / RKLLM 1.3.1 路线的等价替代；改选模型须先确认目标变化。

以下固定源码和补丁是自编译候选，并非唯一升级方式；若选择预编译产物，应另锁定其实际版本、来源、哈希和验证记录。详细资产核对见[环境记录 §15](environment.md#15-免板端重编路线核对)。

### 官方固件与 Ubuntu 26.04 镜像核对

本节保留 2026-10-18 先前只读检查官方入口、公开网盘目录和固定源码时的依据，当次未下载所列系统镜像；后续 BSP 实取结果见下一节。**是否满足本项目要求，取决于具体板型的内核、厂商 RKNPU 与 SDK 配套情况；仅升级 Ubuntu 用户态发行版不能保证解决。** 原始目录响应及检查边界见[环境记录 §18](environment.md#18-官方固件与-ubuntu-2604-只读核对)。

| 发布方 / 路线 | 已核实内容 | 对本板的判断 |
|---|---|---|
| [瑞芯微官方固件入口](https://redmine.rock-chips.com/urllist) | 留存 RKLLM 手册第 12 页“官方固件地址”的真实超链接指向此处。通用 Linux SDK 下有 [Linux6.1](https://console.box.lenovo.com/l/mu2SrX)、[Linux6.12](https://console.box.lenovo.com/l/wJL9QN) 等目录；本次展开的 RK3588 子目录按 EVB 板型分类，RK3588S-EVB1-V10 下为 Buildroot / Debian / Yocto 固件 | 这些已检查目录没有 Orange Pi 5 成品；参考板的启动、设备树和外设配置不能直接视为本板适配，不刷写 EVB 固件。不据此断言全部官方目录都没有其他系统 |
| [香橙派 Orange Pi 5 官方下载页](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/service-and-support/Orange-pi-5.html)的 [Ubuntu 目录](https://drive.google.com/drive/folders/1i5zQOg1GIA4_VNGikFl2nPM0Y2MBw2M0) | 确有本板 Ubuntu 26.04 Resolute / GNOME / Linux6.18.49 镜像；这是板卡厂商发布，区别于瑞芯微 EVB 固件及此前第三方镜像 | 板型匹配，但公开构建配置走 Rocket；尚无此镜像满足 RKLLM 厂商驱动要求的证据，不作为直接修复方案 |
| 同一香橙派官方目录的 BSP 镜像 | 同时列有本板 Ubuntu Focal / Jammy 的 server、XFCE desktop / Linux6.1.99 镜像；对应公开 BSP 分支源码标为厂商 RKNPU 0.9.8 | 更值得优先核验实际内核产物及来源；镜像内部版本、包配套、当前 Noble 的安装可用性和启动均未验证，不能宣称可以直接混装 |
| Canonical 通用 Ubuntu 26.04 | [官方发布说明](https://canonical.com/blog/canonical-releases-ubuntu-26-04-lts-resolute-raccoon)说明默认采用 Linux 7.0 | 通用发行版默认内核与板厂镜像内核是不同事实；不能由“Ubuntu 26.04”推断 Orange Pi 5 的实际驱动版本 |

本次目录中的 26.04 文件为 `Orangepi5_1.2.4_ubuntu_resolute_desktop_gnome_linux6.18.49.7z`，公开文件 ID `1TxK3QZFOuFM_0I0T_tR9UB4awYOEsa-F`，目录列示 1,658,702,841 B，修改时间 2026-09-30。这里只取得目录元数据，**没有该归档的本机 SHA-256 或内部驱动检查结果**，不把文件名当作二进制验证。

固定源码依据：

- 香橙派构建提交 [`a95129bf30ea11886cc35f08ebb3e69695a26656`](https://github.com/orangepi-xunlong/orangepi-build/commit/a95129bf30ea11886cc35f08ebb3e69695a26656)明确加入 Ubuntu 26.04 / Debian 13 与 Linux 6.18；本板 `next` 选择 `orange-pi-6.18-rk35xx`，其 [next 配置](https://github.com/orangepi-xunlong/orangepi-build/blob/a95129bf30ea11886cc35f08ebb3e69695a26656/external/config/kernel/linux-rockchip-rk3588-next.config)为 `CONFIG_DRM_ACCEL_ROCKET=m`，没有 `CONFIG_ROCKCHIP_RKNPU` 项。
- 对应内核分支快照 [`f04628765c30c04e3515ddb8c0451589ff3a2579`](https://github.com/orangepi-xunlong/linux-orangepi/tree/f04628765c30c04e3515ddb8c0451589ff3a2579)的 Makefile 为 6.18.49，`drivers/` 列表无厂商 `rknpu/`，有 `accel/rocket/`。[内核官方说明](https://www.kernel.org/doc/html/latest/accel/rocket/index.html)将 Rocket 的用户态实现指向 Mesa Gallium。这套接口不能直接当作 RKLLM Runtime 所需的厂商驱动；“支持 RK3588 NPU”也不等于配套 `.rknn` / `.rkllm` 模型已验证。
- BSP 分支快照 [`f6eedab0cbe9b4be3f855661a0a7e7008812d40a`](https://github.com/orangepi-xunlong/linux-orangepi/tree/f6eedab0cbe9b4be3f855661a0a7e7008812d40a)的 Makefile 为 6.1.99，[厂商驱动头文件](https://github.com/orangepi-xunlong/linux-orangepi/blob/f6eedab0cbe9b4be3f855661a0a7e7008812d40a/drivers/rknpu/include/rknpu_drv.h)声明 `DRIVER_MAJOR=0`、`DRIVER_MINOR=9`、`DRIVER_PATCHLEVEL=8`；上述构建提交的 current 配置启用 `CONFIG_ROCKCHIP_RKNPU=y`。这是源码级候选依据，尚未证明下载目录内镜像使用这一精确提交。

[本板构建配置](https://github.com/orangepi-xunlong/orangepi-build/blob/a95129bf30ea11886cc35f08ebb3e69695a26656/external/config/boards/orangepi5.conf)允许 Resolute 同时选择 current / BSP 6.1 和 next / 6.18，所以定制的 Ubuntu 26.04 可以搭配厂商驱动路线；**可构建组合不等于现成镜像已经满足条件**。如果未来选择主线内核，还需单独评估厂商 RKNPU 移植、设备树及 SDK，超出“重装即解决”的范围。

用户交接的 BSP 6.1.99 已完成后续离线核验，结果见下节；官方来源不能代替本机 initramfs、外围设备和恢复条件。此前第三方评估与失败差异的摘要保留，旧大产物和排查运行目录已清理，见[环境记录 §22.1](environment.md#221-旧排查运行目录清理)。安装和迁移仍须用户明确确认。

### 官方 BSP 6.1.99 离线核验与候选包

2026-10-18，用户提供从[板厂百度目录](https://pan.baidu.com/s/1MMyK2cA54zV-swELYAu5yw?pwd=mjbi)下载的 `Orangepi5_1.2.4_ubuntu_jammy_server_linux6.1.99.7z`（公开提取码 `mjbi`）。已使用隔离工具完成解包、镜像校验及不挂载的分区读取；来源系统为 Ubuntu 22.04.5，仅提取内核组件，当前用户态仍为 Ubuntu 24.04。实际文件与证据路径见[环境记录 §21](environment.md#21-官方-bsp-镜像实取与离线候选包)。

| 核对项 | 离线结果 |
|---|---|
| 归档内容锁 | 621,852,720 B，与公开目录一致；SHA-256 `f9af8e19f8abfe8e55d763b31930d220a95975561d4b1d4112bf5221153cd20d`；7z CRC 通过 |
| 内部磁盘镜像 | 3,091,202,048 B；SHA-256 `d47109d059f47d8a3d33a7639715a82527dadbb46dd78b7d1e06f2e624a42426`，与归档内 `.img.sha` 一致。内部校验文件和本地哈希证明内容一致性，不是独立发布签名 |
| 内核与驱动 | `6.1.99-rockchip-rk3588`，来源 image / DTB 包均为 `1.2.4`；`CONFIG_ROCKCHIP_RKNPU=y`，内置元数据和隔离 `modinfo` 为 `0.9.8` |
| 源包载荷校验 | 对内核、模块和 DTB 完成 1,034 条 MD5 记录核对（包含 DTB 重复安装路径的记录），无缺失或差异；仅未选取四项原包文档。之后本地 `depmod` 更新候选索引，原始索引另存 |
| 模块配套 | 778 个 `.ko` 全部为 AArch64，vermagic 均匹配候选版本；`depmod -e -F System.map` 无符号警告。候选 `CONFIG_MODVERSIONS=n`，未做 CRC 校验，不据此宣称完整 ABI 或运行通过 |
| 本板设备树 | 包含 `rk3588s-orangepi-5.dtb`，model 为 Orange Pi 5，板型/NPU compatible 匹配；文件 SHA-256 `00cce7e6c0183043198d080771a05bf19bf5ed776e219ca4971a39d5c6750673`，与当前 DTB 不同，须使用候选成套文件验证 |
| 根存储与有线网络 | EXT4、MMC/SDHCI、DWMAC Rockchip 支持内置；实际启动与网络待验证 |
| 当前 USB Wi-Fi / 蓝牙 | `0bda:c820` 的无线接口别名解析到 `8821cu`，区别于当前实际绑定的 `rtw88_8821cu`；蓝牙接口仍解析到 `btusb`，依赖可解析。候选未启用压缩固件加载，已从同一官方镜像提取并核验未压缩 RTL8821C 固件及配置，放入候选内核专用固件目录；未修改现有固件 |
| GPU 与配置差异 | 与当前配置有 2,410 项差异，当前已加载模块中有九个名称不在候选中。当前 GPU 实际绑定 `mali`；`bifrost_kbase` 从 g18p0 / UK 1.18 改为 g25p0 / UK 1.31。现有 libmali 与桌面配套、无线及蓝牙音频均须试启动实测 |

已生成 `voice-engine-kernel-6.1.99-rockchip-rk3588_1.2.4+voice1_arm64.deb`，34,902,652 B（约 33.29 MiB），SHA-256 `c2efb9e45ccaf7c2e9805550c403edf9b23123bc6f5461c9e5159f1a43720fa8`。包使用独立名称，包含版本化的内核、模块、设备树和 RTL8821C 固件，不含镜像的 preinst / postinst 等维护脚本；919 个包内普通文件逐项哈希复核通过，归属均为 root，没有与当前系统普通文件冲突。所需 initramfs-tools、kmod、u-boot-menu 已安装。包不会自行生成 initramfs 或修改启动菜单。

打包前的 `candidate-root/` 副本已按用户要求清理；独立包、源归档与原核验记录保留，安装评审中的包路径未变。当前产物与恢复说明见[环境记录 §22](environment.md#22-可重建中间产物清理与整目录取舍)。

`install-review.json` 保留命令计划、写入路径、备份范围及回退约束，并已更新为第一阶段完成。2026-10-18 用户明确授权后，在开发板终端使用 sudo 执行固定候选安装脚本：备份原内核/模块/设备树和启动配置并验证，再安装独立包，直接用本机 `mkinitramfs` 生成新版本 initramfs；919 个安装文件哈希、模块符号检查、initramfs 内容与主压缩流完整性均通过。实际命令、备份与 initramfs 哈希及占用集中见[环境记录 §23](environment.md#23-第一阶段授权与安装执行)。没有调用启动菜单更新流程或重启，旧文件与默认入口保持不变；恢复演练、启动和 SDK/模型验证仍未完成。候选启动项预览尚未写入系统，第二阶段须先确认恢复方式并另获试启动授权，P0-02 继续进行中。

### 预编译产物与镜像提取评估

以下为此前两类第三方产物的历史评估摘要，原始本机排查目录已清理；历史结论见[环境记录 §16](environment.md#16-预编译产物实取与离线评估)，当前留存范围见[§22.1](environment.md#221-旧排查运行目录清理)。**原压缩镜像、PPA 包和两者提取目录已按用户要求清理，以下结果不代表当前仍有这些安装载荷；镜像提取路线改用官方 BSP 候选。**

| 项目 | 本板镜像提取 | 第三方独立 PPA 包 |
|---|---|---|
| 来源与版本 | darkice9x `master`，内核 `6.1.0-1027-rockchip` / 包版本 `6.1.0-1027.27`，基础 Linux 6.1.75 | haoyangw/rockchip-bsp Noble，内核 `6.1.0-1005-rockchip` / 包版本 `6.1.0-1005.5`，基础 Linux 6.1.115 |
| 驱动静态标识 | 配置为内置，`modules.builtin.modinfo` / 隔离 `modinfo` 为 0.9.8 | 配置为内置，`modules.builtin.modinfo` / 隔离 `modinfo` 为 0.9.8 |
| 本板设备树 | 已取得 `rk3588s-orangepi-5.dtb`，与当前文件逐字节相同 | 包中有其他板型 DTB，但缺少本板文件 |
| 配套与配置 | 模块架构/vermagic、符号依赖及当前已加载模块名称覆盖完成静态检查；与当前配置有少量差异，包括 CFG80211/MAC80211 从内置改为模块 | 当前已加载的部分模块名称无对应项，包括 8821cu/rtw88_8821cu；存在大量配置差异。名称缺失本身不证明没有其他替代驱动，但尚未建立替代关系 |
| 完整性与来源 | XZ 流完整性通过；包记录对照发现 `88XXau.ko` 的 MD5 不符，重新提取后复现。发布未提供独立签名/校验资产，不能把本地哈希当作作者真实性证明 | 用 Launchpad API 公布的 PPA key 在本机隔离目录核验签名链及包 SHA-256，未添加系统源或信任；只证明对应第三方源的产物一致性 |
| 当前决策 | 大产物已清理，历史校验差异未解决；没有生成本机 initramfs 或安装包 | 包及提取目录已清理，不直接用于本板 |

历史镜像输入固定为发布 ID `203475984`、资产 ID `234362702` 的 `ubuntu-24.04-preinstalled-desktop-arm64-orangepi-5.img.xz`，1,842,807,668 B；本机计算的 SHA-256 为 `d2ba70165e2edd570e3271f8ca92294a6ab8f83f1719166e623f76f9c2bee786`。这是内容锁，不是发布者提供的签名。原压缩文件、提取目录及旧排查运行目录均已清理，重现命令和逐文件原始校验记录不再留存；本节保存内容锁及已知失败差异的摘要，重现评估需重新取得输入并执行检查。清理范围见[环境记录 §22.1](environment.md#221-旧排查运行目录清理)。

镜像中的内核镜像、配置、System.map 和本板 DTB 与各自包 MD5 记录一致，差异限定到 `lib/modules/6.1.0-1027-rockchip/kernel/drivers/net/wireless/rtl8812au/88XXau.ko`：包记录为 `2eeacb4d6fb118cb6be1b7e39d6a4f45`，实际为 `ec99a035a4b48ec3e455736157e34cc0`。该模块的 vermagic 仍匹配 1027，但当前没有找到解释其变更的包记录或作者说明，不能因此免除完整性失败。

### 镜像提取路线的实施顺序

以下保留此前方案 2 的实施顺序：使用 Orange Pi 5 镜像的配套内核组件，在 Ubuntu 24.04 上并存新内核。归档校验、提取、静态核验、打包及第一阶段安装已完成，旧大产物已清理，候选实际运行尚未验证。2026-10-19 用户改走整镜像重装，下列未完成步骤不再作为当前待执行指令；当前入口见[重装交接](#重装-2604-的交接与核验边界)。

1. **接收并锁定输入（已完成）。** 用户将上述官方 Jammy server 归档放在 `output/runs/rknpu098-route2-storage-2026-10-18/`，已确认文件传输完成、大小及本机哈希，并通过归档和内部镜像校验。实际来源、哈希和版本锁集中见[官方候选结果](#官方-bsp-6199-离线核验与候选包)；隔离工具准备过程见[环境记录 §20](environment.md#20-方案-2-启动与旧产物清理)。
2. **核算空间并离线提取（已完成）。** 先列归档并按完整镜像、分区副本、提取/打包空间和至少 3 GiB 余量核算，再在运行目录提取配套内核、模块和 DTB。没有挂载系统镜像或执行镜像内程序。原始归档、独立包及证据保留；大中间文件及后续候选组件副本已清理，历史空间快照见环境记录 §21，当前清理范围见 §22，安装实际占用见 §23。
3. **核验驱动与本板配套（静态部分已完成）。** 已核验 RKNPU 内置标识、配置、模块架构/vermagic/符号、本板设备树及当前 USB 设备别名。配置和驱动变化已记录，实机网络、蓝牙音频与桌面验证仍待试启动。
4. **处理完整性差异（官方输入已核对）。** 源包记录核对通过；本地 `depmod` 对模块索引的重生成单独留证，原始文件另存。已清理的第三方 1027 产物不再作为当前安装候选，其 `88XXau.ko` 历史 MD5 差异仍未解释，更换输入不改写历史失败。
5. **制作独立候选包（已完成）。** 成套版本化文件已打包并检查，未混用 1025 的 `.ko`，未改写现有包或系统文件；包名、大小、哈希及无维护脚本的边界见上文。
6. **第一阶段安装（已授权并完成）。** 原内核与启动配置备份核验通过，独立候选包已并存安装，本机 initramfs 已生成并检查；原默认入口不变，没有重启。`install-review.json` 已关联实际执行与核验记录。未照搬来源镜像的 initramfs、extlinux、根 UUID、用户配置或 U-Boot；下一步确认可操作的恢复入口，再单独确认试启动。
7. **试启动并验证 SDK。** 经确认安装、试启动后，先复查内核/RKNPU 版本、SSH/网络和蓝牙音频，再执行配套 RKLLM / RKNN 最小模型验证。官方候选须独立验证其配置变化、网络依赖与 ABI，不能继承旧候选结论，也不能只看 0.9.8 版本号就关闭 P0-02。

### 自编译候选版本锁

首个源码候选固定为：

| 输入 | 固定版本 / 校验 |
|---|---|
| 本机内核的发行版源码 | [Ubuntu-rockchip-6.1.0-1025.25](https://github.com/Joshua-Riek/linux-rockchip/tree/7348ed5de414cc11a9831d269963d4a62f60b00e)，commit `7348ed5de414cc11a9831d269963d4a62f60b00e`；源码版本为 Linux 6.1.75 |
| 官方驱动归档 | [rknpu_driver_0.9.8_20241009.tar.bz2](https://github.com/airockchip/rknn-llm/blob/f7390530443bf84f0394255a449d7cbe81e69d1c/rknpu-driver/rknpu_driver_0.9.8_20241009.tar.bz2)，固定 RKLLM commit `f7390530443bf84f0394255a449d7cbe81e69d1c`；36,877 B；SHA-256 `188306a3eba7ca48c186fe2a39acf232dec8536484581e54e9b96c3f65677eba` |
| 候选适配补丁 | [rknpu-0.9.8-ubuntu-1025-rk3588.patch](../scripts/patches/rknpu-0.9.8-ubuntu-1025-rk3588.patch)；在覆盖官方驱动源码后应用，仅面向本板 RK3588/RK3588S |

官方手册对 6.1 分支建议 6.1.84，而当前发行版源码为 6.1.75；本候选属于对现有系统的适配尝试，不能称为完整满足官方参考内核组合。若后续确需换基础内核，须重新评估启动和设备兼容，不直接混用其他板卡的包。

### 源码预检查与边界

仅使用本机已安装的 `6.1.0-1025-rockchip` headers 编译驱动；headers 缺少的内部 `drivers/devfreq/governor.h` 从上述固定源码提交补到运行目录，额外 include 路径只对本次编译生效，不写入系统 headers。

官方源码在 `rknpu_devfreq.c` 的 `rk3576_npu_opp_data` 中引用了本机内核未提供的 `rockchip_opp_set_low_length`。候选补丁只删除该 RK3576 回调引用；RK3588 使用的 `rk3588_npu_set_soc_info` 保留。补丁不构成 RK3576 的适配，也不修改系统 OPP 实现。

补丁以 `--fuzz=0` 应用。编译命令采用外部目标 `CONFIG_ROCKCHIP_RKNPU=m ... rknpu.o`，只检查源码与头文件能否编译并合并为目标文件；**没有执行模块安装、最终内核链接或启动验证，也未把系统配置改成模块。** 实测命令、日志与结果集中在[环境记录 §13](environment.md#13-rknpu-098-升级预检查)。

### 完整构建、安装评审与恢复条件

以下保留旧系统的自编译准备和内核替换条件。自编译路线未完成完整构建，此前预编译路线第一阶段完成范围见[环境记录 §23](environment.md#23-第一阶段授权与安装执行)。用户现改走整镜像重装，不把本节备份/试启动步骤套用为本次迁移的前置任务；新机操作见根目录迁移交接。

1. 先按[内核专项资源预算](../EXECUTION_PLAN.md#81-资源控制)确定工作区、调试配置和开发依赖；板端具备低并发试编条件，但当前 SD 卡余量不足以保证原调试配置的完整构建。依赖及连带升级先交用户确认；随后下载并校验完整的固定内核源码，用本机配置和候选驱动构建。采用独立的新 kernel release / 包名，使镜像、modules、headers、DTB 各有独立版本目录；完整镜像、模块、设备树和包检查通过后，才提出内核安装操作。
2. 核对包内设备树及安装钩子。该源码的通用 `bindeb-pkg` 将 DTB 放在 `/usr/lib/linux-image-<release>/`；本机 `u-boot-update` 默认从 `/lib/firmware/<release>/device-tree/` 等路径寻找。进一步核对固定源码的[发行版打包规则](https://github.com/Joshua-Riek/linux-rockchip/blob/7348ed5de414cc11a9831d269963d4a62f60b00e/debian/rules.d/2-binary-arch.mk)已使用后一种布局，若转回自编译路线，应核验它实际生成的配置及包内容；该路线尚未生成或安装完整包，不能仅据规则推断可启动。板型必须保留为 `rk3588s-orangepi-5.dtb`。
3. 安装前向用户列出确切的包、依赖、系统写入、默认启动项变化和重启安排，取得明确确认；按实际写入范围备份原内核、模块、DTB 和启动配置，涉及安装钩子变更时另备份 `/etc/kernel/` 等相应配置。预编译第一阶段已取得授权，用户通过本机 sudo 执行，备份及核验已完成；没有调整启动菜单。进入试启动前仍须确认可用的本地菜单、串口或恢复介质，目前尚未确认或演练具体恢复入口。
4. 保留 `6.1.0-1025-rockchip` 镜像、initramfs、模块与固件目录，不覆盖原包，不执行 autoremove，不刷写 U-Boot/SPI。`u-boot-update` 按版本重新排序，原内核不一定仍为 `l0`；须逐项核验新旧入口及默认项后安排试启动。
5. 启动失败时，通过已确认的恢复入口选择原内核；无法进入菜单时，用恢复介质挂载根分区并恢复备份的启动配置。回到原系统后复核 `uname -r` 和 RKNPU 0.9.7，再移除失败候选；这是恢复设计，尚未实际演练。
6. 新内核启动后，复查驱动版本、NPU 枚举、内核错误、SSH/网络及音频/蓝牙基础状态；再验证配套 SDK/模型。版本显示 0.9.8 只完成驱动版本检查，P0-04 及模型验收仍须另行通过。

## 留存证据

- [已下载的官方手册](evidence/sdk/Rockchip_RKLLM_SDK_CN_1.3.1.pdf)：61 页，3,902,489 字节。
- 下载地址：`https://raw.githubusercontent.com/airockchip/rknn-llm/main/doc/Rockchip_RKLLM_SDK_CN_1.3.1.pdf`。
- SHA-256：`b05da74d3e6b6f47d940e962266b4f3079a087e9a3eb628329743b32e1a67146`。
- 已阅读第 12～13 页完整正文；驱动 0.9.7 来自本对话此前完成的 SSH 盘点，见 [environment.md](environment.md)。

主线文件可能变化；后续复查应比较哈希或固定官方提交版本。

## P0/M0 首个已验证组合（2026-10-21）

固定组合见 [verified-combination.json](../output/runs/p0-r-closeout-20261021-161053/verified-combination.json) 与 [独立复核](../output/runs/p0-r-closeout-20261021-161053/review/final-review.md)。本轮未变更驱动、runtime、头文件或模型；Qwen 实际加载 `deps/rkllm-1.3.0/lib/librkllmrt.so`，maps 和 loader 再次确认 1.3.0 / RKNPU 0.9.8。ASR 的 RKNN 2.3.2、CPU 的 sherpa-onnx 1.12.14 / ORT 1.17.1 配套继续复用既有 ABI/加载证据。

四模型和四依赖的清单 `status=verified`、`run_id=p0-r-closeout-20261021-161053`，另存原 `preparation_run_id`，范围明确为 P0/M0 单模型 SDK 最小可行性。用户明确使用社区预转换 LLM，不再追查其转换步骤；产物身份已固定，历史转换未知保持未知。完整 A04/A08、共驻库冲突、正式 adapter、实时与发布验收尚未完成。详细结果及边界集中见[环境记录 §43](environment.md#43-p0-收尾与首个已验证组合)。
