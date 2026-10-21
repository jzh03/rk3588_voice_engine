# MeloTTS 文件探针与适配接口

本工具只做文件输入与文件输出，不连接耳机、不打开录放设备、不接入实时引擎，也不初始化或推理 LLM。模型仍为 candidate；生成有效 WAV 不代表用户已确认声音、数字日期发音或 A05 已通过。

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

`--validate-text-only` 只检查清单和分句，允许省略配置；不要求模型锁、不初始化任何模型。默认用 TTS 分句默认值，给定配置时采用配置值。它仍要求新的输出目录。`--help` 不加载配置或模型。

退出码：0 表示本次无失败且已采样的合成 RTF P95 < 1（不代表 A05 全项通过），2 为配置/输入/资产/锁错误，4 为模型、资源或文件生成失败，5 为单段阻塞 Generate 返回后发现超过 `timeouts.tts_segment_ms`，6 为样本有效但合成 RTF P95 未达标。外部 GNU `timeout` 强制结束进程时返回 124；可能只有部分 JSONL，没有最终 summary，不能当成功。

锁覆盖整个模型验证进程。适配器构造与每次生成还调用公共锁检查，防止直接启动绕过公共互斥。每 50 ms 记录 RSS 和 MemAvailable；发现可用内存低于 1.5 GiB 或监控异常时阻止后续生成并记录失败。当前底层 Generate 是阻塞整段调用，正在执行的调用无法因内存采样或超时检查立即中止；硬限时依赖外层进程 `timeout`，不声称已经实现 A08 的及时取消。

## 适配契约与输入策略

`include/voice_engine/tts.hpp` 提供：

- `MeloTts`：一个实例只加载一次模型，批次循环复用。内部复制 Config，仅保留 TTS 启用后调用 `validate_assets`；LLM 等其它模型既不校验也不加载。模型、tokens、lexicon、date/number/phone/heteronym FST 与字典按经校验 manifest 的角色解析，检查真实路径边界、字典文件覆盖以及实际运行库映射。
- `TtsSegmenter`：支持跨块 UTF-8、按中文/英文句界、配置最短/最长字符数分段。长度按 Unicode scalar 计；不切断英文单词、数字小数、带符号数值和数字日期。超过单段上限的不可分数字/单词报错，不在内部硬切。`poll(elapsed_ms)` 由调用者提供当前待发文字已经等待的毫秒数；达到等待阈值且字符足够时冲刷。EOS `finish()` 可冲刷短句，`reset()` 丢弃残留文字。
- `TtsPcmConverter`：原生 44100 Hz float 单声道音频经有状态 `StreamResampler` 真正转换到 16000 Hz，再经 `MonoFrameAssembler` 形成 320 样本/20 ms 帧。一个文本的多个分段共用转换状态；`finish()` 只在文本末尾冲刷滤波尾部并补零最后一帧，`valid_samples` 保存其真实长度。写入 WAV 时不把补零计为音频。`reset()` 清空滤波与聚帧残留。

输入采用严格纯文本策略：标签尖括号、LLM 特殊 token 定界符、Markdown 控制字符、隐藏控制字符和非法 UTF-8 均拒绝；不删除标记后把剩余未知文本当成可播自然语言。`segment_tts_text` 会先检查整条输入，确保不会先合成前半句再发现后半句控制标记。增量接口本身不能撤回已经返回的文本；接入未来 LLM 流之前，调用方仍需完成独立的控制/思考内容策略，本次没有接入 LLM。

中文数字与日期归一化使用固定包提供的 FST，顺序固定为 date、number、phone、new_heteronym。固定 C API 不返回归一化后的字符串；不能用字符串存在或文件有效替代发音试听。常用英文按同一 lexicon 发音，`unknown_english_words` 单独列出词典外英文词，不能把非空音频当作英文发音准确的证据。

`generate` 只接受配置上限内的一个已分段纯文本，返回原生 44100 Hz float 音频和精确底层 Generate 耗时。输出超过 `queues.max_tts_audio_ms` 会失败；该判断发生在上游已分配整段输出之后，不能解释为底层内存的硬上界。单个文件探针保留整句原生与转换音频用于证据，不声称具备实时有界播放队列。

## 指标与产物

模型在整个批次驻留。warmup 使用第一条文本，每次预热单独写记录并从正式分位数中排除；正式各轮按固定种子 20261020 打乱文本顺序，模型噪声配置保持上游默认，不宣称输出确定性。每个 WAV 保存实际 SHA-256。

- `run.json`：argv、配置/清单/二进制/资产 manifest 哈希、固定模型选项、解析后的资源路径、加载时间与实际共享库路径。
- `results.jsonl`：每次预热及正式尝试，包含 ID、原文、分段、词典外英文词、成功/失败、失败原因、Generate/首段/完整处理时间、RTF、原生与内部采样数、末帧有效长度、WAV 哈希。失败不丢弃，也不伪造 RTF。
- `summary.json`：完整结果与 P50/P95/max（nearest-rank）、成功/失败/预热失败数、峰值 RSS、最小 MemAvailable、性能门槛和保留为 false 的 A05 全项状态。
- `resources.jsonl`：50 ms 采样的进程 RSS、系统 MemAvailable 与相对时间。
- `*-native.wav`：44100 Hz 单声道 PCM16；`*-16k.wav`：16000 Hz 单声道 PCM16。原生适配接口返回 float，文件按 PCM16 保存以便外部试听。探针不会主动播放。

`generate_s` 只计实际 sherpa Generate 调用，原生结果检查/复制耗时另在分段 `adapter_s` 和整体时间里。`first_segment_s` 从分句开始计到首段生成并转换出完整 PCM 帧；这是首段可用时间，不是流式模型首样本延迟。`complete_s` 包含分句、所有分段 Generate、检查/复制、重采样与最终尾帧冲刷，排除 WAV 写盘与文件哈希。两种 RTF 的分母都采用原生音频时长。分位数只对成功的正式尝试计算，同时必须并列失败数；只要有失败，性能门槛不通过。

## 本轮验证

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
