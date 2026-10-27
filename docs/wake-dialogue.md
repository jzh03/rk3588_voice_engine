# P5-01 语音唤醒与连续对话

P5-01 增加可选 CPU KWS 和对话窗口。原 P4 按键入口及已确认参数继续使用；新入口只增加 KWS、关键词文件和唤醒开关，语音打断仍关闭。结果见[环境 §57](environment.md#57-p5-01-唤醒与对话窗口自动化验证)。真人5正例/5负例/60秒背景快速检查及软件收尾已完成；窗口已补单调时钟保护，用户允许换页且要求不再跟进。用户已确认P5-01按功能范围完成；精确声学触发延迟仍未测，同类计时已从当前及后续验收移除。P5-02已完成实现、定向修复和约定的耳机批次，修正后3/3有效打断、5段0误打断，用户已确认按功能范围结项、M2完成；见[收尾记录](environment.md#577-p5-01软件收尾与范围确认2026-10-27)。

## 构建与运行

沿用[实时主引擎构建命令](realtime-engine.md#构建与配置)。ASR 探针开关同时构建 `voice_engine_kws` 和 `kws_probe`，复用已有 Sherpa 1.12.14 / ORT 1.17.1-patched。默认无模型构建只包含关键词解析和控制器，不依赖模型 SDK。

公开模板为 [config/wake.yaml](../config/wake.yaml)，本机入口为被忽略的 `config/wake-local.yaml`。本机入口从现有实时配置复制，仅新增/启用唤醒相关项。原 `config/realtime.yaml`、`config/realtime-local.yaml`、`config/local.yaml` 不改。新配置仍为一次加载的完整 YAML，不与其他配置合并。

~~~bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  build/model-probes/bin/voice_engine \
  --config config/wake-local.yaml --control stdin \
  --run-id p5-wake-001 --output-dir output/runs
~~~

所有模型进程继续使用同一个运行锁及新的 run ID。`--check-config` 校验配置和启用的模型/依赖资产，不加载 KWS，也不验证关键词与模型词表的对应关系；关键词内容在模型入口加载时校验。`kws_probe` 可用文件验证实际编码，无需打开耳机。

等待 `ENGINE_READY` 和 `WAKE_LISTENING` 后说“你好小派”；出现 `WAKE_DETECTED` / `STATE LISTENING` 后再说问题。此阶段采用“唤醒后说问题”，不保证把与唤醒词无间隔连说的同一句指令完整识别。唤醒词不重放给 ASR；KWS 检出后已经排队的后续 PCM 保留在原采集 ring 和至多一个模型输入槽中，不因切换重新打开或清空采集。

原[控制命令](realtime-engine.md#运行与命令)保留。`start` 可以覆盖等待唤醒或当前轮次，但先经过取消屏障；`cancel` 结束当前对话并返回等待唤醒；`quit` / EOF / 信号才停止应用监听。`status` 增加 `wake_listening`。断连、低内存和采集不连续取消当前检测/轮次；恢复后开启新唤醒会话，不复用旧问答。KWS模型错误或调用超时则锁存在ERROR，避免反复自动重启；取消屏障结束后须显式start或new-session恢复，若原生调用不能在取消期限内结束仍退出5。

## 状态与窗口

- IDLE 中只向 CPU KWS 送音，四个既有模型保持驻留，不在此阶段调用 ASR 或 LLM。每次重新等待唤醒都重建 KWS stream，清除特征/编码器/解码状态，增加 session 并清空 LLM 应用历史。
- 命中后停止 KWS 接收，等待其 worker 空闲，在相同音频会话中进入 LISTENING。旧 generation 的唤醒事件不能开启新轮。
- 唤醒模式将等待开口时长取为 `dialogue.window_ms`（15000 ms），同时使用有效采集样本计时和单调时钟截止保护；采集中断时不再必须凑满15秒PCM才结束。截止检查由模型worker非阻塞轮询；VAD已经确认语音后不按开口截止时间截断，继续沿用最短语音、尾静音和最长发言规则。原生调用、调度及收尾仍受既有监督，日志中的实际关闭时间单列。不修改原配置的 `vad.start_timeout_ms`，按键模式和文件探针保留原样本计时。
- 正常非空回答经过 drain 和历史提交后，自动开启新一轮 15 秒窗口，保留 session、增加 generation。无声结束不生成回答、不提交历史，回到新的等待唤醒会话。
- FINALIZING / RESPONDING中不运行KWS；原配置关闭打断，P5-02可在RESPONDING且首帧PCM已被后端接受后启用独立CPU VAD检测，详见[语音打断](barge-in.md)。未引入AEC。ASR/LLM 继续顺序使用 NPU，TTS 可与本轮 LLM 流水执行。

原未启用打断配置的采集总容量仍为2000 ms（后端99帧、模型1帧）；P5-02在同一额度内分配预录，见[队列边界](barge-in.md#交接与边界)，不另开无界缓存。KWS 同步调用在 worker 中运行，主线程继续处理取消；单次调用沿用 `asr_final_ms` 监督预算，取消确认仍为2秒，超时退出5。SDK特征、模型状态与工作内存另计入资源采样。

## 模型与关键词编码

使用计划首候选 `sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01` 的 epoch-12/avg-2/chunk-16/left-64 三个 FP32 ONNX，CPU单线程。原四模型及所有运行库不变。模型清单增加独立 candidate 项，登记来源、归档和逐文件SHA-256、模型卡许可、角色与依赖；未知原权重revision/导出命令保留unknown，不扩大原P0 verified组合。

模型采用带声调的声母/韵母 token。初始文件 [wake-keywords.txt](../config/wake-keywords.txt) 为：

~~~text
n ǐ h ǎo x iǎo p ài :1 #0.25 @你好小派
~~~

`dialogue.keywords_file` 相对当前YAML目录解析，也接受绝对路径；`dialogue.wake_word` 必须与末尾 `@` 标签完全一致。只接受一行非空关键词，最多64个token、文件最多4096字节。token必须属于选定词表，禁止blank/unk等特殊项；拒绝空词、多行、标签不匹配、未知token、重复或非有限score/threshold和错误顺序。`:` 为boosting score（默认1，范围(0,10]），`#` 为threshold（默认0.25，范围(0,1]）。固定max_active_paths=4、num_trailing_blanks=1；本轮未调阈值。

换词时同时修改词名与编码文件；词表可表示不等于召回合格。规则见 [Sherpa官方KWS文档](https://k2-fsa.github.io/sherpa/onnx/kws/index.html)，模型来源见[官方预训练模型](https://k2-fsa.github.io/sherpa/onnx/kws/pretrained_models/index.html)。不自动安装Python编码依赖，也不根据测试结果自动调参。

## 有限自动验证

无模型CTest检查关键词、状态、缓存交接、会话与取消边界。文件探针只加载KWS及CPU依赖，即使输入YAML还声明其他模型：

~~~bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  build/model-probes/bin/kws_probe \
  --config config/wake-local.yaml --wav /absolute/path/wake.wav \
  --output /absolute/path/new-kws-report.json
~~~

WAV要求16 kHz、单声道PCM16，可重复 `--wav`。每个文件重建stream，按至多320个样本送入，末尾补1000 ms诊断静音；报告补零量、命中位置、耗时和RTF。模型token时间不是声学词尾，文件加速解码时间不是端到端唤醒延迟。`KWS_PROBE_OK` 只表示程序完成，须审核JSON的hits判断是否命中。

`tests/integration/wake_runtime.py` 是显式执行的五模型/私有PipeWire检查，未注册普通CTest，依赖板端现有PyYAML。提供准备好的唤醒WAV、固定问句和新证据目录；只改隔离服务的设备标签，保留TTS/LLM/线程参数。默认有界模式执行至少300秒、五轮回答、两次唤醒和两次无声窗口。显式加 `--formal-a06` 时先完成1轮全链路预热，再独立计量至少300秒/5轮，两个无声窗口中一个注入1.75秒私有输入间断；独立采样系统内存/换页、进程VmSwap/major faults、温度和频率，缺失的可选PSI明确标不可用。计量区间与预热分别记录，A06仍须按冻结换页规则复核，脚本功能通过本身不等于资源验收通过。两种模式均记录库映射，不使用用户耳机服务。命令在本轮证据的 `runtime-command.json`，不因文档更新重跑。

## 日志与验收进度

新增 `wake_listening/wake_detected/dialogue_window_opened/dialogue_window_closed`。startup记录启用模型、唤醒开关、窗口和关键词文件哈希；resource_sample保留原数据，pipeline增加wake_mode及KWS调用起点。日志不保存原始麦克风PCM、完整转写或问答。

按2026-10-27用户最新选择，已完成真实耳机正例5次、相似词/普通讲话负例5次和1分钟日常背景快速检查；正例5/5，负例与背景均无误触发，用户确认一次正常回答。窗口和新会话切换穿插完成，没有另加一批。原第3次墙钟16.754秒结果保留；后续已补单调时钟保护并定向复核，未重跑真人批次。用户已接受换页为范围限制，不再跟进；用户已按功能范围确认结项，声学延迟保留未测，同类声学计时不再列入当前或后续待办。详见[实机记录](environment.md#576-a11真人耳机快速检查2026-10-27)和[软件收尾](environment.md#577-p5-01软件收尾与范围确认2026-10-27)。门槛、原范围及修订依据集中见[验收说明](ACCEPTANCE.md#a11-缩减范围与背景测试说明)。1分钟背景不外推长期误唤醒率。本轮引擎已退出，参数未改，不因文档同步重复测试；P5-02已执行约定的5次插话与5段安静播报，修正后3/3有效打断、5段0误打断，原5/5门槛未满足保留，用户已确认按功能范围结项，见[当前打断进度](barge-in.md)。
