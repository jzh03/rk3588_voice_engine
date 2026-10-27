# 构建、配置与开发运行

本手册面向开发和排查，日常使用见[项目首页](../README.md#日常使用)。命令在板端工程根目录执行。实验记录和内部技术文档不随仓库提供；以下仅说明通用接口。

## 基础环境检查

在开发板仓库根目录执行：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/bin/env_check
```

输出 `ENV_CHECK_OK` 表示该检查完成。它只验证编译和库调用，不打开录音、播放或 NPU。

多人或多代理并行开发时，构建、CTest 和大文件校验也应通过 `flock -x /tmp/rk3588_voice_engine_model_validation.lock <完整命令>` 与模型初始化、推理及性能采样互斥，锁覆盖整个进程。性能采样窗口内不得并行编译或大文件校验。单独运行的上述命令仍使用同一板端构建目录。

CMake 基础开关默认 `ON`；三个模型文件探针开关默认 `OFF`：

| 开关 | 构建内容与依赖 |
|---|---|
| `VOICE_ENGINE_BUILD_ENGINE` | `voice_engine` 与配置、资产、日志、控制库；依赖 yaml-cpp、nlohmann-json、pthread 和 coreutils 的 `sha256sum`，无需 OpenSSL 开发包 |
| `VOICE_ENGINE_BUILD_AUDIO` | `voice_engine_audio` 内存 mock，仅依赖 C++17 标准库；与下一开关共同决定是否构建真实后端 |
| `VOICE_ENGINE_BUILD_PULSE_AUDIO` | 音频模块开启时构建 `voice_engine_audio_dsp` 和 `voice_engine_pulse_audio`，依赖 libpulse、libsamplerate 和 pthread；引擎也开启时构建 `audio_probe`，另需 libsndfile |
| `VOICE_ENGINE_BUILD_ENV_CHECK` | `env_check`，单独检查 libpulse、libsamplerate、libsndfile、yaml-cpp 和 nlohmann-json |
| `VOICE_ENGINE_BUILD_ASR_PROBE`（OFF） | VAD/RKNN及CPU KWS文件探针；显式开启后检查固定私有 RKNN、sherpa/ORT 与官方 fbank 依赖 |
| `VOICE_ENGINE_BUILD_TTS_PROBE`（OFF） | CPU TTS 文件探针；显式开启后检查固定私有 sherpa/ORT，复用音频 DSP 与 libsndfile |
| `VOICE_ENGINE_BUILD_LLM_PROBE`（OFF） | 常驻 RKLLM 文件探针；需 ENGINE 及固定私有 RKLLM 1.3.0。与 TTS 开关同时打开时额外构建 `llm_tts_probe`；三个模型开关全开时再构建 `coexist_probe` 和 `voice_file_demo`，真实 Pulse 后端可用时同时接通 `voice_engine` 实时入口 |

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

检查成功输出 `CONFIG_OK`、`ASSETS_OK models=N dependencies=N files=N` 和音频/日志摘要，退出码为 `0`；配置、资产或命令行错误退出码为 `2`，并提示相关文件或字段。`--help` 返回 `0`。`--check-config` 与 mock/实时控制模式互斥，不打开设备或 NPU。

每次只加载 `--config` 指定的一个完整 YAML 文件，不合并 `default.yaml` 与 `local.yaml`。`audio.source`、`audio.sink`、`audio.profile` 必须是非空字符串，source 不能以 `.monitor` 结尾；`sample_rate`、`channels`、`frame_ms` 缺省为 `16000`、`1`、`20`，首版只支持这组参数。可选 `audio.playback_lead_in_ms` 缺省0，允许0～2000 ms且为20 ms整数倍；每个非空播放轮次的首段语音前添加该时长静音。可选 `audio.playback_keepalive` 为布尔值、通用缺省false；启用且允许播放时，用独立零PCM流保持指定输出运行，贯穿空闲/采集/回答/取消，退出或失联时释放。当前实时模板与本机实时配置启用保活、前置静音设0；不改变旧 `config/local.yaml`。保活会使输出流持续占用设备，使用时应考虑耗电。未知字段、重复键和非法类型会被拒绝。

完整字段的缺省值、单位和数值范围集中在 [config/default.yaml](../config/default.yaml) 的注释中。除必填音频设备字段外，各分组与字段均可省略；现有仅含 audio/logging 的 `local.yaml` 无须迁移。整数、布尔、字符串类型分别校验，字符串不得含 NUL。模板的设备字段留空，需填入设备名称或使用已有本机配置。

| YAML 分组 | 内容与类型 |
|---|---|
| `audio` | 设备名称/双向 profile 字符串；采样率、声道、帧长和播放前置静音时长整数；播放保活布尔值 |
| `logging` | 单个日志文件字节上限、保留文件数整数 |
| `models` | `vad/asr/llm/tts/kws` 各含 `enabled` 布尔及 `id` 字符串；默认全部禁用 |
| `assets` | 模型与依赖 manifest 路径字符串 |
| `vad` | 预录、最短语音、尾部静音、等待开口与最长发言的毫秒整数 |
| `threads` | 各模型模块的线程数整数 |
| `llm` | 上下文、输出和余量 token 数整数 |
| `tts` | 最短/最长分段字符数、软目标、标点前瞻、冲刷和预缓冲整数；合成语速数值；首段/后段策略及段内短句合成布尔值 |
| `queues` | 采集/播放缓冲、待处理轮数、文本字节数、TTS 段数与音频时长整数 |
| `timeouts` | ASR、LLM 首 token/总生成、TTS、取消超时的毫秒整数 |
| `dialogue` | 唤醒/打断布尔值、唤醒词和关键词文件路径字符串、窗口及打断时长整数；字段与约束以配置模板和解析器为准 |

`tts.prebuffer_ms` 通用缺省0、范围0～2000 ms；正值开启两槽原生语音预取和独立送音线程，等待真实音频达到目标、两槽满或合成结束后起播。实时模板使用1500 ms；它不是追加的数字静音。每槽仍受 `queues.max_tts_audio_ms` 单段上限约束，原PCM队列额度不变；软件起播等待可能增加，需在软件时间线中记录。

`tts.target_chars` 通用缺省0，表示保持原先按句末、定时和硬上限切分；正值须位于 `min_chars`～`max_chars`，优先在目标附近的安全位置分段。较长数字、日期或英文单词可以超过目标，仍受原 `max_chars` 硬上限约束；输入字段范围0～1024。当前实时关闭first_segment覆盖，首块和后续统一min/target/lookahead=12/12/2，保留40字符硬上限、300 ms冲刷，起播储备为1500 ms。调小目标可能改变语调和分段开销，候选需同时核验延迟、连续性与听感。

`tts.speed` 缺省1.0，接受有限数值0.5～2.0；小于1放慢、大于1加快，带引号的数值、布尔和非有限值会被拒绝。这是传给固定Melo模型的合成语速，原生44100 Hz及设备16000 Hz采样率不变。实时模板使用0.85；调慢也会增加音频长度及合成成本，不能单独保证无顿挫。

`tts.boundary_lookahead_chars` 缺省0，整数范围0～32。正值要求 `target_chars > 0` 且 `target_chars + boundary_lookahead_chars <= max_chars`；达到软目标后，在有限后续字符中优先找句末、中文逗号/顿号或非数字分组的ASCII逗号。等待从达到目标后的首次poll计时，最多再等 `flush_ms`；EOS立即冲刷，字符额度用完则回退安全软切。没有通用中文分词保证；使用时需考虑文本等待对起播时间的影响。

`tts.sentence_mode` 为布尔值，缺省false保持原规则；true时要求 `target_chars=0`、`boundary_lookahead_chars=0`，且不设置first_segment覆盖。首段达到min_chars后可在分句标点提前输出，后续优先等完整句末；已排队的文字在触发定时回退前先读取。flush_ms到期只在已有分句标点或词间空白处回退；没有边界时等EOS或max_chars，避免短定时器机械切中文字。40字等硬上限仍优先使用已有标点，完全无边界时沿用安全硬切/不可分词拒绝。它不是通用语义分析或中文分词。

`tts.first_segment` 是可选映射，缺省/null表示不覆盖首块，保持旧配置行为。启用时必须完整提供`min_chars`（1～256）、`target_chars`（1～1024）和`boundary_lookahead_chars`（0～32），满足min≤target且target+lookahead≤共享max_chars，并要求sentence_mode=false。仅首个已输出非空文本块使用该组，随后使用tts顶层值；每轮和reset恢复首块。字符计数包含标点。当前实时配置为：

~~~yaml
tts:
  sentence_mode: false
  first_segment: null
  min_chars: 12
  target_chars: 12
  boundary_lookahead_chars: 2
  max_chars: 40
  flush_ms: 300
  prebuffer_ms: 1500
  speed: 0.85
  join_short_sentences: true
~~~

启用可选动态模式时，两阶段均可在达到各自min后遇逗号/句末提前输出；没有标点时只等待有限前瞻字符或既有flush期限，不强制完整句末。动态模式也会先读取积压文字，避免旧计时器把已到齐的后续文本提前切碎。精确计时明细新增text_chars及本段实际min/target/lookahead，可核对策略切换。

`tts.join_short_sentences` 为布尔值，通用缺省false、当前实时true。将已有有界段内部的中文句末标点按逗号音素连接，使短句共享声学上下文；保留段尾语气和原文，不改变分段长度、不额外等待。独立于sentence_mode，适配实现见`src/tts/`。

跨字段检查包括：预录不超过采集缓冲、最短语音不超过最长发言、TTS 最短分段不超过最长分段、LLM 输出加余量小于总上下文、首 token 超时不超过生成总超时；采集/播放缓冲必须为音频帧长的整数倍。启用模型时 `id` 必须非空；启用唤醒时要求 KWS 与非空唤醒词，启用打断时要求 VAD 和 ASR。这些参数由模型适配器和实时控制器执行。

### 资产清单

`--check-config`检查启用模型及其依赖，不自动下载。`models.<模块>.id`绑定模型清单的同名ID；全部模型禁用时不读取资产清单。公开清单保留文件身份与来源，不附带实验数据或性能结论。

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

清单用于完整性和版本字符串匹配；程序不联网核实来源、不验证run_id所代表的记录，也不据平台字符串证明兼容性。status保持本机资产登记值；schema要求run_id时使用明确的`private-validation-record-withheld`标记，不链接或冒充公开实验记录。`ASSETS_OK`只表示资产检查通过。清理元数据会改变工程清单自身的哈希，不改变资产文件或现有已安装发布目录。

## 终端控制与 JSONL 日志

真实实时模式需要启用模型的构建及已准备好的本机配置。以下mock入口不加载模型或打开音频设备；两种模式共用run/session/generation/seq关联和日志轮转规则。

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

在板端工程根目录构建可选模型工具与完整文件链路（默认构建仍关闭三个模型开关）：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake -S . -B build/model-probes -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVOICE_ENGINE_BUILD_ASR_PROBE=ON -DVOICE_ENGINE_BUILD_TTS_PROBE=ON \
  -DVOICE_ENGINE_BUILD_LLM_PROBE=ON
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake --build build/model-probes -j2
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  ctest --test-dir build/model-probes --output-on-failure
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 tests/integration/check_fixtures.py
```

这组 CTest 不初始化模型或访问物理音频设备；音频集成项使用完全隔离的合成输入和空输出，现已纳入完整回归。固定集负例也在 CTest 中运行，只变异临时副本。默认构建不要求私有模型 SDK，三种独立构建边界继续保留。

以下只读取固定文件、保存结果。每次把 `NEW_ASR_RUN`、`NEW_TTS_RUN` 改成新 run ID；`--output-dir` 是父目录，已有 `<父目录>/<run-id>` 会被拒绝。初始化、推理和采样全程共用同一锁，未持有锁时程序在模型初始化前退出 2。

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  timeout 180 build/model-probes/bin/asr_probe \
  --config config/file-probes.yaml \
  --manifest /path/to/your-corpus/manifest.json \
  --listening --chunk-ms 20 --repeat 3 --output-dir output/runs --run-id NEW_ASR_RUN

flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  timeout 900 build/model-probes/bin/tts_probe \
  --config config/file-probes.yaml --cases tests/fixtures/tts_cases.json \
  --threads 4 --repeat 3 --warmup 1 --output-dir output/runs --run-id NEW_TTS_RUN
```

上例须替换为自己的语料清单。仓库内`tests/fixtures/manifest.json`定义固定合成回归输入，不能代替自然人声评价。ASR的`--official`可改用已准备的官方WAV；TTS的`--validate-text-only`只检查文本和分句。两工具不播放WAV或录音。退出6表示工具检查未达标；外层timeout返回124时保留部分输出。私人录音和审核目录不随仓库提供；语料审核工具需用户提供对应材料。

完整WAV → ASR → LLM → TTS使用`voice_file_demo`及`config/voice-file-demo.yaml`。它写出WAV，不打开耳机；程序退出0不等同于内容或设备验收。具体选项以各工具`--help`为准。

## 本地部署与控制

部署管理器是[scripts/deploy.py](../scripts/deploy.py)。准备好本机配置后，可将程序和固定资产打包；下面的版本名是示例，不能覆盖已存在版本：

```bash
python3 scripts/deploy.py stage --project . --binary build/model-probes/bin/voice_engine \
  --config config/barge-in-local.yaml --release example-release
python3 scripts/deploy.py verify --release example-release
systemctl --user stop rk3588-voice-engine
python3 scripts/deploy.py activate --release example-release
python3 scripts/deploy.py install-service --template packaging/rk3588-voice-engine.service.in
systemctl --user start rk3588-voice-engine
```

默认安装目录为`~/.local/share/rk3588-voice-engine/`。current/previous指向发布版本，runs保存托管日志；服务手动启停。verify做配置、资产和依赖检查，不执行模型验收。正常使用前须另备模型、运行库和本机音频配置。

```bash
python3 scripts/deploy.py status
python3 scripts/deploy.py send cancel
systemctl --user stop rk3588-voice-engine
python3 scripts/deploy.py rollback
systemctl --user start rk3588-voice-engine
```

回退恢复上一程序、配置及资产组合，不回退系统库或内核。更多子命令以`python3 scripts/deploy.py --help`为准。

## 版本管理约定

代码和Git操作统一在板端完成；Windows用作SSH入口和参考副本。开始工作先检查`git status --short --branch`，保留已有修改；只在工作区适合且任务需要时同步远端。

模型实体、运行库、私人录音、输出、本机配置及实验资料按.gitignore排除。取消跟踪使用`git rm --cached`并保留工作区文件；不能将忽略规则视为历史删除。

公开资产和材料清单仅保留运行/校验契约与来源，实验记录不随仓库提供。只有得到用户授权时才提交、推送或改变历史；Git身份与认证由账号本人管理。
