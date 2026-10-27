# voicenpu_engine 参考项目总结与实施建议

审阅日期：2026-10-21（Asia/Shanghai）

性质：本地参考源码静态审阅；以下建议待实施，不是新的运行或验收结果。

## 1. 来源与查阅方式

- 参考源码位于 Windows：`F:\AI\RK3588音频引擎\voicenpu_engine-Reference Project`。后文“参考源码位置”均相对这个目录，行号对应本次快照，不能当成板端项目路径。
- 原规划记录的上游入口：[voicenpu_engine](https://gitee.com/bravexyz/voicenpu_engine)。本次依据用户提供的本地文件；目录没有 Git 元数据，未核实其上游 commit/tag，也不宣称代表上游最新状态。
- 本工程的唯一开发副本：`/home/orangepi/projects/rk3588_voice_engine`。本总结按用户要求存放在板端 `doc/`，通过 [README](../README.md) 和 [执行计划](../EXECUTION_PLAN.md) 查阅；其他工程记录仍在 `docs/`。
- 本次只读取参考源码和板端现状、更新文档。没有运行参考程序、移植源码、修改模型/配置或执行新的推理、录放和性能测试。模型保持 candidate，P0/M0 未关闭，既有未通过项保留。

本地参考包缺少 CMake 要求的同级 `rknn_model_zoo`。其中包含的自定义 Zipformer 流式底层、RKNN MeloTTS 实现及配套 SDK/模型没有随本参考目录提供，不能把这个目录当作可直接构建的完整发行包。

## 2. 工程定位与差异

参考项目以 C++17 / ROS2 Humble 组织语音对话，包含音频前端、VAD/流式 ASR、RKLLM、TTS 路由和会话调度，也包含 KWS、知识检索、回声处理及在线 TTS。README 对其平台和性能的陈述属于作者自报，本次没有复现实测。

| 项目 | 参考源码或配置 | 本工程采用方式 |
|---|---|---|
| 平台、构建 | README 实测平台为 RK3576；CMake 默认 `rk3576`，依赖 ROS2 / ament / colcon | Orange Pi 5 / RK3588S，独立 C++17 / CMake / Ninja |
| 驱动/runtime | README 列出 RKNPU 0.9.8 / RKNN 2.3.2 / RKLLM 1.3.0 | 版本号相同，但本板配套结论以[兼容性记录](../docs/compatibility.md)为准 |
| LLM | 配置为 Qwen3-VL-2B、W4A16、RK3576；提示词处理与本工程不同 | 保留已验证的 Qwen3.5-2B / RK3588 / W8A8 及固定模板 |
| 音频 | ALSA / M1066，带硬件前端和 AEC 相关处理 | 保留现有蓝牙耳机、PipeWire Pulse 兼容服务和异步 libpulse 后端 |
| TTS | 离线 RKNN MeloTTS 与在线后端，所给 YAML 默认选择 online | 使用现有 CPU sherpa-onnx / MeloTTS；四线程作为文件性能候选 |
| ASR | 包装外部 Model Zoo 的流式 Zipformer 接口 | 复用本工程已实现的增量特征、缓存复位和文件探针 |
| 调度 | ROS 节点、会话控制、generation、分句回调 | 借鉴接口和状态分工，在本工程中实现有界队列与完整取消链 |

现有能力、测试结果及未完成项以 [README 当前进度](../README.md#当前进度) 和 [执行计划任务表](../EXECUTION_PLAN.md#5-工作分解依赖与交付) 为准。参考工程不是本板模型质量、共驻内存或蓝牙延迟的证据。

## 3. 与当前问题直接相关的四项建议

<a id="r1-first-audio"></a>
### R1：短首分句与流式 TTS，优先测量首段等待

**参考做法。** `config/voicenpu.yaml:68` 将播报风格放进配置，要求第一分句六到十个汉字并有标点；`src/llm/rkllm_qwen.cpp:26` 优先按标点取段，没有标点时以约十四个字符截段。生成过程中即可向下游交付文本，生成结束时冲刷剩余文本。

**对应问题。** 本工程四线程 TTS 的 Generate RTF P95 为 0.883，但首段可用时间 P95 为 3.455 秒。速度达标不代表很快听到第一句；该数值是文件探针的首段生成/转换时间，不是耳机实际起声延迟。完整测量定义见 [TTS 探针](../docs/tts-probe.md)。

**落地方式。**

1. 保留现有 `TtsSegmenter` 的 UTF-8、数字/日期和短句 EOS 规则，在统一提示词中引导“首分句简短、完整、直接回答”，不强迫已经很短的答案凑字数。
2. 参考的六至十字要求须与本工程当前分段下限对齐，不能直接复制后让短首句一直留在缓冲区；字符数与字节数也要分开。
3. LLM 回调只整理文本并投递带 generation 的有界队列；独立 TTS worker 合成，不能在 SDK 回调内等待合成、播放或磁盘 I/O。
4. 在相同模型、线程、输入和输出预算下比较当前方案与短首分句方案。记录首可播文本、首段 PCM、完整合成耗时、段数及停顿听感；保留失败样本。最终声学延迟仍按 A10 测量，不能直接用分段耗时相加代替。

**对应任务与交付。** P2-02 / P2-03 / P3-01；正式 LLM adapter、现有分句器的流式对接及文件对比记录。接口现状见 [tts.hpp](../include/voice_engine/tts.hpp)，验收沿用 [A05/A10 与时间点定义](../docs/ACCEPTANCE.md)。

<a id="r2-llm-adapter"></a>
### R2：常驻 LLM adapter，集中管理模板、预热和恢复

**参考做法。** `src/llm/rkllm_qwen.cpp:212` 管理加载，`:250` 提供短请求预热，`:268` 处理流式生成与尾文本，`:291` 封装 abort；对象持有驻留模型句柄，并汇集 SDK 性能字段。

**对应问题。** 本工程已有固定十问、一次取消和同句柄恢复证据，但正式 `src/llm/` 仍为空。短答修正后仍有一题声称未接入的能力；加载及首 token 耗时出现过波动。具体原始结果见 [LLM 准备记录](../docs/model-prep-llm.md#2026-10-21-p0-十问取消与短答复核)。

**落地方式。**

1. 从板端已验证的私有示例整理正式 adapter 和 `llm_probe`，沿用固定模型、SDK、非思考模板及预算，不移用参考项目的 Qwen3-VL 模板。
2. 明确模型 worker、回调上下文和句柄的存活关系；加载后复用，任务结束确认后再复位或销毁，取消后按照已验证方式清理 KV。
3. 把系统提示、当前实际能力和短答要求集中管理。针对原 llm_01 回归，保留题目、原失败和评分规则；提示词约束不能被写成“已经保证没有能力幻觉”。
4. 将初始化、首次请求和受控预热后的请求分别测量。可借鉴预热接口，但本板收益尚未验证，不能把既有延迟波动全部归因于冷启动，也不能把预热计入成功用户请求或隐式会话历史。
5. 完成超时、重复取消、恢复、新会话和上下文预算回归；独立采集首 token、首可播文本及 SDK token 速率。

**对应任务与交付。** P2-02；拟新增 `include/voice_engine/llm.hpp`、`src/llm/` 实现和 `tools/llm_probe.cpp`，同步配置、构建和测试。上述路径为交付约定，尚未实现；正式验收遵循 [A04/A08](../docs/ACCEPTANCE.md)。

<a id="r3-cancel"></a>
### R3：将轮次检查与输出提交放在同一同步边界

**参考做法。** `src/scheduler/voice_scheduler.cpp:254` 的 `commitAudio` 在 generation 锁内检查并提交；`:262` 的 interrupt 在同一把锁内更新轮次，再调用模型取消。这样可以避免“检查通过后发生取消，旧结果仍提交”的竞态窗口。

**对应问题。** 板端实测取消请求后仍收到一个尾 token，约 102 ms 后才确认模型空闲；仅调用 abort 不能保证输出立即消失。当前 mock 的 generation 需要扩展到真实文本、合成及播放链路。

**落地方式。**

1. session / generation / sequence 贯穿 LLM 回调、文本队列、TTS 任务、PCM 队列和最终播放提交。
2. 在短临界区内完成轮次检查与受控入队/提交，并与取消失效操作同步；不能持有该锁等待模型、合成或设备阻塞 I/O。
3. 取消先使旧轮次失效并关闭旧输出入口，再清文本/PCM 队列、请求模型停止并 flush 播放流。旧回调可到达，但不得产生新的有效下游输出。
4. 区分“丢弃旧 TTS 结果”与“底层同步 Generate 已停止”；后者不能由空 cancel 或 generation 过滤推断。模型结束确认后才允许下一次互斥 NPU 任务。
5. 回归覆盖检查后/提交前取消、取消后尾回调、连续 start、TTS 进行中取消及立即新会话；软件停止提交和耳机实际尾音分别统计。

**对应任务与交付。** P2-02 / P2-03 / P4-01；原子提交规则、队列失效与恢复回归，沿用 [A08](../docs/ACCEPTANCE.md) 的门槛，不用一次模型取消时间代替整个音频链路结果。

<a id="r4-asr-vad"></a>
### R4：持续增量识别，由 VAD 决定何时收尾

**参考做法。** `src/engine/voice_engine.cpp:16` 持续将 PCM 交给 ASR，再更新 VAD；端点出现后调用 finish 并开始新流。`src/asr/zipformer_asr.cpp:46` 将 start / accept / finish / reset 分开包装。

**对应问题。** 本工程增量 ASR 已有跨分块一致性，但 VAD 端点尚未接入实时控制链路。参考接法可减少端点后才开始整句识别的等待，有助于整理流状态和复位边界。

**落地方式。**

1. 在 LISTENING 阶段复用现有增量 ASR、VAD 和音频帧，在端点处收尾；保留预录覆盖，避免重复喂帧或漏首尾。M1 输出期间仍遵循现有 ASR/LLM 串行调度约定。
2. 手动 stop、无声等待、最长发言、取消和设备重连都必须有明确的 finish/reset 行为；补足现有边界集未实际触发的等待开口与最长发言路径。
3. 识别质量单独诊断：现有合成集为 13/20 关键词正确，两个失败样本在官方整 WAV 示例中同样失败。先核听输入、补独立自然人声，再比较识别结果；不能据此认定所有失败都来自模型，也不能声称参考封装能提高准确率。

**对应任务与交付。** P2-01 / P4-01；端点接入、尾部与复位测试。现有结果和失败样本见 [ASR 探针](../docs/asr-probe.md)，质量门槛仍为 [A03](../docs/ACCEPTANCE.md) 原要求。

## 4. 不应原样移入的实现

| 观察到的问题 | 参考源码证据 | 本工程处理 |
|---|---|---|
| LLM 回调内同步合成 TTS | `rkllm_qwen.cpp:135` 调用 sentence sink；`voice_scheduler.cpp:221` 的 sink 内调用 synthesize | 使用有界文本队列和独立 worker；回调不等待合成 |
| 取消没有贯穿播放队列 | `src/engine/voice_node.cpp:443` 发布 PCM 时未携带 generation；`src/asr/audio_frontend.cpp:28` 允许最多 30 秒队列，`:193` 持续取出播放；`include/tts/melo_tts.h:27` 的 cancel 为空 | 在应用队列和最终提交处保留轮次，取消清队列并 flush；不能宣称参考打断已满足 A08 |
| 文本清理可能改变内容 | `src/llm/rkllm_qwen.cpp:60`、`:97`、`:114` 删除编号形式、符号和空白；可能破坏小数、负号及英文分词 | 沿用本工程数字/UTF-8 保护；新增过滤须处理跨 token 标记并有回归，不能依赖盲目字符串替换 |
| ASR/TTS 关键依赖缺失 | `CMakeLists.txt:30`、`:55`、`:89` 引用同级外部目录 | 以本板现有固定依赖为基础，不能宣称已审计缺失的底层实现 |
| 许可声明不一致 | `LICENSE:1` 为 AGPLv3，`package.xml:8` 为 MIT | 本次总结接口和行为；直接复制实现前核清来源与适用许可，不自行按 MIT 认定 |
| 额外功能改变范围 | ROS2、在线 TTS、RAG、KWS、AEC、PAUSE 推测与实时调度 | 本次只借鉴与 P2/P4 相关的做法；其他能力按原里程碑另行安排 |

## 5. 建议落实顺序与验收边界

| 顺序 | 任务 | 可交付结果 |
|---|---|---|
| 1 | 按 P2-02 前置条件整理正式 LLM adapter，落实 R2/R3 | `llm_probe`、模板/能力约束、生成与取消恢复回归 |
| 2 | 结合现有 TTS 做 LLM → TTS 局部文件联调，落实 R1 | 关联轮次的文本/WAV、首段和完整耗时、取消后旧输出拦截；不等同四模型共驻或实时播放验收 |
| 3 | 满足 P2-01～03 前置条件后执行 P2-04 | `coexist_probe`，实际库加载路径、共驻资源、顺序 NPU 调度；按 A06 完整条件判断 |
| 4 | P2-04 通过后执行 P3-01 | `voice_file_demo`：固定 WAV → ASR → LLM → TTS → WAV，各阶段可追踪 |
| 5 | P1-02/P3-01 条件满足后执行 P4-01 | 接入现有耳机后端、端点与终端 start/stop/cancel，完成真实播放取消与恢复 |

ASR 质量定位和 TTS 试听可按既有任务推进，不能被参考项目替代。P0 模型来源缺项继续跟踪；候选内部试验、M0 关闭和发布验收分别判断，不能因为增加本文而放宽任务依赖、把 candidate 改成 verified 或将未通过项记为完成。

详细实现约定见 [执行计划 §4.5](../EXECUTION_PLAN.md#45-参考项目建议的实施约定)，任务状态和下一步以执行计划为准。验收标准集中维护在 [ACCEPTANCE.md](../docs/ACCEPTANCE.md)，本文不新增性能承诺，也不安排重做用户已确认的蓝牙录放基线或自动续做已停止的长时测试。

## 6. 参考源码索引与快照标识

下表为 Windows 参考目录中的相对位置；建议先看相关函数，再阅读调用方。板端没有这些参考源码的副本。

| 查阅目的 | 参考源码位置 |
|---|---|
| 平台、依赖、默认 SoC | `README.md:67`；`CMakeLists.txt:13` |
| 首分句提示词、实际模型选择 | `config/voicenpu.yaml:64` |
| 分句、清理、流式回调、预热、abort | `src/llm/rkllm_qwen.cpp:26`、`:60`、`:129`、`:250`、`:291` |
| 模型调度、提交同步与轮次失效 | `src/scheduler/voice_scheduler.cpp:179`、`:221`、`:254` |
| ASR/VAD 端点配合 | `src/engine/voice_engine.cpp:16`；`src/asr/zipformer_asr.cpp:46` |
| TTS 抽象与取消边界 | `include/tts/tts_backend.h:11`；`include/tts/melo_tts.h:27` |
| PCM 发布与实际播放队列 | `src/engine/voice_node.cpp:443`；`src/asr/audio_frontend.cpp:28`、`:193` |

关键文件 SHA-256 在 2026-10-21 读取，供本地参考代码变化后确认本次评审对象；这些哈希不是上游真实性、许可或运行效果证明。

| 文件 | SHA-256 |
|---|---|
| `README.md` | `058204350576b87b5a89a21f5e281b18a6dda64b06a73f007589b861f1e6126c` |
| `CMakeLists.txt` | `ff20e271ea08eb6e7c5bd1b8bce29d3a342b3dd54d012ef4b57b3a5bd924ac62` |
| `config/voicenpu.yaml` | `179e0edb133b902532eedefd5bd5d78118afee606f1ff41d80bb26e59ab87120` |
| `src/llm/rkllm_qwen.cpp` | `6f9b9e4eaa61f78954e027ddcbd34b79a6a4d31bf424e5d40c4060460147911d` |
| `src/scheduler/voice_scheduler.cpp` | `ea9939bb308e7b6655a6b400ac45ee528e995d26d0032aefb1854b0589aa34a0` |
| `src/engine/voice_engine.cpp` | `2c5e2539b142a0d79f4625a10b3a040d39ceabe3c057ec8bc1d6f8662776c1ab` |
| `LICENSE` | `6f1e622c82a380075843bb084a7ec3b1f1d12a4a02526d75e78b0924a860aa75` |
| `package.xml` | `d1cee301d7aecfe96225dd32875657318cec61eb72ad223e378b350fad1f7c72` |

## 7. 2026-10-26 标点控制与段内合成适配

补充核对本地参考 `src/tts/melo_tts.cpp:160-225`：内部句末标点软化、按音素预算聚合小片段，限于单次synthesizeStream调用。现工程按用户要求借鉴为 `tts.join_short_sentences`，采用现有有界段和冻结Melo前端不分段逗号别名，保留段尾语气，不移用RKNN音素预算。已完成同速私有音频对照和四模型自动化，语速改为0.9；详细结果与耳机听感范围见[环境§56.20](../docs/environment.md#5620-参考项目标点控制与有界段内短句合成)。本节不改写2026-10-21静态审阅时的工程状态。
