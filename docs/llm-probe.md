# LLM 常驻适配与文件探针

`llm_probe` 使用常驻 `LlmAdapter` 验证固定 Qwen3.5-2B W8A8 与 RKLLM 1.3.0 的文本生成、上下文、超时和取消。探针仅处理文本及文件日志，不打开录放设备。它对应 P2-02 的模型侧工作；完整验收条件仍以 [ACCEPTANCE](ACCEPTANCE.md) 的 A04/A08 为准，本轮 P2-02 模型侧交付已通过，运行结果及限制由[环境记录 §48](environment.md#48-p2-模型适配端点与流式合成)集中维护。

实现入口为 [LLM 接口](../include/voice_engine/llm.hpp)、[常驻适配器](../src/llm/adapter.cpp)、[RKLLM 后端](../src/llm/rkllm.cpp)和[探针](../tools/llm_probe.cpp)。当前沿用选定 R 的通用短答模板及固定采样配置；模型、SDK、题目、题目预算和评分门槛均不随探针模式改变。

**构建需显式开启 `VOICE_ENGINE_BUILD_LLM_PROBE`。** 默认构建关闭该工具，不依赖私有 RKLLM SDK。开启后需要 `VOICE_ENGINE_BUILD_ENGINE=ON`、板端 `deps/rkllm-1.3.0/include/rkllm.h` 和配套 `lib/librkllmrt.so`；运行时还会按资产清单核对固定模型、runtime 及实际加载路径。不会自动下载或替换依赖。

以下命令均在板端工程根目录执行。共享构建目录的配置、编译、CTest、模型校验和推理串行使用同一把锁；性能采样期间不并行编译或大文件校验。保留现有 build 目录中 ASR/TTS 的开关设置，不同时从另一终端重配置。

```bash
cd /home/orangepi/projects/rk3588_voice_engine
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake -S . -B build/model-probes -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVOICE_ENGINE_BUILD_ENGINE=ON -DBUILD_TESTING=ON \
  -DVOICE_ENGINE_BUILD_LLM_PROBE=ON
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake --build build/model-probes --target llm_probe llm_adapter_test -j2
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  ctest --test-dir build/model-probes --output-on-failure \
  -R '^(llm_adapter|llm_probe_cli)$'
```

`llm_adapter` 使用替身后端覆盖生命周期、预算、UTF-8/控制标记分片、取消竞态、关闭后的提交拒绝，以及 `run` 返回但 SDK 仍活跃时的 callback 保活；`llm_probe_cli` 检查参数和用例输入。这两个 CTest 不加载模型，不能替代实机矩阵。完整工程回归仍按 README 的统一构建流程执行。

**模型侧配置使用 [config/llm-probe.yaml](../config/llm-probe.yaml)。** 该配置仅启用 `qwen3.5-2b-rk3588-w8a8`，其他模型关闭；`file-only` 音频配置用于保持配置结构一致，LLM 探针没有音频 I/O。当前限制如下，未显式写入的队列大小使用共享配置默认值。

| 项目 | 当前值与含义 |
|---|---|
| 上下文 | 2048 token；当前后端只接受这个固定上下文配置 |
| 默认输出预算 | 128 token；固定 20 题的每题预算仍为 fixture 中的 64 token |
| 保留余量 | 至少 64 token；当前配置为 64 |
| 首 token / 总生成期限 | 10000 / 30000 ms，从提交请求时计时 |
| 取消确认期限 | 2000 ms；超时置为 `cancel_timeout`，要求进程恢复 |
| LLM 回调文本队列 | 默认 8192 字节，另有最多 1024 个待消费 packet 的限制；满队列触发受控取消 |

可先检查固定题目结构：

```bash
build/model-probes/bin/llm_probe \
  --cases tests/fixtures/llm_cases.json --validate-input-only
```

该命令只检查题目 JSON、ID、UTF-8、控制标记和每题预算，不校验模型资产、不初始化模型，也不代表题目已经执行。固定 fixture 顶部的 `execution_status` 和 `execution_prohibited_in_this_round` 是备料阶段保留的历史快照；当前执行授权、结果和验收状态记录在本轮证据与文档中，不通过改写题目文件消除历史状态。

**`llm_probe` 提供四种执行模式。** 固定参数入口如下；模式名和参数不可重复，`run-id` 为 1～96 个 ASCII 字符，以字母或数字开头，其余只允许字母、数字、`_`、`-`。已有 `output-dir/run-id` 会被拒绝，避免覆盖原始证据。

```text
llm_probe --config PATH --cases PATH --run-id ID [--output-dir ROOT]
  [--mode cases|lifecycle|boundaries|cancel] [--repeat 1..100]
  [--warmup 0..10] [--init-cycles 1..20] [--case-id ID]
```

| 模式 | 执行内容 | 参数与判定范围 |
|---|---|---|
| `cases` | 同一常驻句柄执行固定问题，每题开始新会话；每批使用可复现的打乱顺序 | 默认 `--repeat 3 --warmup 1`。正式 A04 保留全部 20 题和每题 64 token；`--case-id` 仅用于局部诊断，不能算完整批测 |
| `lifecycle` | 每次重新初始化、执行短问答、确认正常结束并关闭句柄 | 默认 `--init-cycles 10`。初始化耗时单列；该模式不执行统一预热循环，`repeat` 不改变循环次数 |
| `boundaries` | 5 组“记住水果→追问→新会话”，超长输入拒绝，以及强制首 token/总生成超时后的同句柄恢复 | 每组首轮成功后显式提交历史；超长输入在 SDK 调用前被拒绝。诊断请求将首 token 期限缩短为 5 ms，另一请求使用 700/800 ms 首 token/总期限；这些注入不改生产默认值 |
| `cancel` | 首 token 前取消 5 次、生成中取消 5 次，每次立即新会话并用同句柄执行恢复问题 | 首类在提交后约 5 ms 请求取消，生成中在观测到至少 3 个 token 回调后请求取消；逐次核对实际时序、abort 返回、空闲确认、旧提交拒绝和历史清空 |

`boundaries` 和 `cancel` 的用例数由模式定义，`repeat` 不扩大它们；除 `lifecycle` 外，`warmup` 在正式模式用例前执行。预热输出不会提交到历史，正式问题前会新建会话。默认预热最多生成 32 token。比较有/无预热时，应使用相同源码、配置、题目和输出预算分别运行 `--warmup 0` 与 `--warmup 1`，初始化、无预热首轮和后续稳态结果分别记录；预热请求本身不进入正式题统计。

**实机运行优先使用 [llm_validate.py](../tests/integration/llm_validate.py) 监督整个子进程。** 脚本默认调用 `build/model-probes/bin/llm_probe`、`config/llm-probe.yaml`、`tests/fixtures/llm_cases.json`；可用同名 `--executable`、`--config`、`--cases` 参数显式指定。每次 `--output` 必须是新目录，内部探针固定写入 `results/`。下面四条命令按顺序执行，锁包住监督脚本及其整个模型子进程；不要在子进程内再嵌套获取同一把锁。

```bash
llm_run_stamp="$(date +%Y%m%d-%H%M%S)"
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 tests/integration/llm_validate.py \
  --mode cases --repeat 3 --warmup 1 --timeout 900 \
  --output "output/runs/llm-cases-${llm_run_stamp}"
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 tests/integration/llm_validate.py \
  --mode lifecycle --init-cycles 10 --timeout 900 \
  --output "output/runs/llm-lifecycle-${llm_run_stamp}"
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 tests/integration/llm_validate.py \
  --mode boundaries --warmup 1 --timeout 900 \
  --output "output/runs/llm-boundaries-${llm_run_stamp}"
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 tests/integration/llm_validate.py \
  --mode cancel --warmup 1 --timeout 900 \
  --output "output/runs/llm-cancel-${llm_run_stamp}"
```

监督脚本的 `--timeout` 单位为秒，范围 1～3600，默认 900；它限制整个进程，包含资产校验、初始化及所有请求，与 adapter 的单轮期限独立。脚本每 250 ms 采集资源；启动前要求 `MemAvailable ≥1.5 GiB`，运行中低于该值持续 3 s 或超过总期限时，只向自己启动的进程组发送 TERM，5 s 内未退出再发送 KILL。探针内部也记录资源并停止低内存下的新请求。首次完整模型哈希校验在探针的正式性能采样窗口之前完成。

**接入方应沿用 adapter 的生命周期和提交契约。** 构造时初始化一次；`initialize/run/clear/shutdown` 由持久 worker 串行执行，监督线程负责期限和 abort 检查。每轮 SDK 使用 `keep_history=0`，推理前后清 KV；应用生成完整模板，SDK 的 system/prefix/postfix 包装置空，避免重复套模板或隐式积累历史。系统模板明确只能生成本地文字短答，不具备联网、播放音乐或操作设备能力；该模板约束仍需通过内容评分检查。

1. `start(text, ...)` 返回带 `session_id`、`generation_id` 的 `LlmTurn`。若上一轮未结束，会先失效旧轮并等其结束；无法确认或句柄已经 poisoned 时拒绝复用。输入为空、UTF-8 非法、含模板控制标记或超过预算时拒绝请求。
2. 每轮只保留一个消费方，通过 `poll()` 取 packet。packet 带序号、单调时间戳、token ID；`text` 可能为空，不能把空文本当作 EOS。回调只投递有界队列，不执行 TTS 或等待播放。
3. 输出过滤器跨 packet 保留未完成的 UTF-8 字节和控制标记前缀，过滤 `<think>…</think>` 及 `<|…|>`，输出完整 UTF-8。原始文本留在 `raw_text` 供诊断，非法编码设置 `invalid_utf8`，不能提交历史。Markdown 和适合朗读的分句由下游文本/TTS 层继续处理。
4. `wait()` 会消费剩余 packet 并返回结果。要保存每一包或流式提交下游，先完整消费 `poll()`；在确认 idle 后再做最后一次取包，最后调用 `wait()`。本探针先观察 idle 再 poll，并核对完成轮次的已交付文本拼接与最终文本相等，避免最后一包被 `wait()` 静默消费。
5. 下游入队使用 `commit_if_current(turn, sink)`，把轮次检查与提交放在同一同步边界。`sink` 必须立即完成，不得阻塞、执行 I/O、调用 SDK 或回调 adapter。先检查 generation 再在锁外提交不能替代该接口。
6. 只有确认本轮输出成功后，应用才调用 `commit_history(turn)`。它仅接受当前、完成、已消费到末尾、非预热、有效 UTF-8 且非空的结果，并且每轮最多提交一次。文件探针在上下文用例中显式确认文本；将来播放链路应在其所定义的成功输出边界确认。取消、过期或未完成结果不得进入历史。
7. `new_session()` 失效旧轮并清空历史。应用历史最多保存 32 对用户/助手消息；每次提交新输入时按预算从最早的完整消息对开始剔除，不切掉半个对话轮次。

输入准入采用完整渲染 prompt 的 UTF-8 **字节数**，包含系统模板、历史、当前输入和模板控制字符串，再加本轮输出预算和保留余量，要求总和不超过 2048。日志中的 `prompt_estimated_tokens` 因此是保守字节估计，不是 tokenizer 的准确计数；不要把它当作实测 prefill token 数。请求级 `RKLLMInferParam.max_new_tokens` 接收本轮输出预算，覆盖初始化时的默认值；完成轮次同时核对 SDK 原始性能计数、adapter 有效 token ID 回调数和已交付有效 ID 数。SDK `generate_tokens` 是生成阶段的性能计数，在实测 4 token 上限时回报 3，不能等同于总输出数，也不能统一加一修正；带有效 ID 的空文本包仍计 token，无 ID 的文本冲刷包不计。观测输出超过预算会明确判失败。队列字节上限和文本组装限制也不代表模型总内存上限。

**取消先使旧 generation 失效，再请求底层中止。** `cancel()` 同步失效当前轮、清空待交付文本队列；迟到 callback 仍可能发生，但不得进入当前轮输出。监督线程请求 `rkllm_abort`，worker 等待 `run` 返回并确认 `rkllm_is_running()==0`，且等旧 abort 调用结束后才清 KV、完成旧轮，允许下一轮使用句柄。abort 返回不等于模型已经空闲，FINISH callback 也不是唯一完成条件。

如果取消确认超时，结果为 `cancel_timeout`、`restart_required=true`，当前进程中的该句柄不再接受新请求。`close()` 使所有当前轮次失效并清空历史，已完成轮次也不能继续 poll 或提交；它有界等待 worker/监督线程退出，返回 `false` 表示未完成安全释放。即使 `run` 已返回但 SDK 仍活跃，worker 也继续持有后端与 callback userdata，直到确认 idle 才清理和销毁；不会强制销毁活动句柄。调用方应终止并重启受监督进程，不能用 `new_session()` 把 poisoned 句柄恢复成可用。

**证据分为监督层与探针层。** 监督脚本会保存子进程原始 stdout/stderr，不应仅凭最后一行或退出 0 宣称内容或整机验收通过。

| 文件 | 内容 |
|---|---|
| `command.json`、`supervisor.json` | 实际命令、子进程返回码、总耗时、监督触发原因及受管进程组 |
| `stdout.log`、`stderr.log`、`supervisor-resources.jsonl` | SDK 原始输出和监督层内存/RSS/峰值/swap 采样 |
| `results/run.json` | 模型/runtime 路径、profile、模板和限制，题目/配置/二进制/相关源码哈希 |
| `results/tokens.jsonl` | 实际交付 packet 的题目、批次、阶段、session/generation、seq、时间戳、token ID 和过滤后文本 |
| `results/metrics.jsonl` | 每轮状态与原始/过滤文本，实际 token 数及 SDK 时长，预算、回调计数、队列峰值、退出/取消返回码和各阶段时间戳 |
| `results/resources.jsonl`、`results/libraries.json` | 探针资源曲线、CPU/NPU 频率、温度和实际加载 `.so` 路径 |
| `results/summary.json` | 自动检查逐项结果、所有轮次、初始化耗时、适用统计和失败数量；内容评分及 A04/A08 字段保持待单独评定 |
| `results/error.json` | 部分运行期异常的原因；启动早期或强制终止可能只有监督层/标准错误记录，应保留全部原始文件 |

TTFT 使用提交到首次有 token ID 的回调时间；`first_speakable_text_ms` 使用提交到消费方首次取得可用文本的时间，两者含义不同。`total_ms` 到 SDK 空闲观测点，清 KV 在结果完成前执行，但不包含在这个时间戳中。`cancel_to_idle_ms` 是取消请求到模型空闲，`sdk_tokens_per_second` 来自 SDK 生成 token 数和生成耗时。所有数值都是模型侧或软件侧时间，不能直接换算为耳机端到端延迟。

摘要的 TTFT/速率只汇总 `phase=case` 中状态为 completed 的轮次，P50/P95/max 与样本数一同报告；warmup/context/recovery/timeout 等阶段另列。存在失败时必须同时保留失败数和详情，不能用成功子集替代整批结果。`termination_reason` 优先保留非 completed 状态；正常完成时优先记录可见 EOS 为 `observed_eos`，没有 EOS 且有效 token ID 数恰好达到预算才记 `token_limit`。后者是观测推断，`termination_basis` 明确标注其依据，SDK 未直接提供内部 stop reason。低于预算且没有 EOS 保留 `sdk_finish_without_visible_eos`；超预算单独标记并使检查失败，即使最后一包是 EOS 也不能掩盖越界。日志保留 `sdk_generate_tokens`、`token_callbacks`、`observed_token_callbacks` 和 `delivered_packets` 各自口径，后者包含无 ID 冲刷包，不能当作 token 总数。

| `llm_probe` 退出码 | 含义 |
|---|---|
| `0` | 本模式适用的软件检查通过；内容评分、完整 A04/A08 另行评定 |
| `1` | 显式记录的探针 I/O 错误 |
| `2` | 参数、题目、配置、资产或路径错误 |
| `4` | 模型/运行期错误，或探针检测到低内存 |
| `5` | 非预期超时或探针监督期限超限 |
| `6` | 适用软件检查未全部通过 |
| `130` / `143` | 直接探针处理 SIGINT / SIGTERM 后退出 |

`boundaries` 中预期的首 token/总生成超时是被测行为，按预期退出并恢复后可返回 0。监督脚本在子进程正常退出时转发其返回码；由进程总期限或持续低内存保护终止子进程时返回 5，并在 `supervisor.json` 记录原因。

**固定题内容评分和验收边界保持独立。** [20 题 fixture](../tests/fixtures/llm_cases.json) 的原文、每题 64 token 和原定三批要求不变。按照 [ACCEPTANCE](ACCEPTANCE.md) 逐题记录“切题、无明显事实矛盾、符合短答范围、无思考/控制标记泄漏”，每批相关且符合短答要求至少 18/20；实时信息题不能编造联网数据。固定测试结果只覆盖这些题目，不能推广为通用知识或工具调用能力。

`cancel` 模式覆盖 A08 的 LLM 首 token 前/生成中各 5 次模型取消及恢复；旧 callback、连续 start、提交竞态、立即新会话和关闭生命周期还由无模型回归覆盖。它没有测量 TTS 合成中/播报中取消、停止软件提交 PCM 的 P95 ≤100 ms、停止后旧 PCM 提交数、播放 flush 或耳机尾音。后续 P2-04 的共驻与顺序 NPU 调度、P3 的完整文件链路以及 P4 的实时状态机/音频与完整 A08/A10，仍需按执行计划分别验证。LLM→TTS 的局部文件联调说明放在 [TTS 探针文档](tts-probe.md)，不能替代这些整机条件。

## P2-04 共驻观测接口

`LlmResult.queue_current_bytes/queue_current_packets` 是当前待消费队列快照，原峰值字段仍保留历史峰值；消费或取消后当前值可下降，不能用峰值充当实时队列深度。接口由 adapter 现有互斥锁保护。四模型共驻的采样、顺序调度及证据见[共驻文档](coexist-probe.md)。
