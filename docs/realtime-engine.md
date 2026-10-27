# 实时主引擎

本入口对应P4实时主引擎：在板端`voice_engine`中连接真实PulseAudio后端、增量VAD/ASR、常驻LLM和流式TTS，支持终端开始/结束录音、取消和清空会话。P4/M1已按用户确认范围结项；既有自动化、实机及限制见[结项记录](environment.md#5633-p4按确认范围结项)，软件实现索引见[交付摘要](../tests/fixtures/realtime-engine-implementation.json)。

## 构建与配置

在板端 `/home/orangepi/projects/rk3588_voice_engine` 执行，沿用已有 C++17/CMake/Ninja 与固定模型/SDK：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake -S . -B build/model-probes -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DVOICE_ENGINE_BUILD_ASR_PROBE=ON \
  -DVOICE_ENGINE_BUILD_LLM_PROBE=ON -DVOICE_ENGINE_BUILD_TTS_PROBE=ON
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake --build build/model-probes -j2
```

三个模型开关及真实 Pulse 后端均可用时，构建的 `voice_engine` 接通实时入口。默认构建继续支持配置检查和 `--mock --control stdin`，不引入私有模型依赖。构建、CTest、资产大文件校验和模型运行共用上述锁；运行中的引擎持锁期间不并行编译或做其他模型测量。

公开模板为 [config/realtime.yaml](../config/realtime.yaml)，设备名留空；本机配置使用被 Git 忽略的 `config/realtime-local.yaml`。本轮从原 `config/local.yaml` 复用完整 audio 分组生成本机实时配置，原文件保持不变。配置一次加载，不与其他 YAML 合并。未来换设备时只在本机配置中填写实际 source/sink/profile，禁止 monitor 和自动回退其他声卡。

实时入口要求 VAD、ASR、LLM、TTS 全部启用，KWS、wake、barge-in 关闭。模型 ID 与清单保持既有组合；TTS 使用已验证的 4 线程，正常输出预算 128 tokens。其余字段、单位和通用范围见 [默认配置](../config/default.yaml)。文件探针的 `file-only` 配置不能用于实时入口。

实时模式将配置额度分配到串联队列，采集总额至少 2 帧（40 ms），播放总额至少 3 帧（60 ms）；不改变通用配置解析器及 mock 的原范围。默认采集 100 帧分为 Pulse 99 帧、模型输入 1 帧；默认播放 100 帧分为 TTS 49 帧、Pulse 50 帧、控制器暂存 1 帧，均按内部 20 ms 帧计算。活动 SDK 的单段临时输出另受 `max_tts_audio_ms` 限制，音频服务和耳机自身缓冲不计作应用队列。小于实时最小值的配置在加载模型前拒绝。

## 运行与命令

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  build/model-probes/bin/voice_engine \
  --config config/realtime-local.yaml --check-config

flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  build/model-probes/bin/voice_engine \
  --config config/realtime-local.yaml --control stdin \
  --run-id p4-interactive-001 --output-dir output/runs
```

真实运行需要持有共享锁；`--check-config` 只检查配置和启用资产，不打开设备或模型。运行目录必须新建，重复使用同一 run ID 会失败。启动先显示 `ENGINE_STARTING`，模型初始化完成后显示 `ENGINE_READY` 及状态。设备未就绪时处于 `DEVICE_WAIT`，只匹配配置中的设备，不自动改动系统 profile 或默认路由。

板端现已将已配对的 vivo TWS 4 设为可信设备，并开启 orangepi 用户音频服务的开机常驻；由既有 BlueZ/PipeWire 策略自动连接，服务启动自动回连实测约3.2 s，见[环境§56.14](environment.md#5614-板端耳机自动连接设置)。连接恢复后仍按主引擎状态提示显式输入 `start` 开始一轮；整板冷启动及回盒再取出后的新配置实测另行记录。启动蜂鸣的隔离、恢复及验证边界见[环境§56.13](environment.md#5613-启动蜂鸣的隔离与恢复)。

| 命令 | 实时行为 |
|---|---|
| `start` | 开始新一轮；忙时取消旧回答并保留一个待开始意图，等待旧模型结束；取消等待期间先缓存新语音，缓存溢出明确报告需要重说 |
| `stop` | 结束本轮采集，消费已采集的尾帧并收尾 ASR；自动端点仍可先结束一轮 |
| `cancel` | 使旧 generation 失效，清空旧文本/PCM，取消模型并 flush 播放，确认模型结束及设备清空后回到可触发状态 |
| `new-session` | 取消旧轮次并清空应用会话历史 |
| `status` / `help` | 查看状态、session/generation 或命令 |
| `quit` / EOF | 受控取消后退出；SIGINT/SIGTERM 也走相同收尾边界 |

主状态为 `STARTING → IDLE → LISTENING → FINALIZING → RESPONDING → IDLE`，异常分支包括 `CANCELLING`、`DEVICE_WAIT`、`ERROR` 和 `STOPPING`。ASR 与 LLM 串行使用 NPU，TTS 与本轮 LLM 可以流水执行。正常播放使用 drain；取消使用 flush。仅当前 generation 的非空回答在完整正常播报后提交会话历史；空输入、取消、失败或断连均不提交。

无声等待、最长发言、端点及模型超时沿用共享配置。MemAvailable 低于 1.5 GiB 时拒绝新轮次，连续 3 s 低于门槛时取消活动轮次；读不到指标时按不可用处理。断连或 profile 变化取消旧轮次、丢弃旧缓存；恢复后等待新的显式 `start`。

当前实时TTS使用独立generation取消令牌，在固定Sherpa/ORT组合上把取消传给ONNX Runtime的RunOptionsSetTerminate；仅在原生调用实际返回且送音线程结束后才确认idle。具体ABI边界、旧回调兼容和协作式取消限制见[TTS取消契约](tts-probe.md#原生取消与固定abi边界)。同步ASR及不响应中止的原生算子仍受原监督期限保护：2 s内无法确认全部模型空闲则记录`restart_required`并退出`5`，不会假报idle或析构仍在推理的句柄；此时需要显式重启。开发诊断使用外部timeout时，应采用`flock ... timeout --foreground -k 5s <时长> engine ...`，让计时器直接监督引擎，flock祖先在清理期间保持存活。将timeout放在flock外可能先结束锁进程并遗留引擎；本次人工入口已修正为上述顺序，外层1800 s不改变引擎内部2 s取消期限。

实时入口退出码：`0` 正常退出；`1` 控制/日志/音频接口错误；`2` CLI、配置、资产或锁错误；`4` 模型/运行错误或启动内存不足；`5` 取消后无法在期限内确认安全状态、必须重启；正常处理信号时为 `128+signal`，若同期模型取消超时则以 `5` 为准。设备暂不可用进入 `DEVICE_WAIT` 等待，未自动视为通过或强制映射为诊断工具的退出码 `3`。

## 播放保活与可选前置静音

`audio.playback_keepalive` 通用缺省false；当前实时模板与本机实时配置为true，`audio.playback_lead_in_ms` 为0。引擎在加载模型前创建音频后端，独立的零PCM播放流从设备就绪起维持指定sink运行，贯穿IDLE、采集、模型推理和回答间隔。它不采集麦克风，不修改profile、默认路由或音量；持续占用播放通道会增加耳机耗电，只在应用运行期间启用。

流的暂停、清空与排空沿用[PulseAudio 官方流接口](https://freedesktop.org/software/pulseaudio/doxygen/streams.html)。保活流与语音流分别管理：取消/新会话仍使旧语音generation失效并cork/flush语音流，正常回答仍独立drain并提交历史；保活不参与语音帧计数、队列容量或会话历史。空轮次也不会因此生成语音。保活采用固定4 KiB零缓冲，按可写空间有界供给；独立服务缓冲请求最大100 ms（16 kHz单声道float32为6400 B），实际属性记录于状态。capture-only不会创建保活流。退出释放所有流；失联释放旧流，重连只恢复保活、等待新的显式start，不重播旧回答。

`playback_keepalive_started` 表示开始向服务提交零PCM；`playback_keepalive_active` 表示解除cork已确认且服务报告未挂起，不能证明耳机声学起声已就绪。状态/资源采样分别记录 `playback_keepalive_active`、`playback_keepalive_native_frames`、`playback_keepalive_underruns`；保活欠载与语音 `playback_underruns` 分开记录，不能用静音掩盖TTS供音不足。`audio_probe` 的 `run.json` 同时记录保活开关和前置静音时长。

此前1 s前置静音文件对照完整清楚，但500 ms后端方案实听仍丢开头，随后改为持续保活。当前固定音频的首次及空闲12 s后第二遍播放均获用户确认：开头完整、整句清楚，本次中间也没有可闻停顿，见[环境§56.9](environment.md#569-保活固定音频双轮试听通过)。该试听使用预先生成的同一WAV，实时ASR/LLM/TTS分段问答下一步复验；既有中段欠载记录保留。

`audio.playback_lead_in_ms` 仍作为可选独立参数保留，通用缺省0，范围0～2000 ms且为20 ms整数倍；启用时每个非空轮次增加原生采样率静音。它使用原语音流和固定1 KiB零缓冲，取消/失联会清掉尚未提交的前缀，空轮次不补前缀。若同时启用保活与前置静音，两者均生效；当前候选明确为true/0。所有实际等待均计入A10，不从声学延迟中扣除。`playback_lead_in_started` / `playback_lead_in_queued` 和 `playback_started` 均为软件提交事件，不能代替外部声学起声。

## 分段合成与起播储备

保活固定音频试听已通过，随后实时问答仍出现合成供音不足导致的中段欠载。当前实时配置按用户要求启用 `tts.prebuffer_ms: 1500`：当前关闭first_segment覆盖，所有段统一min/target/lookahead=12/12/2，长数字/日期/英文词仍可延伸至40字符硬上限；常驻合成线程与PCM送音线程分离，原生语音最多两个槽位，首段较短时先积累真实语音再起播。达到目标、两个已生成槽位满或合成结束即可放行；单句/空EOS可完成，不添加数字静音或放大既有2 s应用PCM队列。内存边界、取消行为和统计字段见[TTS worker契约](tts-probe.md#有界-tts-worker-与提交边界)。

首段生成后到实际放行之间可能增加等待，按实际值计入A10；后台始终保留已验证的耳机保活。此前均匀12字、1000 ms储备方案曾完成软件复测及一轮完整耳机问答：用户确认启动无蜂鸣、开头完整、段间无明显停顿、等待可接受；本轮语音欠载0，模型初始化期间独立保活欠载1次单列，见[环境§56.15](environment.md#5615-当前候选完整耳机问答试听通过)。该次为前置诊断；正式首组五轮后来完成但有顿挫，后续见下文。完整正式20轮及声学延迟分布尚未验证；候选选择、延迟及保留的失败见[环境§56.12](environment.md#5612-一秒储备下的固定文字分段对照)。此前储备调节和掉电恢复记录见[§56.10](environment.md#5610-实时中段欠载与有界tts原生预取)、[§56.11](environment.md#5611-按用户要求缩短为1秒起播储备)。

当前参数正式20轮已在`output/runs/p4-02-normal20-085-20261027-01/`完成执行，原题不变、全程由用户手动start。20/20流程完成、19/20听感确认，语音欠载0；第11轮字音略模糊，另有1次回答前静音保活欠载，故整批未全部通过。原私有控制台仅连接该测试进程，当前已正常quit，不再作为活动入口；普通交互仍使用上方受哈希检查的手动启动脚本。逐轮数据、人工反馈及正式验收剩余项见[环境§56.28](environment.md#5628-085候选正式20轮人工交互批次)。

## 日志与测量范围

每个 run 使用 `mode=realtime` 的轮转 JSONL 日志，继承 run/session/generation/seq 关联字段。记录状态、端点、识别字节数、PCM 帧提交、正常 drain、取消、错误及资源采样，不保存原始麦克风样本、完整转写或完整问答。

`asr_final` 记录输入样本数、收尾原因和完成时刻；`playback_submit` 记录当前 generation 的 PCM 帧序号与提交时刻；`playback_drain_requested` 保存 LLM/TTS 阶段时间和队列峰值；`turn_completed` 记录正常完成及历史是否写入。取消记录请求、停止软件 PCM 提交、模型完成与 flush 确认的观测时间。

这些时间是软件观测。同步 ASR 收尾后的端点回推值须保留估算标签；软件提交和服务 drain/flush 都不能替代蓝牙耳机实际起声或尾音。时间口径继续遵循[测量规则](ACCEPTANCE.md#4-时间点口径与统计)；本次结项保留软件数据，外部声学未测且已移出当前执行范围。

此前A/B选择0.95，后续曾设0.9，当前按用户新要求设为0.85（通用默认仍1.0）；起播储备改为1500 ms，独立保活与0 ms前置静音保持。原12字候选的正式第1～5轮流程均完成，语音/保活欠载均0，但用户反馈句间顿挫，历史单阶段标点前瞻实验随后出现欠载，当时保持禁用；已按用户要求停止试参，转为基于[GitHub成熟方案](environment.md#5618-github流式tts方案调研与后续依据)设计。速度及前瞻均记录在资源/drain事件中；旧五轮与新候选分别留档，见[环境§56.16](environment.md#5616-正式首组五轮与语速选择)和[§56.17](environment.md#5617-同速标点前瞻分段验证)。

用户随后选择先实施首段/后段分句策略，新增 `tts.sentence_mode`。候选保持0.95语速和1000 ms储备；真实TTS六轮出现2次欠载，故正常配置仍为sentence_mode=false，供音回归通过后才切换。新策略不会修改silence_scale、模型或播放余量调度；测试、启用状态和听感范围见[环境§56.19](environment.md#5619-首段快出与后续完整句分段)。

当前启用 `tts.join_short_sentences=true`，将已有有界段内的中文小句合成连续语音；当前分段已回退统一12/12/2，40字上限与sentence_mode=false保持，储备按新要求改为1.5秒。资源/drain新增 `tts_join_short_sentences`，同速对照及四模型结果见[环境§56.20](environment.md#5620-参考项目标点控制与有界段内短句合成)。旧0.95试听及旧正式五轮不作为本候选听感确认。

当前程序仍可在VS Code板端终端运行 `bash output/runs/p4-physical-fix-20261027-01/launch-manual.sh`。这是板端私有脚本，核对冻结程序和配置哈希，外部人工交互保护最长1800 s；等待ENGINE_READY/IDLE后由用户输入start，看到LISTENING后立即说话，聊天提示不会自动由耳机播出。P4实机取消和三状态传输恢复已完成，范围内结项及限制见[§56.33](environment.md#5633-p4按确认范围结项)。用于内容排查的私有文字诊断入口已结束并禁止直接重复启动；日常入口保持原默认日志。旧候选脚本及各批证据保留各自二进制身份。

2026-10-27手动有效问答已复现用户报告的明显顿挫：同一交付程序/配置下合成6段，语音播放欠载1次，保活欠载0；最大软件送帧空档约1.67 s，不能当作耳机可闻停顿时长。该历史候选连续性验证失败，正式批次未开始；当时保持0.9语速与1秒储备做逐段定位。详情见[环境§56.22](environment.md#5622-新候选实机顿挫与播放欠载复现)。

## 验证入口与边界

无模型控制器回归注入音频和推理替身，检查状态、generation、首尾帧、两重取消屏障、队列边界、故障恢复和会话提交；默认构建也能执行。真实四模型的自动验证通过隔离的 PipeWire/Pulse 服务，将冻结自然语音实时送入非 monitor 管道 source，输出到私有 null sink；它验证生产音频路径和模型调度，不使用或改变用户音频服务。

P4-01当时的真实模型自动验证曾出现同步TTS取消2 s超时和6次播放欠载，该历史失败保留。后续原生取消及故障恢复已定向修复/复验，P4最终按用户确认范围结项，详见[§56.30](environment.md#5630-tts取消与故障恢复送音定向修复)和[§56.33](environment.md#5633-p4按确认范围结项)；不据此重写旧失败或声称所有播放连续性问题已修复。

P4-01自动化交付时没有蓝牙耳机试听、真实断连操作、外部尾音测量或 A10 的 20 轮/60 次批测。用户当时选择先完成自动化验证；P4-02后续前置静音失败、持续保活候选及实际结果以环境 §56 为准。历史模拟音频偶发超时及本轮所有失败保留原始报告；测试结果和代码身份见环境记录，不从单次通过推断历史问题已修复。


## 逐段合成精确计时

完成播报供音时，`playback_drain_requested.data`新增`tts_segment_timing_schema=1`、`tts_segment_timings`和`tts_segment_timings_dropped`。每段记录`segment_seq`（从0开始）、文字字节数及原生采样数/采样率，不记录私有文字或PCM。阶段时间均为steady_clock单调纳秒：

- `text_selected_ns`：TTS worker选择出该段的时间；不等同于LLM原始文字到达时间。
- `native_wait_begin_ns/native_slot_acquired_ns`：原生槽等待起止；`generate_begin_ns/generate_end_ns`精确包围同步generate回调。`sdk_generate_seconds`是该回调内部SDK调用计时，不含外层检查/音频复制。
- `native_enqueued_ns/publish_begin_ns/resample_end_ns`：原生音频交给发布路径、发布/重采样开始与完成；无预缓冲时前者表示直接交给发布函数。
- `first_pcm_enqueued_ns/last_pcm_enqueued_ns/publish_end_ns`及`pcm_queue_wait_ns`：PCM入队边界、该段发布完成与累计PCM队列等待。
- `first_pcm_seq/last_pcm_seq/pcm_frames`可与已有`playback_submit`按session、generation和帧seq关联，取得真正提交给播放后端的时间。区间覆盖本段converter.push输出；其中首帧可能携带上一段不足20 ms的尾部，最终EOS冲刷帧不归入某个段。
- `input_queued_bytes/pcm_frames_before_generate/native_samples_before_generate`：进入生成前的应用队列观测，不包含音频服务或耳机内部储备。

每轮最多保留128条记录，超出只增加丢弃计数，不改变供音、不阻塞等待、不无限增大存储。正常完成时才将明细统一写入日志，避免在同步生成或PCM热路径做文件I/O；实时资源采样不复制该明细。未正常完成的轮次不能据缺少明细认定未发生生成。旧generation与取消后的明细不会混入新轮次。以上是软件计时，不能替代耳机实际首音、尾音或静音时长测量。

§56.23诊断当时仅补计时，0.9语速、1000 ms储备、12字软目标、标点合成策略、模型/线程参数和音频路由均未改；验证及同参数实机复测见[环境§56.23](environment.md#5623-逐段精确计时与同参数实机复测)。

可选动态模式配置与切换规则集中在[README配置说明](../README.md#配置检查)。逐段计时另记录text_chars及实际min_chars/target_chars/boundary_lookahead_chars；首块输出后切换，每轮重新开始，不改变sentence_mode=false。首块文字长度并不保证固定音频时长，首块更短也不能据此承诺消除首段衔接欠载。

动态模式固定回放已验证策略切换与文本完整性，但6轮中出现4次语音欠载（同组原12字分段为0）；该动态配置现已按用户新要求停用；新统一分段/0.85/1500 ms组合已完成六轮隔离音频自动化和一轮耳机诊断，语音/保活欠载均0、单轮听感获用户接受，该段所述为正式批次前的历史诊断；后续20轮流程已完成，19/20听感及范围内结项见§56.33；动态失败保留于[环境§56.24](environment.md#5624-用户指定的首块后续块动态分段)，当前试听范围见[环境§56.27](environment.md#5627-085语速与15秒储备的单轮耳机试听通过)。

## 固定参数自动化验收（2026-10-27）

前一批§56.29在当时不修改程序和参数的条件下完成7/7相关CTest、A09的10类故障控制/后续流程、5次取消（软件停止5/5、模型时限3/5）、私有音频重连3/3、软件延迟20次和进程冷启动3次；原TTS两次退出5及恢复轮欠载保持失败记录。原始证据及诊断接入范围见[环境§56.29](environment.md#5629-p4固定参数自动化验证)。

前一批ASR定稿→首PCM软件P95为5.011 s，仍属于该批旧程序与输入；修复后没有重跑完整延迟分布。后续实机取消/三状态传输恢复已完成，声学首音/尾音因条件不具备保持未测，供音/第11题发音跟进由用户删除；P4按确认范围结项，不把这些限制改为原完整A08/A10数值通过，见[§56.33](environment.md#5633-p4按确认范围结项)。

### 音频服务与定向修复（2026-10-27）

音频线程改用Pulse的prepare/poll/dispatch等待：上限仍为5000微秒，服务I/O、定时器或应用命令可立即唤醒；poll期间释放应用互斥锁，回调与流销毁仍由同一线程持锁执行，跨线程只调用官方允许的wakeup。采集积压每次转换最多20 ms原生输入，保留剩余队列并在采集处理前后服务语音和保活；正常stop跨循环消费缓存和DSP尾部，cancel则丢弃。应用额度、服务buffer请求、重连退避、1.5 s起播储备均未扩大。接口依据：[Pulse主循环](https://www.freedesktop.org/software/pulseaudio/doxygen/mainloop_8h.html)。

状态、resource_sample和audio_probe报告新增`output_service_max_gap_ns`、`capture_service_max_work_ns`；欠载事件附带当时服务间隔、播放排队帧数和原生采集积压。这些是软件服务观测，不是声学空隙或硬实时保证。原欠载计数条件保留，未屏蔽故障。

本轮6/6相关CTest、两次真实ONNX合成中取消与一次采集溢出恢复通过，详见[环境§56.30](environment.md#5630-tts取消与故障恢复送音定向修复)和[版本化定向复核](../tests/fixtures/p4-targeted-fix-review.json)。历史欠载缺少调度追踪，不能把本轮成功等同于证明所有旧耳机事件同因；后续物理路径复核及按用户确认范围结项结果见[§56.33](environment.md#5633-p4按确认范围结项)。
