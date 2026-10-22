# MeloTTS 文件探针与适配接口

本工具只做文件输入与文件输出，不连接耳机、不打开录放设备、不接入实时引擎，也不初始化或推理 LLM。模型组合的原verified范围仍为P0/M0；单次退出0不自动代表A05通过。2026-10-22已完成修正后的正式三批和独立耳机试听，A05/P2-03通过，见[最新复验](#2026-10-22-搜索修正与-a05-复验)。下方各轮历史9/10和失败记录保留原义。

## 固定依赖与构建

运行组合保持 [模型准备记录](model-prep-speech.md) 的 sherpa-onnx 1.12.14 CPU、ONNX Runtime 1.17.1-patched、完整 MeloTTS FP32 模型包。官方 C++ wrapper 使用旧 libstdc++ 字符串 ABI，因此工程采用同版本 C API 封装为 C++17 RAII 类，不改变全工程 ABI。INT8 LFS 指针不会加载，也没有下载其它模型。

在板端工程根目录构建：

```bash
cmake -S . -B build/model-probes -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVOICE_ENGINE_BUILD_TTS_PROBE=ON
cmake --build build/model-probes --target tts_probe tts_text_test -j2
ctest --test-dir build/model-probes -R '^tts_' --output-on-failure
```

开关默认 OFF。显式开启时要求 ENGINE、AUDIO、PULSE_AUDIO 基础目标和公共 probe support 已开启，才检查私有 sherpa/ORT 头文件和库；默认、engine-only、mock-only、native-only 构建不隐式查找这些模型依赖。TTS 模块复用 `voice_engine_audio_dsp`，文件工具另外链接 libsndfile。

## 文件运行

每次必须使用新的输出目录。`--output-dir` 表示证据根目录，工具新建其中的 `<run-id>` 子目录。下面的 `NEW_RUN_ID` 需要替换；该子目录已存在时会在模型初始化之前拒绝，不覆盖已有证据。

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  timeout 900 build/model-probes/bin/tts_probe \
  --config config/file-probes.yaml --cases tests/fixtures/tts_cases.json \
  --output-dir output/runs --run-id NEW_RUN_ID \
  --threads 4 --repeat 3 --warmup 1
```

`--threads` 只允许 2 或 4；省略时采用 `threads.tts`。`--repeat` 为 1–100，`--warmup` 为 0–10。`--case-id ID` 可做少量回归；只有完整十条三次的运行才对应本轮固定文本统计。省略 `--output-dir` 时根目录为 `output/runs`，最终目录仍为 `output/runs/<run-id>`。run ID 和文本 ID 只接受字母、数字、下划线和连字符；不能包含路径片段。文本清单是 `schema_version: 1`、`cases` 数组，每项包含唯一 `id`、`text`、`category`。

`--validate-text-only` 只检查清单、分句和内部标点预处理，允许省略配置；不要求模型锁、不初始化任何模型。默认用 TTS 分句默认值，给定配置时采用配置值。它仍要求新的输出目录。`--help` 不加载配置或模型。

退出码：0 表示本次无失败且已采样的合成 RTF P95 < 1（不代表 A05 全项通过），2 为配置/输入/资产/锁错误，4 为模型、资源或文件生成失败，5 为单段阻塞 Generate 返回后发现超过 `timeouts.tts_segment_ms`，6 为样本有效但合成 RTF P95 未达标。外部 GNU `timeout` 强制结束进程时返回 124；可能只有部分 JSONL，没有最终 summary，不能当成功。

锁覆盖整个模型验证进程。适配器构造与每次生成还调用公共锁检查，防止直接启动绕过公共互斥。每 50 ms 记录 RSS 和 MemAvailable；发现可用内存低于 1.5 GiB 或监控异常时阻止后续生成并记录失败。当前底层 Generate 是阻塞整段调用，正在执行的调用无法因内存采样或超时检查立即中止；硬限时依赖外层进程 `timeout`，不声称已经实现 A08 的及时取消。

## 适配契约与输入策略

`include/voice_engine/tts.hpp` 提供：

- `MeloTts`：一个实例只加载一次模型，批次循环复用。内部复制 Config，仅保留 TTS 启用后调用 `validate_assets`；LLM 等其它模型既不校验也不加载。模型、tokens、lexicon、date/number/phone/heteronym FST 与字典按经校验 manifest 的角色解析，检查真实路径边界、字典文件覆盖以及实际运行库映射。
- `TtsSegmenter`：支持跨块 UTF-8、按中文/英文句界、配置最短/最长字符数分段。长度按 Unicode scalar 计；不切断英文单词、数字小数、带符号数值和数字日期。超过单段上限的不可分数字/单词报错，不在内部硬切。`poll(elapsed_ms)` 由调用者提供当前待发文字已经等待的毫秒数；达到等待阈值且字符足够时冲刷。EOS `finish()` 可冲刷短句，`reset()` 丢弃残留文字。
- `TtsPcmConverter`：原生 44100 Hz float 单声道音频经有状态 `StreamResampler` 真正转换到 16000 Hz，再经 `MonoFrameAssembler` 形成 320 样本/20 ms 帧。一个文本的多个分段共用转换状态；`finish()` 只在文本末尾冲刷滤波尾部并补零最后一帧，`valid_samples` 保存其真实长度。写入 WAV 时不把补零计为音频。`reset()` 清空滤波与聚帧残留。

输入采用严格纯文本策略：标签尖括号、LLM 特殊 token 定界符、Markdown 控制字符、隐藏控制字符和非法 UTF-8 均拒绝；不删除标记后把剩余未知文本当成可播自然语言。`segment_tts_text` 会先检查整条输入，确保不会先合成前半句再发现后半句控制标记。增量接口本身不能撤回已经返回的文本；接入未来 LLM 流之前，调用方仍需完成独立的控制/思考内容策略，本次没有接入 LLM。

中文数字与日期归一化使用固定包提供的 FST，顺序固定为 date、number、phone、new_heteronym。进入该固定前端前，adapter应用[中文逗号上下文策略](#2026-10-22-搜索修正与-a05-复验)，原始输入仍原样记录。固定 C API 不返回归一化后的字符串；不能用字符串存在或文件有效替代发音试听。常用英文按同一 lexicon 发音，`unknown_english_words` 单独列出词典外英文词，不能把非空音频当作英文发音准确的证据。

`generate` 只接受配置上限内的一个已分段纯文本，返回原生 44100 Hz float 音频和精确底层 Generate 耗时。输出超过 `queues.max_tts_audio_ms` 会失败；该判断发生在上游已分配整段输出之后，不能解释为底层内存的硬上界。单个文件探针保留整句原生与转换音频用于证据，不声称具备实时有界播放队列。

## 指标与产物

模型在整个批次驻留。warmup 使用第一条文本，每次预热单独写记录并从正式分位数中排除；正式各轮按固定种子 20261020 打乱文本顺序，模型噪声配置保持上游默认，不宣称输出确定性。每个 WAV 保存实际 SHA-256。

- `run.json`：argv、配置/清单/二进制/资产 manifest 哈希、固定模型选项、解析后的资源路径、加载时间与实际共享库路径。
- `results.jsonl`：每次预热及正式尝试，包含 ID、原文、分段、词典外英文词、成功/失败、失败原因、Generate/首段/完整处理时间、RTF、原生与内部采样数、末帧有效长度、WAV 哈希。失败不丢弃，也不伪造 RTF。
- `summary.json`：完整结果与 P50/P95/max（nearest-rank）、成功/失败/预热失败数、峰值 RSS、最小 MemAvailable、性能门槛和保留为 false 的 A05 全项状态。
- `resources.jsonl`：50 ms 采样的进程 RSS、系统 MemAvailable 与相对时间。
- `*-native.wav`：44100 Hz 单声道 PCM16；`*-16k.wav`：16000 Hz 单声道 PCM16。原生适配接口返回 float，文件按 PCM16 保存以便外部试听。探针不会主动播放。

`generate_s` 只计实际 sherpa Generate 调用，原生结果检查/复制耗时另在分段 `adapter_s` 和整体时间里。`first_segment_s` 从分句开始计到首段生成并转换出完整 PCM 帧；这是首段可用时间，不是流式模型首样本延迟。`complete_s` 包含分句、所有分段 Generate、检查/复制、重采样与最终尾帧冲刷，排除 WAV 写盘与文件哈希。两种 RTF 的分母都采用原生音频时长。分位数只对成功的正式尝试计算，同时必须并列失败数；只要有失败，性能门槛不通过。

## 2026-10-20 历史验证

构建与无模型回归证据位于 `output/runs/tts-probe-20261020-211405/`（目录名是本轮标识，实际命令时钟以运行记录为准）。已覆盖短句 EOS、定时 flush、长句、跨字节 UTF-8 日期、小数和带符号数字、控制标记拒绝、转换跨块一致性、尾部样本/补零、reset 丢残留、CLI 参数与证据目录不可覆盖。正式性能证据为 `output/runs/tts-probe-20261020-211053/`，包含两组各自的运行记录、退出码、固定二进制、源文件快照及 SHA-256。测试时 ASR 批次与所有编译已结束；两组按统一锁串行执行，未改 CPU 调度、绑核或系统运行库。

| 项目 | 2 线程 | 4 线程 |
|---|---:|---:|
| 正式成功 / 失败 | 30 / 0 | 30 / 0 |
| 预热成功 / 失败 | 1 / 0 | 1 / 0 |
| Generate RTF P50 / P95 / max | 1.299 / 1.326 / 1.350 | 0.843 / 0.883 / 0.894 |
| 完整处理 RTF P50 / P95 / max | 1.317 / 1.341 / 1.366 | 0.859 / 0.899 / 0.909 |
| 首段可用时间 P95（秒） | 5.411 | 3.455 |
| 完整处理时间 P95（秒） | 5.411 | 3.455 |
| 模型初始化（秒，不含资产校验） | 8.982 | 9.051 |
| 峰值 RSS（KiB） | 575040 | 570820 |
| 最小 MemAvailable（KiB） | 6091264 | 6101924 |
| 退出码 / RTF 门槛 | 6 / 未通过 | 0 / 通过 |

两组均为十条固定文本各三次，分位数按 nearest-rank，对成功正式尝试统计；上述批次没有失败或被排除的正式尝试。各组 62 个 WAV（正式 60 个 + 预热 2 个），共 124 个文件通过独立格式、单声道、采样率、非零 PCM、样本数、末帧有效长度与 SHA-256 检查，统计分位数亦独立重算。详见 `artifact-check.json` 和可重跑的 `verify_performance.py`。

本轮后续文件测试建议显式使用 `--threads 4`。没有改通用线程默认值、固定模型、系统调度或运行库；单模型文件结果不能外推为多模型共驻或实时链路性能。

上游对 `tts_07` 的全角冒号“：”各轮均记录 `Ignore OOV`（两组各 3 次），在 `oov-warnings.json` 单列，未改写用例或掩盖警告。全部文本的词典外英文词列表为空。该标点会被上游忽略，停顿和语气仍需试听；数字、日期、英文发音也未由用户确认。因此 **4 线程的性能条件通过，A05 全项仍未验收**。

可供后续试听的文件均在正式 run 的 `thread4/` 下：`tts_01-r0-native.wav`（普通中文）、`tts_04-r0-native.wav`（日期）、`tts_05-r0-native.wav`（数字/小数）、`tts_09-r0-native.wav`（中英混合），每份都有对应 `-16k.wav`。本轮未播放这些音频。

本工具与 TTS 自身生成的 ASR/VAD fixtures 不能提供自然人声独立准确率证据。自然语音、噪声、耳机试听、实时链路、共驻压力和 A08 取消仍需要后续独立验收。


## 2026-10-22 材料审核补充

P1-03 对原合成工程材料的审核已完成，`mix_02` 中“调低”被用户听成“吊低”，此发音问题纳入 P2-03 后续定位；原 WAV、参考与历史性能报告保持不变。该轮审核针对 fixture 音频，未代替本探针 A05 的正式 10 条输出试听。证据与覆盖范围见[材料审核](file-fixtures.md#人工审核覆盖与已知问题)。

## 2026-10-22 正式试听与发音诊断

正式试听已完成，**A05 未通过：10 条审完，9 条通过，`tts_09` 发音失败**。试听前统一选定历史 `output/runs/tts-probe-20261020-211053/thread4/` 的十条正式 `repeat_index=0` / `warmup=false` 输出，不从三次重复中挑选听感最好的一次。对应文本、WAV 哈希、相关源文件/配置以及运行依赖声明已核对；历史资产清单至当前只增加非运行时许可证/来源记录，不是模型替换。本轮没有重新生成 TTS，也没有重新测量其性能。

| 试听范围 | 用户确认 |
|---|---|
| `tts_01`～`tts_05` | 内容、数字日期、首尾和声音正常，无多余标记 |
| `tts_06`、`tts_07`、`tts_08`、`tts_10` | 时间、英文、首尾与声音正常，冒号停顿和分句衔接正常；上游全角冒号 Ignore OOV 历史警告仍保留 |
| `tts_09` | 原文“请打开浏览器，搜索 hello world。”中“搜索”听成“欧索”；同一文件单独复听仍如此，未据片段确认其余字段全部通过 |

使用 vivo TWS 4 的显式 sink，保持 mSBC 和 50% 输出音量；两组各五条及第九条复听由 `audio_probe` 完成，三轮均 exit 0、软件计数正常、无设备丢失。没有录音、加载模型或改变麦克风音量。随后同一 r0 的 44.1 kHz 原生 WAV 和应用转换后的 16 kHz WAV 经 `paplay` 依次播放，用户确认“两段都读成‘欧索’”。耳机仍为 mSBC 16 kHz，原生文件也会由音频服务转换，故仅说明问题不能归因于应用转换这一个环节，**还不能排除共同的服务/耳机路径，也不能确定合成端根因**。

静态检查发现：词典没有“搜索”整词项，但单字“搜”为 `s ou 1 1`，不支持直接断言词典把“搜”改成“欧”。独立材料缺陷 `mix_02` 的“调低”也无整词项，单字“调”与“吊”均为 `d iao 4 4`，这是多音字回退的具体线索；现有日志没有最终音素/token，尚未证实实际运行路径。两处问题分开保留。后续在独立诊断候选中捕获归一化、分词、最终音素/token，再决定通用修复；不通过改固定句子、参考或覆盖原模型词典来改变本轮结论。

4 线程 30/30 有效输出、Generate RTF P95 0.882977、完整处理 RTF P95 0.899146、首段 P95 3.455315 s 均复用 **2026-10-20 历史批次**，独立重算一致；2 线程 1.326 未达标记录保留。性能条件与用户发音确认分别判断，A05/P2-03 不因性能达标而关闭，实时首段、共驻与 A08 也未据此验收。

本轮根目录为 `output/runs/a03-a05-acceptance-20261022-111537/`：`a05-listening-plan.json` 固定选择，`a05-human-*.json` 保存用户确认，`playback/` 保存三轮原始播放报告，`a05-review.json` 汇总逐条结果及历史性能核对。`a05-native-converted-diagnostic.json` 保存两版对照命令/哈希/退出码，其执行时的 pending_user 不追改，后续确认由带原报告哈希的 `a05-diagnostic-human.json` 补充；`pronunciation-diagnostic.json` 保存词典定位、哈希和推断范围。原性能 summary、文本、WAV 均不改，当前结论另存[版本化模型审核摘要](../tests/fixtures/model-acceptance-review.json)。

## P2 流式队列与补充读音（2026-10-22）

[本轮环境汇总](environment.md#48-p2-模型适配端点与流式合成)区分工程实现、固定集内容验收与实时应用验收。原模型包、模型/依赖主清单及此前报告保持不变。

### 可选补充词典与诊断

`--debug-frontend` 打开固定 sherpa 前端日志；日志只用于诊断，不把归一化/音素正确等同于声音正确。`--pronunciations config/tts-pronunciations.json` 显式加载词级读音补充；两项均不能与 `--validate-text-only` 一起使用。默认没有补充词典。

规则为 `schema_version: 1`、`entries` 数组，每项含 `word`、等长的 `phones`/`tones`。规则最多 64 KiB、256 项；单词须为一个未经修剪的合法纯文本单元，音素须存在于冻结 tokens，声调须在冻结 lexicon 中实际出现。不是任意改写输入文本。新词条放在派生词典前部，同名原词条被替换，其余行保持；输出使用 O_EXCL/O_NOFOLLOW 创建，不覆盖原词典、已有文件或符号链接。原始资产验证仍在构造时执行。

本轮唯一规则把“调低”绑定为 `t iao d i / 2 2 1 1`，登记在[独立候选清单](../models/pronunciation-candidates.json)，并不改变主清单 verified 的 P0/M0 范围。每个新 run 保存 `derived-lexicon.txt`，记录规则与派生词典哈希、原词典路径及条目数。规则文件本身没有自动启用到主引擎或联合探针。

固定 ORT API 的只读输入追踪证明：原“调低”在原句与新“请调低亮度。”中均送入 diào dī；补充后均为 tiáo dī。用户确认两条修正读音及“调整/调节”正确，五条诊断样本首尾完整。独立词“搜索本地文件”也被确认正确；正式句的新合成仍听感不清。原“搜索”的实际模型输入含 `s ou 1 1 / s uo 3 3`，不能把其问题归为缺少 s 音素。

临时 A2DP 对照中新/旧正式句都被用户反馈有电音，因此此对照不能排除或确定合成端根因；已经恢复 mSBC、输出 50%。旧 A05 9/10、原“搜索→欧索”失败及原材料 WAV 均保留。新诊断和词典候选尚不构成完整 A05 重验。输入追踪的 LD_PRELOAD 只位于私有诊断目录，正式性能与 R1 运行没有该插桩。

### 有界 TTS worker 与提交边界

`include/voice_engine/tts_stream.hpp` 的 `TtsStreamWorker` 用独立常驻线程消费文本并调用同步 Generate。接口为 `begin / submit / finish / poll / cancel / wait_idle / stats`；`commit_if_current` 将轮次校验与短小的下游提交放在同一互斥边界，锁内禁止设备 I/O、推理、等待或重入。开始新轮前须等待旧生产者空闲；(session,generation) 必须严格递增，防止复用编号放行旧帧。

默认输入排队最多 8192 字节、待合成分段最多 4 个、PCM deque 最多 100 个 20 ms 帧。文本/分段溢出明确失败并失效当前轮；PCM 满时仅 worker 等待，取消可立即唤醒。分句器保留自身最多 8192 字节 pending；同步 Generate 的整段原生缓冲和重采样临时帧不包含在 deque 上限中，原生输出仍有 20 s 检查。不能把 deque 峰值等同于全部内存硬上限。

取消清空输入/PCM 并使旧 generation 提交失效；同步 Generate 仍可能继续，返回后丢弃旧结果。单段超时同样在 Generate 返回后检查，硬退出依赖进程监督。不得把“旧结果不再提交”写成 SDK 已支持中断。单元测试覆盖跨 UTF-8/日期、定时冲刷、尾帧、队列背压、溢出、同步生成中取消、立即新轮拒绝、旧帧和 ID 重用。

### LLM → TTS 局部文件探针

同时打开 `VOICE_ENGINE_BUILD_LLM_PROBE` 和 `VOICE_ENGINE_BUILD_TTS_PROBE` 时构建 `llm_tts_probe`。它用真实 LLM 可播文本流驱动上述 worker，输出文件；没有 VAD/ASR 或耳机录放，不替代 P2-04 四模型共驻、P3 完整链路或 P4。

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  timeout -s TERM -k 5 180 build/model-probes/bin/llm_tts_probe \
  --config config/llm-tts-probe.yaml --output output/runs/NEW_STREAM_RUN \
  --prompt '请用一句话解释什么是离线语音助手。'
```

配置只启用 LLM/TTS，TTS 显式 4 线程，其余采用已有默认值。输出目录须不存在。`--delivery stream|batch` 默认 stream；`--replay PATH` 与 `--prompt` 二选一，重放此前 `packets.jsonl` 的同一文本/到达时间线，此模式只初始化 TTS。对照应复用相同 replay、配置、线程与采样口径，不能以两次 LLM 的不同答案比较首段延迟。

replay 每行含单调的 `offset_ns` 与完整 UTF-8 `text`，总文件 ≤1 MiB、≤4096 行、累计文本 ≤8192 字节、时间线 ≤60 s。`--validate-input-only` 只验证 CLI/replay；`--cancel-ms 1..30000` 从本轮时间原点起定时取消。运行时 MemAvailable <1.5 GiB 连续约 3 s、监控文件写入失败或 175 s 内部期限触发受控进程失败，外层 timeout 另提供硬上限。

LLM SDK 回调仅写有界 token 队列；主消费者在 LLM generation 提交边界把可播文本交给 TTS。消费协议在观察生产者空闲后再次取尽队列，并核对完整 packet 文本与最终结果完全一致；TTS 也在 idle 后再次取尽尾帧。文件成功写出和核验后才确认历史。取消或 poisoned 句柄阻止继续提交；未确认 SDK idle 的异常要求重启受监督进程。

产物为 `run.json`、`packets.jsonl`、`resources.jsonl`、`summary.json` 与 `output-16k.wav`；异常保存 `failure.json` 或独立 guard/restart 记录。记录首可播文本、首分段、首 PCM、输入结束、生产结束、取消及空闲时间，文本/分段/PCM 队列峰值、实际分段、二进制/配置/输入/WAV 哈希。整句诊断 WAV 可暂存最多 120 s，与生产队列分开。退出 0 只表示局部流程成功（有意取消也可成功）；2 是 CLI/输入参数错误，4 是其余运行错误，5 是资源/内部期限；外层 timeout 通常为 124。

一次真实 LLM→TTS 和固定时间线各三次 stream/batch 对比已执行；用户确认预先选定的第一对 WAV 完整、正常且分段停顿不影响听感。新候选与 R1 逐次时间、取消结果和限制见[环境汇总](environment.md#48-p2-模型适配端点与流式合成)；不能把文件首 PCM 当作耳机首音频，也不能将单句对比外推到全部回答。原 4 线程 A05 30 条性能仍只引用 2026-10-20 历史批次。

## 2026-10-22 搜索修正与 A05 复验

**A05通过，P2-03模型侧交付完成。** 使用原[十条固定文本](../tests/fixtures/tts_cases.json)，预先固定正式`repeat_index=0`的十条试听，另听第九条r1/r2；用户确认全部正确完整、三次“搜索”清楚，停顿与声音正常，无多余标记。没有从重复中挑选较好文件。新三批性能及资源数字集中在[环境 §50](environment.md#50-p2-03-搜索修正与-a05-复验)，版本化审核见[tts-model-acceptance.json](../tests/fixtures/tts-model-acceptance.json)。

### 中文逗号与固定前端契约

`melo_frontend_text()`先执行完整严格UTF-8/纯文本检查，仅把中文全角`，`（U+FF0C）转为内部`、`（U+3001）。冻结的Melo前端将二者映射为同一comma音素，但后者不触发逗号处的独立推理，因此有界段内保留前后文。原文、分段、数字、日期、英文、ASCII逗号和其余码点保持；这不是把“搜索”替换成其他词。默认adapter自动应用此策略，可选“调低”词典仍需原CLI显式启用。

`MeloTts::generate()`仍先要求单个合法且不超过`tts.max_chars`的分段，`max_num_sentences=1`、20s生成音频限制和超时检查不变。不能简单把`max_num_sentences`调大：该版本先对各子句分别插blank再连接，第二子句的音素位置会错开参考导出器的语言编码约定。诊断保留此候选但不采纳；新策略让逗号关联的原始音素一次插blank。正式句原两个Run长度27/31，新Run长度57；“搜索”起首s位于正确的奇数索引27。七条诊断及生产路径追踪均证明去除插入blank后的音素/声调与原文一致，包括显式英文空格。SDK升级时必须重验alias、FST及此输入契约。

`tts_probe`的`run.json`记录`frontend_policy=melo-fullwidth-comma-context-v1`和`max_num_sentences`；逐段`segment_metrics`同时保留`text`与实际送入固定前端的`frontend_text`。无模型模式增加`frontend_segments`。联合探针同样保留原文与内部输入，并记录`tts_frontend_policy`；内部输入还会经过既有FST，不等于最终归一化字符串。

此次修正的验证范围是现有固定模型与中文全角逗号。ASCII逗号仍沿用原行为，以免破坏数字分组；未宣称任意上下文都达到相同发音质量。正式第七条全角冒号仍有三次`Ignore OOV`警告，用户已确认内容与停顿正常；不隐藏该警告。

### 证据边界

新run为`output/runs/p2-03-search-20261022-125833/`。控制组、弃用的直接合并候选、输入追踪和全部用户反馈均保留；正式`formal/`三批不使用LD_PRELOAD、debug或补充词典。原模型/SDK/lexicon、固定原文与历史WAV不改，原9/10报告保持失败，当前结论由独立审核承接。

文件流式回归以同一“搜索”文本分四次送入worker，验证新前端参与实际生成；另一次生成中取消确认旧结果丢弃、PCM提交为0。该局部回归不代替原R1同时间线对照，也不等于完整A08的设备flush/声学尾音或P2-04共驻验收。

## P2-04 共驻观测接口

`TtsStreamStats.current_input_bytes/current_segments/current_pcm_frames` 分别记录当前输入字节、待合成分段与待消费 PCM 帧，`segmenter_pending_bytes` 记录分段器残留；均在 worker 现有互斥锁下取快照。它们与累计峰值分开，正在 Generate 的临时缓冲仍受原段长/音频上限约束。四模型资源采样与 A06 结果见[共驻文档](coexist-probe.md)。
