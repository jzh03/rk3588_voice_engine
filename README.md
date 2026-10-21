# RK3588 原生语音引擎

面向 Orange Pi 5（RK3588S）的本地语音助手，使用 C++17 / CMake，在板端原生编译。当前系统为官方 Orange Pi 1.2.4 / Ubuntu 22.04.5 LTS / BSP 6.1.99；已完成网络、时间与构建依赖恢复，见[当前环境](docs/environment.md#26-官方-jammy-系统网络时间与构建依赖恢复)。

## 当前进度

**P0-01、P0-02 已完成**：开发与构建基线复用既有证据；固定 RKNPU 0.9.8 / RKNN 2.3.2 / RKLLM 1.3.0 的驱动、头文件、ABI、官方例程和实际加载路径已核对闭环。系统和私有 SDK 组合保持不变。配套依据见[兼容性记录](docs/compatibility.md#p0-02-配套闭环复核2026-10-21)，备份和回退见[环境记录 §30](docs/environment.md#30-npu-runtime-指定版本更新2026-10-20)。

**P0-01～P0-04 已完成，M0 技术可行通过**：四类模型及四项私有依赖已冻结为首个 `verified` 组合，范围限于 P0 的资产、SDK 配套和各模型最小推理。选定 R 配置复用固定十问两批 10/10，本轮主动取消至空闲 97.467 ms、清 KV 后同句柄恢复成功。用户确认直接使用社区预转换 LLM，未公开的转换步骤保留未知记录，不再追查或阻塞 P0。完整应用验收仍按原标准执行；见[收尾结果](docs/environment.md#43-p0-收尾与首个已验证组合)和[冻结组合](output/runs/p0-r-closeout-20261021-161053/verified-combination.json)。

**P1-01 已在新系统复核；P1-02 进行中**：Release 构建、ENV_CHECK_OK 与 8 项通用 CTest 通过；新系统音频集成测试已运行但失败。已完成两套音频服务对照并恢复 PipeWire；AP6275P M.2 已启用，Wi-Fi/蓝牙扫描通过；vivo TWS 4 播放及 10 秒录音回放已由用户确认正常。按用户要求完成约一分钟双向检查，未见断连/溢出/欠载，用户确认听感正常；短测时长偏差保留、不继续排查。耳机出盒后自动重连，双向节点及本机配置已恢复；A01/A02 完整验收尚未完成。模型推理尚未接入实时引擎。[一分钟检查与重连](docs/environment.md#34-ap6275p-耳机长时稳定性及重连检查)；[此前音频排查](docs/environment.md#31-音频链路恢复)。

**P1-03 工程回归集已交付；P2-01/03 文件探针已实现**：固定 20 条合成语音、6 条边界、10 条 TTS 文本及 20 个静态 LLM 问题；`asr_probe` 支持增量分块/reset，`tts_probe` 支持分句和 44.1 → 16 kHz 转换。ASR 三批各 13/20 条关键词全对、中文 CER 15.96%，未达质量门槛；10/20/37 ms 分块结果一致。TTS 4 线程 RTF P95 0.883 达标，2 线程 1.326 未达标，尚未试听。测试集为合成工程回归集，不代表自然语音独立验收。[本轮结果与限制](docs/environment.md#36-固定测试集与文件模型探针)。

参考项目源码审阅已归档，短首分句、常驻 LLM adapter、原子输出提交及 ASR/VAD 端点接入建议已纳入执行计划，均待实施；见[参考项目总结](doc/voicenpu-reference-review.md)。参考建议仍按执行计划分阶段验证。

## 文档入口

- [协作与文件同步规范](AGENTS.md)：Windows Codex / SSH / 板端开发分工、各类文件归属、每次更新的同步要求与交付检查。
- [执行计划](EXECUTION_PLAN.md)：任务依赖、里程碑与交付条件。
- [参考项目总结与实施建议](doc/voicenpu-reference-review.md)：Windows 参考源码索引、四项借鉴建议、限制与后续验证。
- [新镜像迁移交接](MIGRATION_GUIDE.md)：Windows Codex 的 SSH 配置、板端依赖与工程恢复步骤。
- [环境记录](docs/environment.md)：系统、依赖版本和音频设备设置。
- [SDK 兼容性](docs/compatibility.md)：驱动、SDK 与模型配套。
- 模型准备：[Qwen3.5-2B](docs/model-prep-llm.md)、[Zipformer](docs/model-prep-asr.md)、[Silero VAD 与 MeloTTS](docs/model-prep-speech.md)。
- 文件测试：[固定测试集](docs/file-fixtures.md)、[VAD/ASR 探针](docs/asr-probe.md)、[TTS 探针](docs/tts-probe.md)。
- [验收用例](docs/ACCEPTANCE.md)：功能、延迟、资源和恢复验证。
- [规划审阅记录](docs/PLAN_REVIEW.md)：问题和修订依据。

## 项目目录

目录按应用源码、开发工具、文档、测试和本机资产划分：

```text
rk3588_voice_engine/
├── README.md                     # 项目入口、目录说明和检查命令
├── AGENTS.md                     # 项目协作与变更同步规范
├── EXECUTION_PLAN.md             # 当前执行计划
├── MIGRATION_GUIDE.md            # 重装后 Windows/板端配置与 Codex 交接
├── CMakeLists.txt                # CMake/Ninja 构建入口
├── cmake/                        # 可选 ASR/TTS 探针的私有依赖与测试目标
├── src/                          # 应用实现，按模块划分
│   ├── main.cpp                  # 配置/资产检查与 mock 控制 CLI 入口
│   ├── audio/                    # 音频 mock、流式 DSP 与 PulseAudio 异步录放
│   ├── asr/                      # VAD、语音识别适配
│   ├── llm/                      # 本地语言模型适配
│   ├── tts/                      # 语音合成与文本前处理
│   └── engine/                   # 配置、资产、JSONL 日志、mock 状态机
├── include/voice_engine/         # 配置、资产、音频、日志和控制接口
├── tools/                        # 独立的 C++ 检查与诊断工具
│   ├── env_check.cpp             # 基础依赖检查
│   ├── audio_probe.cpp           # 真实录放、格式转换与设备恢复诊断
│   ├── asr_probe.cpp             # VAD/RKNN 固定 WAV 与增量分块诊断
│   └── tts_probe.cpp             # CPU 合成、分句、原生/16 kHz WAV 诊断
├── scripts/                      # 构建、检查、启动、回退脚本与适配补丁
│   ├── install_npu_runtimes.py    # 本机固定版本 runtime 安装、检查与回退
│   ├── prepare_fixtures.py       # 固定工程语料生成与原生 WAV 重建
│   └── patches/                  # 固定源码版本的候选驱动补丁
├── config/                       # 通用配置；local.yaml 为本机配置
├── tests/
│   ├── fixtures/                 # 固定输入、参考结果和用例清单
│   ├── unit/                     # 单元测试
│   └── integration/              # 模块联调与完整链路测试
├── doc/                         # 按本次约定保存参考项目总结
│   └── voicenpu-reference-review.md # 参考源码审阅与实施建议
├── docs/
│   ├── environment.md            # 开发环境与已完成基线
│   ├── compatibility.md          # SDK、驱动和模型兼容性
│   ├── model-prep-llm.md          # 2B 来源、固定十问、取消与验证限制
│   ├── model-prep-asr.md          # Zipformer 转换、文件推理与限制
│   ├── model-prep-speech.md       # VAD/TTS 私有依赖及最小验证
│   ├── file-fixtures.md          # 固定合成工程集、来源、哈希和限制
│   ├── asr-probe.md              # 增量文件 ASR、测量和未达标样本
│   ├── tts-probe.md              # 分句/合成/转换接口与线程性能
│   ├── ACCEPTANCE.md             # 验收用例
│   ├── PLAN_REVIEW.md            # 规划审阅记录
│   ├── archive/                  # 历史计划，保留原始内容
│   └── evidence/
│       ├── environment/          # 板端音频、电脑资源 JSON 快照
│       └── sdk/                  # 已引用的官方 SDK 手册
├── models/                       # llm/asr/vad/tts 资产与 manifest.json
├── deps/                         # 私有 SDK、运行库与 manifest.json
│   └── model-prep/               # 各任务私有示例、构建和转换环境
├── build/                        # 本机编译产物（Git 忽略）
├── output/                       # 运行日志、验收结果（Git 忽略）
└── tmp/                          # 临时检查和中间文件（Git 忽略）
```

`config/default.yaml` 保存通用模板，板端 `config/local.yaml` 已保存耳机参数，模型仍全部禁用；本轮未修改该配置。独立文件探针使用 `config/file-probes.yaml`，只启用 VAD/ASR/TTS，LLM/KWS 禁用；其中的文件输入输出标签不能用于真实录放。默认模板的设备字段为空，不能直接用于启动。空目录用 `.gitkeep` 保留；模型与依赖分别登记于 `models/manifest.json`、`deps/manifest.json`。模型、运行库、私有构建/转换环境和运行证据仍被 Git 忽略。

## 基础环境检查

在开发板仓库根目录执行：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/bin/env_check
```

输出 `ENV_CHECK_OK` 表示该检查完成。它只验证编译和库调用，不打开录音、播放或 NPU。

CMake 基础开关默认 `ON`；两个模型文件探针开关默认 `OFF`：

| 开关 | 构建内容与依赖 |
|---|---|
| `VOICE_ENGINE_BUILD_ENGINE` | `voice_engine` 与配置、资产、日志、控制库；依赖 yaml-cpp、nlohmann-json、pthread 和 coreutils 的 `sha256sum`，无需 OpenSSL 开发包 |
| `VOICE_ENGINE_BUILD_AUDIO` | `voice_engine_audio` 内存 mock，仅依赖 C++17 标准库；与下一开关共同决定是否构建真实后端 |
| `VOICE_ENGINE_BUILD_PULSE_AUDIO` | 音频模块开启时构建 `voice_engine_audio_dsp` 和 `voice_engine_pulse_audio`，依赖 libpulse、libsamplerate 和 pthread；引擎也开启时构建 `audio_probe`，另需 libsndfile |
| `VOICE_ENGINE_BUILD_ENV_CHECK` | `env_check`，单独检查 libpulse、libsamplerate、libsndfile、yaml-cpp 和 nlohmann-json |
| `VOICE_ENGINE_BUILD_ASR_PROBE`（OFF） | VAD/RKNN 文件探针；显式开启后检查固定私有 RKNN、sherpa/ORT 与官方 fbank 依赖 |
| `VOICE_ENGINE_BUILD_TTS_PROBE`（OFF） | CPU TTS 文件探针；显式开启后检查固定私有 sherpa/ORT，复用音频 DSP 与 libsndfile |

构建工具使用现有 CMake/Ninja 与 C++17 编译器；引擎依赖通过 pkg-config/CMake 查找。启用引擎测试时还需要 Python 3；真实音频集成测试另需 `pipewire`、`pipewire-pulse`、`wireplumber`、`pactl`，测试服务在独立临时目录运行。`BUILD_TESTING=OFF` 可关闭测试目标。仅构建引擎并运行纯状态 mock：

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
  -DVOICE_ENGINE_BUILD_ENGINE=OFF -DVOICE_ENGINE_BUILD_PULSE_AUDIO=OFF \
  -DVOICE_ENGINE_BUILD_ENV_CHECK=OFF
cmake --build build/audio-only -j2
ctest --test-dir build/audio-only --output-on-failure
```

保留真实音频、关闭引擎和环境检查，可独立构建原生后端及 DSP 测试：

```bash
cmake -S . -B build/native-audio-only -G Ninja \
  -DVOICE_ENGINE_BUILD_ENGINE=OFF -DVOICE_ENGINE_BUILD_ENV_CHECK=OFF
cmake --build build/native-audio-only -j2
ctest --test-dir build/native-audio-only --output-on-failure
```

该构建不需要 yaml-cpp、nlohmann-json 或 libsndfile，也不生成 `audio_probe`。`voice_engine_audio_interface` 是共享帧格式与抽象接口；音频实现和引擎实现分别链接，不要求在禁用模块后仍查找其依赖。

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

`--check-config` 只检查已启用模型及其声明的依赖，不自动下载。`models.<模块>.id` 绑定模型清单中的同名 `id`；所有模型禁用时不读取两份 manifest，输出的三个资产计数均为 `0`。两份清单已合并本轮实际取得的模型及 RKNN 2.3.2 / RKLLM 1.3.0 / sherpa-onnx 1.12.14 / ORT 1.17.1-patched 依赖；当前模型和依赖已按 `verification_scope` 标记为 P0/M0 最小基线 verified。各模型阶段结果见专属准备记录。文件校验通过不代表推理验收；文件探针的适配器可单独运行，实时控制入口尚未接入它们。

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

元数据用于完整性、追溯记录与版本字符串匹配；程序不联网核实来源 URL、不验证 `run_id` 证据，也不判断平台/量化/SDK 字符串的兼容性。`candidate` 可以通过文件校验，`verified` 标签仍须按执行计划取得推理证据，并读取条目的 `verification_scope`；当前组合只覆盖 P0/M0，范围字段不由程序强制执行；`ASSETS_OK` 不证明 ABI、驱动、官方真实性或推理已经通过。

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

每个事件独占一行 JSON 并立即 flush，字段为 `timestamp_utc`、`monotonic_ns`、`run_id`、`seq`、`level`、`event`、`session_id`、`generation_id`、`state`、`mode` 和 `data`；控制 mock 的 `mode=mock`，真实音频诊断的 `mode=audio_probe`。到达大小限制前按需轮转，旧文件依次为 `events.1.jsonl`、`events.2.jsonl` 等。默认每次运行最多保留 10 份、每份 10 MiB；多次运行目录的总量尚无自动清理。日志不保存音频或对话内容；未知命令及超过 128 字节的输入会被拒绝，原文不写入日志。

正常 quit/EOF 返回 `0`，参数、配置或资产错误返回 `2`，运行目录创建、日志、输入 I/O 或音频 mock 运行错误返回 `1`。运行中收到 `SIGINT`（如 Ctrl+C）或 `SIGTERM` 时先记录 shutdown 事件，再分别退出 `130` 或 `143`。

## 模型文件诊断

在板端工程根目录一次构建两个可选工具：

```bash
cmake -S . -B build/model-probes -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVOICE_ENGINE_BUILD_ASR_PROBE=ON -DVOICE_ENGINE_BUILD_TTS_PROBE=ON
cmake --build build/model-probes -j2
ctest --test-dir build/model-probes --output-on-failure -E '^audio_probe$'
python3 tests/integration/check_fixtures.py
```

这组 CTest 不初始化模型或访问音频设备；明确排除新系统已有失败的 `audio_probe` 音频集成项，不将它记为本轮通过。完整测试仍用不带 `-E` 的命令，其音频问题单独处理。默认构建不要求私有模型 SDK，三种独立构建边界继续保留。

以下只读取固定文件、保存结果。每次把 `NEW_ASR_RUN`、`NEW_TTS_RUN` 改成新 run ID；`--output-dir` 是父目录，已有 `<父目录>/<run-id>` 会被拒绝。初始化、推理和采样全程共用同一锁，未持有锁时程序在模型初始化前退出 2。

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  timeout 180 build/model-probes/bin/asr_probe \
  --config config/file-probes.yaml --manifest tests/fixtures/manifest.json \
  --chunk-ms 20 --repeat 3 --output-dir output/runs --run-id NEW_ASR_RUN

flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  timeout 900 build/model-probes/bin/tts_probe \
  --config config/file-probes.yaml --cases tests/fixtures/tts_cases.json \
  --threads 4 --repeat 3 --warmup 1 --output-dir output/runs --run-id NEW_TTS_RUN
```

ASR 的 `--official --repeat 5` 可代替 `--manifest` 复核官方 WAV，`--chunk-ms 10/20/37` 检查分块一致性。TTS `--validate-text-only` 只验证文本和分句。两工具不播放 WAV、不录音、不执行 LLM。退出 6 表示本次质量/行为或性能指标未达标，应保留失败报告；退出 0 也不等于 A03/A05 全项通过。外层 `timeout` 返回 124 时保留部分证据并记为未完成。输入契约、其它错误码、耗时口径和已知限制见 [ASR](docs/asr-probe.md)、[TTS](docs/tts-probe.md)；固定数据、参考和哈希见[测试集说明](docs/file-fixtures.md)。

## 真实音频诊断

使用现有 `config/local.yaml` 的耳机参数，在开发板终端执行：

```bash
./build/bin/audio_probe --config config/local.yaml --duplex --duration-sec 10
```

`--duplex` 同时采集与播放，另可选择 `--capture-only` 或 `--playback-only`，三者必须且只能指定一个。默认运行 10 s，`--duration-sec` 接受 1～3600 s，从首次设备就绪时开始计时；`--wait-device-sec` 设置首次等待设备的上限，范围 1～60 s、默认 10 s。自动模式在设备首次就绪后开始录放。

若输出 `exit_code=3 reason=device_unavailable`，先在系统中连接耳机，确认蓝牙设备的 `Connected` 为 `yes`；已配对、已信任仍可能处于断开状态。再用 `pactl list short sources` 和 `pactl list short sinks` 核对配置中的两个节点均已出现、活动 profile 与配置一致，随后重试。具体等待原因记录在本次 `events.jsonl` 的 `device_wait` 事件中。

程序只连接配置中的 source/sink，并核验设备身份及当前 profile；禁止 monitor，不修改系统默认路由或 profile，不回退到 HDMI/其他声卡。即使只录音或只播放，也核验完整设备对。原生设备采样率/声道与内部 16 kHz、单声道、20 ms、归一化 float32 帧之间执行流式转换；最后不足一帧时补零并保留有效样本数。采集与播放使用配置中的有界队列，采集回调不推理或写磁盘。

播放默认每 5 s 发出一段 300 ms 的低音量提示音；也可用 `--play-wav FILE` 播放本机测试 WAV。默认采集仅在内存中处理，只有显式添加 `--record-wav` 才保存本次目录内的 `capture.wav`（16 kHz 单声道 PCM16）；交互模式的多次录音会连续写入同一文件。

交互诊断从空闲状态启动，便于检查重连：

```bash
./build/bin/audio_probe --config config/local.yaml --duplex \
  --duration-sec 600 --control stdin
```

| 命令 | 真实音频诊断行为 |
|---|---|
| `start` | 设备 READY 后启动新 generation，清空旧缓冲并开始所选模式 |
| `stop` | 停止采集和生产新播放帧，排空已提交音频后回到空闲 |
| `cancel` | 使旧 generation 失效并 flush 待播放音频 |
| `status` / `help` | 输出设备、格式、队列及计数 / 查看命令 |
| `quit` | 结束本次诊断；EOF 也正常结束 |

断线或 profile 变化后取消旧轮次、清空 PCM 和转换状态，按 1/2/5 s 退避重新查找；重建流后回到 READY，必须重新输入 `start`，不续播旧音频。非交互模式中途断线会使本次诊断失败，即使设备重连也不计为连续录放完成。

运行目录沿用上文的 `--run-id`、`--output-dir` 和不覆盖旧目录规则。`run.json` 保存配置/可执行文件哈希及参数；`events.jsonl` 保存设备属性、协商格式/缓冲、状态事件与每秒计数；`summary.json` 汇总有效样本、时长误差、RMS/峰值、队列峰值及溢出/断流/重连计数。成功返回 `0`，参数/配置错误 `2`，设备不可用或非交互模式中途断线 `3`，排空超时 `5`，软件计数异常或长录音时长误差超标 `6`，其他运行错误 `1`；信号退出沿用 `130/143`。

报告中的 A01/A02 保持 `not_run`，软件检查通过不会自动确认耳机听感、物理路由或蓝牙尾音。用户需按 [A01/A02 验收步骤](docs/ACCEPTANCE.md#6-当前命令与后续约定)完成 10 min 实机录放与空闲/录音/播放三种状态的重连；结果与证据记入 [环境记录](docs/environment.md)。

## 版本管理约定

开发板 `/home/orangepi/projects/rk3588_voice_engine` 是唯一日常代码、文档和 Git 工作目录；Windows 用作 SSH 入口及参考副本。远程版本保存在 [GitHub 公开仓库](https://github.com/jzh03/rk3588_voice_engine)，板端使用 GitHub CLI 通过 HTTPS 认证。账号配置和交接历史见 [环境记录](docs/environment.md#8-git-与云端托管)。当前 Windows Codex SSH 开发环境及验证范围见[开发就绪复核](docs/environment.md#29-windows-codex-ssh-开发就绪复核)。

模型权重、运行库、录音、构建产物、临时文件和本机配置按 `.gitignore` 排除，资产只提交清单。开始工作先检查状态；工作区干净且需要同步远程时执行拉取。完成修改并验证后，只暂存本次相关文件，按用户授权提交和推送；日常 Git 命令不使用 sudo：

```bash
cd ~/projects/rk3588_voice_engine
git status --short --branch
git pull --ff-only
# 编辑并验证后，按具体文件名执行 git add、git commit 和 git push
```

提交还需要独立的 Git 作者配置 `user.name` 和 `user.email`。GitHub CLI 登录负责远程认证，不能替代作者信息；出现 `no email was given and auto-detection is disabled` 时，应在板端仓库补齐作者配置。2026-10-21 已修复本仓库身份并通过严格身份检查，见[作者配置记录](docs/environment.md#40-提交作者身份配置修复)。

推送出现 `Invalid username or token` 或 `Authentication failed` 时，在开发板终端重新认证：

```bash
gh auth login --hostname github.com --git-protocol https --web
gh auth setup-git
gh api user --jq .login
git push --dry-run origin main
```

按终端提示在电脑浏览器打开授权页面，使用 `jzh03` 完成授权；登录过程需要账号本人操作。确认用户名正确、dry-run 成功后，再从 VS Code 推送或执行 `git push origin main`。公开仓库 fetch 成功不能说明写入凭据仍有效；无需删除仓库或重新提交已有改动。
