# RK3588 原生语音引擎

面向 Orange Pi 5（RK3588S）的本地语音助手，使用 C++17 / CMake，在板端 Ubuntu 24.04 上原生编译。

## 当前进度

**P1-01 已完成并验证**：完整配置与按功能启用的资产校验、JSONL 日志、终端控制、独立 audio/engine 构建及无硬件音频 mock 均可运行。下一步为 P1-02 真实录放及 P1-03 固定语料准备；应用录放与模型推理尚未接入。完整任务状态见 [执行计划](EXECUTION_PLAN.md#5-工作分解依赖与交付)，实测与验证结果见 [环境记录](docs/environment.md#11-p1-01-完整工程骨架验证)。

## 文档入口

- [协作与文件同步规范](AGENTS.md)：各类文件归属、每次更新的同步要求与交付检查。
- [执行计划](EXECUTION_PLAN.md)：任务依赖、里程碑与交付条件。
- [环境记录](docs/environment.md)：系统、依赖版本和音频设备设置。
- [SDK 兼容性](docs/compatibility.md)：驱动与 RKLLM 要求。
- [验收用例](docs/ACCEPTANCE.md)：功能、延迟、资源和恢复验证。
- [规划审阅记录](docs/PLAN_REVIEW.md)：问题和修订依据。

## 项目目录

目录按应用源码、开发工具、文档、测试和本机资产划分：

```text
rk3588_voice_engine/
├── README.md                     # 项目入口、目录说明和检查命令
├── AGENTS.md                     # 项目协作与变更同步规范
├── EXECUTION_PLAN.md             # 当前执行计划
├── CMakeLists.txt                # CMake/Ninja 构建入口
├── src/                          # 应用实现，按模块划分
│   ├── main.cpp                  # 配置/资产检查与 mock 控制 CLI 入口
│   ├── audio/                    # 独立音频 mock；真实录放待开发
│   ├── asr/                      # VAD、语音识别适配
│   ├── llm/                      # 本地语言模型适配
│   ├── tts/                      # 语音合成与文本前处理
│   └── engine/                   # 配置、资产、JSONL 日志、mock 状态机
├── include/voice_engine/         # 配置、资产、音频、日志和控制接口
├── tools/                        # 独立的 C++ 检查与诊断工具
│   └── env_check.cpp             # 已通过的基础依赖检查源码
├── scripts/                      # 构建、检查、启动、回退脚本
├── config/                       # 通用配置；local.yaml 为本机配置
├── tests/
│   ├── fixtures/                 # 固定输入、参考结果和用例清单
│   ├── unit/                     # 单元测试
│   └── integration/              # 模块联调与完整链路测试
├── docs/
│   ├── environment.md            # 开发环境与已完成基线
│   ├── compatibility.md          # SDK、驱动和模型兼容性
│   ├── ACCEPTANCE.md             # 验收用例
│   ├── PLAN_REVIEW.md            # 规划审阅记录
│   ├── archive/                  # 历史计划，保留原始内容
│   └── evidence/
│       ├── environment/          # 板端音频、电脑资源 JSON 快照
│       └── sdk/                  # 已引用的官方 SDK 手册
├── models/                       # 模型、词典与 manifest.json
├── deps/                         # 私有 SDK、运行库与 manifest.json
├── build/                        # 本机编译产物（Git 忽略）
├── output/                       # 运行日志、验收结果（Git 忽略）
└── tmp/                          # 临时检查和中间文件（Git 忽略）
```

`config/default.yaml` 保存通用模板，`config/local.yaml` 保存本机完整配置。空目录用 `.gitkeep` 保留；模型与依赖资产清单尚未建立。

## 基础环境检查

在开发板仓库根目录执行：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/bin/env_check
```

输出 `ENV_CHECK_OK` 表示该检查完成。它只验证编译和库调用，不打开录音、播放或 NPU。

CMake 提供以下开关，默认均为 `ON`：

| 开关 | 构建内容与依赖 |
|---|---|
| `VOICE_ENGINE_BUILD_ENGINE` | `voice_engine` 与配置、资产、日志、控制库；依赖 yaml-cpp、nlohmann-json、pthread 和 coreutils 的 `sha256sum`，无需 OpenSSL 开发包 |
| `VOICE_ENGINE_BUILD_AUDIO` | `voice_engine_audio` 内存 mock，仅依赖 C++17 标准库；不依赖原生音频库 |
| `VOICE_ENGINE_BUILD_ENV_CHECK` | `env_check`，单独检查 libpulse、libsamplerate、libsndfile、yaml-cpp 和 nlohmann-json |

构建工具使用现有 CMake/Ninja 与 C++17 编译器；引擎依赖通过 pkg-config/CMake 查找。启用引擎测试时还需要 Python 3；`BUILD_TESTING=OFF` 可关闭测试目标。仅构建引擎并运行纯状态 mock：

```bash
cmake -S . -B build/engine-only -G Ninja \
  -DVOICE_ENGINE_BUILD_AUDIO=OFF -DVOICE_ENGINE_BUILD_ENV_CHECK=OFF
cmake --build build/engine-only -j2
ctest --test-dir build/engine-only --output-on-failure -E '^build_boundaries$'
./build/engine-only/bin/voice_engine --config config/local.yaml --mock --control stdin
```

仅构建音频 mock 及其单元测试：

```bash
cmake -S . -B build/audio-only -G Ninja \
  -DVOICE_ENGINE_BUILD_ENGINE=OFF -DVOICE_ENGINE_BUILD_ENV_CHECK=OFF
cmake --build build/audio-only -j2
ctest --test-dir build/audio-only --output-on-failure
```

`voice_engine_audio_interface` 是共享帧格式与抽象接口；音频实现和引擎实现分别链接，不要求在禁用模块后仍查找其依赖。

## 配置检查

```bash
./build/bin/voice_engine --config config/local.yaml --check-config
./build/bin/voice_engine --help
```

检查成功输出 `CONFIG_OK`、`ASSETS_OK models=N dependencies=N files=N` 和音频/日志摘要，退出码为 `0`；配置、资产或命令行错误退出码为 `2`，并提示相关文件或字段。`--help` 返回 `0`。`--check-config` 与 mock 控制模式互斥，不打开设备或 NPU。

每次只加载 `--config` 指定的一个完整 YAML 文件，不合并 `default.yaml` 与 `local.yaml`。`audio.source`、`audio.sink`、`audio.profile` 必须是非空字符串，source 不能以 `.monitor` 结尾；`sample_rate`、`channels`、`frame_ms` 缺省为 `16000`、`1`、`20`，首版只支持这组参数。未知字段、重复键和非法类型会被拒绝。

完整字段的缺省值、单位和数值范围集中在 [config/default.yaml](config/default.yaml) 的注释中。除必填音频设备字段外，各分组与字段均可省略；现有仅含 audio/logging 的 `local.yaml` 无须迁移。整数、布尔、字符串类型分别校验，字符串不得含 NUL。模板的设备字段留空，需填入设备名称或使用已有本机配置。

| YAML 分组 | 内容与类型 |
|---|---|
| `audio` | 设备名称/双向 profile 字符串；采样率、声道和帧长整数 |
| `logging` | 单个日志文件字节上限、保留文件数整数 |
| `models` | `vad/asr/llm/tts/kws` 各含 `enabled` 布尔及 `id` 字符串；默认全部禁用 |
| `assets` | 模型与依赖 manifest 路径字符串 |
| `vad` | 预录、最短语音、尾部静音、等待开口与最长发言的毫秒整数 |
| `threads` | 各模型模块的线程数整数 |
| `llm` | 上下文、输出和余量 token 数整数 |
| `tts` | 最短/最长分段字符数和冲刷等待毫秒整数 |
| `queues` | 采集/播放缓冲、待处理轮数、文本字节数、TTS 段数与音频时长整数 |
| `timeouts` | ASR、LLM 首 token/总生成、TTS、取消超时的毫秒整数 |
| `dialogue` | 唤醒/打断布尔开关、唤醒词字符串与对话窗口/打断持续毫秒整数 |

跨字段检查包括：预录不超过采集缓冲、最短语音不超过最长发言、TTS 最短分段不超过最长分段、LLM 输出加余量小于总上下文、首 token 超时不超过生成总超时；采集/播放缓冲必须为音频帧长的整数倍。启用模型时 `id` 必须非空；启用唤醒时要求 KWS 与非空唤醒词，启用打断时要求 VAD 和 ASR。这里定义并校验参数，模型线程、端点检测、分句及真实超时执行仍由后续任务接入。

### 资产清单

`--check-config` 只检查已启用模型及其声明的依赖，不自动下载。`models.<模块>.id` 绑定模型清单中的同名 `id`；所有模型禁用时不读取两份 manifest，输出的三个资产计数均为 `0`。仓库当前没有真实 `models/manifest.json` 和 `deps/manifest.json`，需要取得可追溯资产后再建立清单。

`assets.models_manifest` 与 `assets.deps_manifest` 可用绝对路径，或相对于所加载 YAML 所在目录的路径；每个 `files[].path` 则相对于对应 manifest 所在目录。资产文件必须是目录内的普通文件，禁止绝对路径、`..` 以及解析后逃出目录的符号链接；同一条目内不得重复声明同一文件。检查逐文件精确比较非负整数 `size_bytes` 和 64 个十六进制字符的 `sha256`，流式调用构建时找到的 `sha256sum`，不把大文件读入进程内存。

| 清单层级 | 必需字段与约束 |
|---|---|
| 模型清单根对象 | `schema_version: 1`（JSON 整数）、`models` 数组 |
| 选中模型条目 | 非空字符串 `id`、`source_url`、`revision`、`license`、`platform`、`quantization`、`toolkit_version`、`runtime_version`、`status`；`status` 取 `candidate/verified`，`verified` 必须含非空 `run_id`；若 candidate 也填写 `run_id`，同样须非空 |
| 模型依赖声明（可选） | `dependencies` 数组，每项含非空字符串 `id`、`version`；同模型内不重复，同依赖的不同版本要求会失败 |
| 依赖清单根对象 | `schema_version: 1`（JSON 整数）、`dependencies` 数组；仅存在实际依赖声明时读取 |
| 被引用依赖条目 | 非空字符串 `id`、`source_url`、`revision`、`license`、`version`；版本须精确匹配模型声明 |
| 两类条目的文件列表 | 非空 `files` 数组，每项含 `path`、`size_bytes`、`sha256` |

每份清单限制为 4 MiB、最多 64 层 JSON 嵌套，拒绝重复 JSON 键和重复条目 ID；允许添加转换命令、校准来源等扩展元数据。加载的清单会检查根结构和全部条目 ID，详细元数据、文件和依赖仅校验选中条目。计数中的 models/dependencies 分别按选中 ID 去重；files 为这些条目中实际校验的文件声明总数，同一文件被不同条目引用时分别计数。

元数据用于完整性、追溯记录与版本字符串匹配；程序不联网核实来源 URL、不验证 `run_id` 证据，也不判断平台/量化/SDK 字符串的兼容性。`candidate` 可以通过文件校验，`verified` 标签仍须按执行计划取得推理证据；`ASSETS_OK` 不证明 ABI、驱动、官方真实性或推理已经通过。

## 终端控制与 JSONL 日志

在项目根目录启动：

```bash
./build/bin/voice_engine --config config/local.yaml --mock --control stdin
```

看到 `MOCK_READY` 后，每行输入一条命令并按回车。此模式校验 YAML 后跳过资产读取，不加载模型或打开设备；即使配置启用了模型，也不会推理或生成回答。默认启用音频 mock 时，启动信息为 `audio_backend=mock`，每次 `start` 合成一帧 320 samples 的 16 kHz 单声道 float32 静音，仅将帧元数据写入日志；不持续采集或播放。关闭音频模块时为 `audio_backend=none`，保留纯状态控制。

| 命令 | mock 行为 |
|---|---|
| `help` / `status` | 查看命令 / 当前会话、generation 和状态 |
| `start` | 创建新 generation 并进入 `LISTENING`；重复执行取代旧轮次 |
| `stop` | 从 `LISTENING` 经 `FINALIZING` 回到 `IDLE`，表示本轮输入结束 |
| `cancel` | 使旧 generation 失效，回到 `IDLE` |
| `new-session` | 结束旧轮次并创建新会话 |
| `quit` | 正常退出；标准输入结束（EOF）也正常退出 |

也可一次运行完整演示，每次自动创建唯一 run ID：

```bash
printf 'status\nstart\nstop\nstart\ncancel\nnew-session\nquit\n' | \
  ./build/bin/voice_engine --config config/local.yaml --mock --control stdin
```

日志写入 `output/runs/<run_id>/events.jsonl`，启动信息给出本次目录。可用 `--output-dir ROOT` 更换根目录，或用 `--run-id ID` 指定 ID；ID 为 1～64 个 ASCII 字符，以字母或数字开头，其余只允许字母、数字、`_`、`.`、`-`。`ROOT/ID` 必须新建，已有目录会报错，以免覆盖旧记录。

每个事件独占一行 JSON 并立即 flush，字段为 `timestamp_utc`、`monotonic_ns`、`run_id`、`seq`、`level`、`event`、`session_id`、`generation_id`、`state`、`mode` 和 `data`；当前 `mode` 为 `mock`。到达大小限制前按需轮转，旧文件依次为 `events.1.jsonl`、`events.2.jsonl` 等。默认每次运行最多保留 10 份、每份 10 MiB；多次运行目录的总量尚无自动清理。日志不保存音频或对话内容；未知命令及超过 128 字节的输入会被拒绝，原文不写入日志。

正常 quit/EOF 返回 `0`，参数、配置或资产错误返回 `2`，运行目录创建、日志、输入 I/O 或音频 mock 运行错误返回 `1`。运行中收到 `SIGINT`（如 Ctrl+C）或 `SIGTERM` 时先记录 shutdown 事件，再分别退出 `130` 或 `143`。

## 版本管理约定

开发板 `/home/jzh/projects/rk3588_voice_engine` 是唯一日常代码、文档和 Git 工作目录；Windows 用作 SSH 入口及参考副本。远程版本保存在 [GitHub 公开仓库](https://github.com/jzh03/rk3588_voice_engine)，板端使用已登录的 GitHub CLI 通过 HTTPS 认证。账号配置和交接历史见 [环境记录](docs/environment.md#8-git-与云端托管)。

模型权重、运行库、录音、构建产物、临时文件和本机配置按 `.gitignore` 排除，资产只提交清单。开始工作先检查状态；工作区干净且需要同步远程时执行拉取。完成修改并验证后，只暂存本次相关文件，按用户授权提交和推送；日常 Git 命令不使用 sudo：

```bash
cd ~/projects/rk3588_voice_engine
git status --short --branch
git pull --ff-only
# 编辑并验证后，按具体文件名执行 git add、git commit 和 git push
```
