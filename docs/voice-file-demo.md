# 完整文件语音链路

`voice_file_demo` 对应 P3-01 / A07：一个进程常驻 Silero VAD、RKNN Zipformer ASR、RKLLM Qwen3.5-2B 与 CPU MeloTTS，依次执行 WAV → 实际识别文本 → LLM 回答 → TTS WAV。ASR 的 `hypothesis` 原样作为本轮 LLM 输入；参考文本、关键词和人工评分只用于独立审核。每条输入创建独立会话，CPU TTS 可与本轮 LLM 流水重叠，下一轮 ASR 等本轮模型空闲后才开始。

本轮正式结果与残留内容问题集中在[环境 §53](environment.md#53-p3-01-完整文件链路与-a07-验收)。本文件说明工具契约；验收阈值见 [A07](ACCEPTANCE.md#2-用例矩阵)。工具不打开录音或播放设备。

## 构建与配置

所有命令在板端 `/home/orangepi/projects/rk3588_voice_engine` 执行。沿用已有模型和私有 SDK，不建立 Windows 开发副本：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake -S . -B build/model-probes -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DVOICE_ENGINE_BUILD_ASR_PROBE=ON \
  -DVOICE_ENGINE_BUILD_LLM_PROBE=ON -DVOICE_ENGINE_BUILD_TTS_PROBE=ON
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake --build build/model-probes --target voice_file_demo -j2
```

三个探针开关同时启用才构建本工具；默认构建不引入其模型依赖。目标及无模型回归注册见 [CMake 文件](../cmake/voice_file_demo.cmake)。共用原有 adapter、RPATH 和资产校验，不替换模型或系统 runtime。

使用专用 [config/voice-file-demo.yaml](../config/voice-file-demo.yaml)：文件模式、固定四模型启用，KWS 关闭，TTS 4 线程，LLM 最多 64 输出 token；上下文、分段、队列与超时采用共享配置默认值。工具要求 `audio.profile=file-only`，不允许 wake/barge-in。它不会修改真实耳机的 `config/local.yaml`，也不启用候选读音词典。配置由现有解析器一次加载，不与另一个 YAML 合并。

## CLI 与输入契约

```text
voice_file_demo --config PATH --manifest PATH --output-dir ROOT --run-id ID
  [--sample ID ...] [--warmup-rounds 1..20] [--tts-mode stream|batch]
  [--validate-input-only]
```

- `--output-dir` 默认 `output/runs`；真实运行必须提供 `--run-id`。`ROOT/ID` 排他新建，拒绝覆盖。run ID 与 sample ID 都须为 1～64 个 ASCII 字符，以字母或数字开头，其余只允许字母、数字、`_`、`.`、`-`。
- 默认按 manifest 顺序处理全部输入。重复 `--sample` 可选择多个不同 ID，执行顺序仍以 manifest 为准；筛选运行不自动取得正式 A07 资格。
- `--warmup-rounds` 默认 1。先从同一已选输入循环执行独立预热，再对每条已选输入执行一次测量。预热记录到 `warmup/`，不计入正式样本数和测量时段；会话、历史和队列统计均与正式轮次分开。
- `--tts-mode stream` 为默认值：逐个有效 LLM 文本包提交 TTS。`batch` 等 LLM 完整结束后提交全文，仍使用同一分句器和有界 worker。不同实时生成之间不能假定 token 时间线相同。
- manifest 必须是 schema version 1、至多 4 MiB 的普通 JSON 文件，包含 1～256 条样本。选中样本声明路径、SHA-256、采样率、声道、帧数、时长和行为；WAV 必须为 16 kHz、单声道、PCM16 RIFF，时长与帧数一致且不超过 15 s。路径相对 manifest 所在目录，拒绝绝对路径、`..` 和符号链接逃逸。
- `expected_behavior` 支持 `speech`、`empty`、`silence`、`no_speech`；零帧只允许非 speech 样本。非空录音的端点由既有 ListeningAsr 决定，报告实际消费量，不伪称总是读完文件全部帧。

实际识别为空时记录 `skipped_empty`，不启动 LLM/TTS、不生成回答 WAV；若原本期望 speech，该跳过使整批软件结果失败。预期非语音却识别出非空文本时也失败。空/静音诊断成功不计入正式 20 条完成数。其他阶段错误记录本轮 `failure_stage`、`error` 后终止批次，后续输入不再提交。

预检只读配置、清单结构及 WAV 头，不加载模型、不校验模型或 WAV 内容哈希，也不获取模型锁或创建输出目录。此模式允许省略 run ID：

```bash
build/model-probes/bin/voice_file_demo \
  --config config/voice-file-demo.yaml \
  --manifest output/runs/p1-natural-review-20261022-104035/natural/manifest.json \
  --validate-input-only
```

成功输出 `VOICE_FILE_INPUT_OK` 及已选样本数、预热轮数、TTS 模式；`A07=not_run`。真实运行在共享锁内重新核对输入哈希和 WAV 头，再验证模型资产与实际加载库。

## 监督运行

通过 [voice_file_validate.py](../tests/integration/voice_file_validate.py) 监督整个进程组：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 tests/integration/voice_file_validate.py \
  --config config/voice-file-demo.yaml \
  --manifest output/runs/p1-natural-review-20261022-104035/natural/manifest.json \
  --warmup-rounds 1 --tts-mode stream --timeout 900 \
  --output output/runs/voice-file-formal-example
```

示例目录必须尚不存在。监督器默认调用 `build/model-probes/bin/voice_file_demo`，可用 `--executable` 指定已构建程序；`--sample` 可重复。监督器固定将工具 run ID 设为 `results`，因此结果位于 `--output/results/`。`--timeout` 为 1～7200 s，默认 900 s，覆盖输入/资产校验、初始化、预热、测量和收尾。

外层锁覆盖监督器及全部子进程，不在内部重复获取同一把锁。编译、CTest 与大文件校验也遵守该锁，性能采样期间禁止并行执行。监督器每秒保存资源，约每 50 ms 检查内存和期限；MemAvailable 低于 1572864 KiB、到期或收到中断时结束受管进程组，并检查退出后是否残留该组子进程。stdout/stderr、命令、传感器来源与监督结论保存在 `results/` 的父目录。

工具在本轮模型循环中另有 120 s 期限与共享 adapter 超时。同步 SDK 调用不能保证被程序内检查即时抢占，外部进程组监督负责最终停止边界。信号检查覆盖输出成功确认、shutdown 和最终报告提交；失败不会覆盖为成功。该诊断工具的退出处理不代表完整 A08 取消验收。

## 证据与独立审核

| 产物 | 主要内容 |
|---|---|
| `results/run.json`、`summary.json` | 配置/manifest/程序身份、选中样本、初始化、模式、预热与正式计数、模型库、资源汇总、最终输出哈希 |
| `results/events.jsonl` | 验证、模型常驻、ASR/LLM/TTS、输出、失败、测量和 shutdown 事件 |
| `results/rounds.jsonl` | 每轮完整结果；使用 `phase`、`warmup` 区分预热与测量 |
| `results/samples/<sample_id>/result.json` | 输入身份、识别、真实 LLM 输入及回答、TTS 分段、时序、队列、WAV 路径与状态 |
| 同目录 `packets.jsonl` | LLM 包的 session/generation、从 1 起的序号、token ID、文本、回调时间及从 LLM submit 起算的 `offset_ns` |
| 同目录 `pcm_frames.jsonl` | 从 0 起的 PCM 帧序号、有效采样数、身份及实际 PCM16 字节的 `pcm16_fnv1a64` |
| 同目录 `output-16k.wav` | 本轮 16 kHz 单声道 PCM16 回答，最多 120 s；不存在于空识别轮次 |
| `results/warmup/<round>-<sample_id>/` | 独立预热结果及输出，结构同上 |
| `results/resources.jsonl` | 工具的每秒/轮末内存、swap 和队列观测；预热峰值不混入测量 |
| 父目录 `supervisor.json`、`supervisor-resources.jsonl` | 实际退出、进程身份、内存保护、资源采样及频率/温度等可读指标 |
| `results/failure.json` | 有输出目录时的失败阶段、退出码和已有结果索引；强制终止时可能只留下部分证据 |

本轮关联使用 `run_id`、`sample_id`、`phase`、`round`、`session_id`、`generation_id`。ASR 尚未启动 LLM 时 session/generation 为 null，依靠 run/sample/round 关联；空识别不伪造 LLM 身份。TTS 实际分句保留在 `tts_generations`，包括原文本与 Melo 前端文本；边缘空白处理和中文逗号策略沿用 [TTS 契约](tts-probe.md)。

PCM 提交在当前 generation 的有界内存操作中完成，WAV I/O 在提交锁外执行，文件写入并核对成功后才确认 LLM 历史；下一条仍重置会话。逐帧 FNV1a64 用于独立核验 PCM 与 WAV 字节对应关系，是非密码学检查。完整 WAV SHA-256 在采样和模型关闭后计算，不放入测量窗口。

[review_voice_file.py](../scripts/review_voice_file.py) 不运行模型，独立核对 manifest/冻结自然集、所有轮次、实际 ASR→LLM 文本、包/分段/PCM/WAV 对应、资源监督与人工内容评分。全部采样结束后运行；以下 `CONTENT_REVIEW.json` 和 `BOUNDARY_RUN/results` 需替换为实际已有证据路径，审核输出须为新文件：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 scripts/review_voice_file.py \
  --run-dir output/runs/voice-file-formal-example/results \
  --manifest output/runs/p1-natural-review-20261022-104035/natural/manifest.json \
  --content-review CONTENT_REVIEW.json \
  --boundary-run BOUNDARY_RUN/results \
  --output output/runs/voice-file-formal-example/independent-review.json
```

`--boundary-run` 可重复，提供同样经过监督器运行的独立边界结果，合计覆盖 `empty` 与 `silence`，证明没有下游回答。边界 manifest 采用相同输入契约，其内容不加入正式20条分母。

内容审核 JSON 使用 `schema_version=1`、`reviewer={kind: agent|user, name: ...}`、`rounds_sha256` 和 `reviews` 数组。每条须包含 `sample_id`、回答 UTF-8 文本的 `answer_sha256`、布尔值 `relevant` / `no_obvious_factual_contradiction` / `within_short_answer_scope` / `no_thinking_or_control_marker_leak`、布尔值 `input_correspondence` 及非空 `review_note`；可选 `passed` 必须与四维布尔值一致。审核人必须逐条判定内容，脚本不会从软件连线自动推断语义；agent 评分不冒充用户试听。

工具自身只报告软件结果，20/20 完成时仍为 `A07=pending_external_review`，其他诊断为 `not_run`。独立审核缺少正式冻结语料、完整监督、内容评分或空/静音边界证据时不会给出 A07 通过。需读取审核报告的 `A07`，不能仅凭退出 0 判验收完成。

## 退出码、性能与范围

| 来源 | 退出码 |
|---|---|
| 工具 | `0` 软件检查通过；`2` CLI/配置/资产/锁错误；`4` 模型或报告/音频 I/O 错误；`5` 本轮期限；`6` 资源或流程条件失败；`128+signal` 已观察到的信号中断 |
| 监督器 | 正常结束透传子进程退出码；监督期限、内存保护、中断或残留进程组等监督失败返回 `5` |
| 独立审核 | `0` 表示未发现已检查证据或质量失败，但 A07 仍可能为 `not_run`；证据核验或正式质量失败返回 `6` |

监督器/审核脚本的 CLI 语法错误、无法创建审核输出等使用对应 Python/系统错误退出，保留 stderr。所有失败目录和旧审核文件均保留，新尝试使用新目录。

`asr.processing_s`/`rtf` 是文件处理时间及其与输入时长之比；`first_pcm_s` 从轮次开始计到首次消费 PCM，`llm_to_first_pcm_s` 从 LLM submit 起算，均不是耳机实际起声延迟；缺失首 PCM 时记录 null。模型初始化、预热、正式测量分开报告，资源队列统计不包含 SDK/NPU 内部全部内存。NPU 专用内存不可读时明确标记，RSS 不能代表其总量。

同条件流式/整段比较应复用 `packets.jsonl` 的同一 token 时间线，交给 [llm_tts_probe 的 replay](tts-probe.md#p2-流式队列与补充读音2026-10-22) 分别执行 stream/batch；性能对照与结论仍归环境 §53。本工具没有联网查询、设备控制或音乐播放能力，文件链路成功不等于 P4 实时引擎、设备取消功能、软件延迟或长稳通过；外部声学计时已从当前及后续验收移除。

自然录音仍是板端私有固定集，版本化 catalog 不是可直接运行的 manifest。诊断产物含转写、完整回答及生成音频，只保存于被忽略的私有 `output/runs/`；对外文档引用证据身份和汇总，不提交私有录音、完整对话或凭据。
