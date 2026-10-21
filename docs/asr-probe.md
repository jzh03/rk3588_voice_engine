# 文件级 VAD / Zipformer 检查

本工具在板端以 C++17 实现真实增量 PCM、Silero CPU VAD 和 RKNN NPU Zipformer。它不是麦克风、蓝牙或完整对话引擎；不初始化 LLM。P2-01、完整 A03 与模型 `candidate` 状态须依实际结果判断，不能由构建成功替代。

## 构建与运行

在 `/home/orangepi/projects/rk3588_voice_engine` 执行。默认 `VOICE_ENGINE_BUILD_ASR_PROBE=OFF`，不开启时不要求私有模型依赖。

```bash
cmake -S . -B build/file-probes -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVOICE_ENGINE_BUILD_ASR_PROBE=ON
cmake --build build/file-probes --target asr_probe vad_window_test asr_metrics_test -j2
ctest --test-dir build/file-probes --output-on-failure \
  -R '^(vad_window|asr_metrics|asr_probe_cli)$'
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  build/file-probes/bin/asr_probe --config config/file-probes.yaml \
  --official --output-dir output/runs --run-id asr-probe-official-NEW \
  --chunk-ms 20 --repeat 5
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  build/file-probes/bin/asr_probe --config config/file-probes.yaml \
  --manifest tests/fixtures/manifest.json --output-dir output/runs \
  --run-id asr-probe-fixtures-NEW --chunk-ms 20 --repeat 3
```

`--official` 与 `--manifest` 互斥；`--sample ID` 可重复，仅选清单内指定样本；`--chunk-ms` 仅 10/20/37，`--repeat` 为 1～20。`--run-id` 为 1～64 个 ASCII 字符，首位字母或数字，后续允许字母、数字、`_ . -`。目录严格新建，拒绝覆盖。固定样本元数据见 [fixture 清单](../tests/fixtures/manifest.json)。可对同一清单分别运行三个 chunk 参数后对照每条 hypothesis。

程序把配置复制为仅 VAD/ASR 启用，再调用共享资产检查。模型角色从校验过的 manifest `roles` 取得，VAD 使用 `model_path`；每个路径必须在该条目 `files` 中且仍位于 manifest 目录内。音频只接受 16 kHz、单声道、PCM16 WAV，逐块读取而非整文件载入。fixture 必须匹配清单 SHA-256、帧数、采样率与声道。

完整模型进程必须在共享独占 flock 内运行；仅另一进程占锁不会授权当前进程。初始化前检查实际映射的 RKNN、sherpa C API、ORT 路径/哈希属于已校验的私有依赖，初始化后查询实际版本。固定 sherpa 发布库使用旧 libstdc++ string ABI，本工程保持原 ABI，以 C++ RAII 封装 C API，避免跨库传 `std::string`；不链接 C++ wrapper、不升级库。

退出码：0 适用检查通过；2 参数、配置、资产或锁错误；4 模型/运行错误；6 适用的质量、行为、reset、一致性或预热 P95 指标失败。只有完整20条主体时应用每批至少18条关键词全对、中文子集 CER ≤15%门槛；子集字段为 null。官方样本要求全文精确相同。可选外部监测器对进程组施加硬超时，本次监测器将超时归为5；底层被信号终止/系统 timeout 的原始退出码仍保留。进程内同步 SDK 调用没有实现可抢占取消。

## 增量与边界契约

`ZipformerAsr::accept/finish/reset/partial/stats` 输入16k单声道归一化 float32。每次 accept 内部再以不超过320样本推进 fbank；有103帧即执行 encoder，每块推进并 `Pop(96)` 回收旧特征。35个encoder缓存按照实际RKNN查询格式回传，对NHWC缓存执行官方NCHW→NHWC转置。decoder保持两个int64 token，blank=0、unk=2；joiner每个encoder输出步最多取一个greedy token。`finish`按固定官方例程补零完成末块，重复finish幂等；reset重建fbank并清空所有模型输入/输出、token、计数与文本。整段PCM不会积累到finish后才推理。

fbank固定80 mel、25ms窗/10ms帧移、povey、低频20/高频-400、dither0、snip_edges=false、预加重0.97、去DC、log power，不乘32768。模型及特征元数据不匹配即拒绝。流最长120秒，文本字节上限使用配置中的 `queues.llm_text_bytes` 作为现有通用文本容量；这是诊断安全界限，不能视为15秒实时发言调度已经接入。

`SileroVad` 聚合20ms或其他运输块为512个新样本，由固定sherpa持有64样本上下文。末尾不足512补零但独立记录有效样本数；Flush后段边界截断到原始有效输入。VAD初值threshold固定0.5；min_speech、end_silence、max_speech和start_timeout读取Config；初始等待超时后不再判后续输入为语音。sherpa的max duration本身只是提高阈值，wrapper另在512样本边界强制Flush/Reset并累计 `max_duration_endpoints`。段起点转回整个输入时间线。

诊断同时把原始PCM增量交给ASR与VAD，VAD未确认语音则发布明确empty，并保留 `asr_unfiltered_hypothesis` 便于识别静音幻觉。`pre_roll_ms`作用于报告的建议预录起点；ASR已经接收全部PCM，不裁剪前导内容。这不是按VAD门控NPU或主引擎发言结束调度的实现。长文件中VAD多个端点仍对应一个文件ASR final；实时分轮、取消、设备恢复及完整预录队列接入待后续工作。

`VadWindowAssembler`一次accept最多16000样本、剩余缓存始终<512；流120秒、段元数据8192条，sherpa循环缓冲按max_speech+pre_roll+end_silence+2秒设定并及时取出已完成段。invalid/NaN/越界PCM拒绝。失败后调用reset开始新流。

## 指标口径

`metadata.json`含配置/两份资产清单/fixture清单/源码哈希、实际库路径与哈希、运行时tensor布局、模型声明版本与实际版本、初始化时间。`results.jsonl`逐样本记录冻结reference、hypothesis、关键词命中、字符编辑距离/CER、原WAV时长RTF、EOF→final耗时、分块、finish前已运行块数、尾部补零、VAD段、reset空新流及重复一致性。`summary.json`按批汇总；运行失败仍保留已完成样本。

参考归一化由fixture的NFKC/casefold/去标点空白冻结；hypothesis实现覆盖当前fixture字符集的ASCII大小写/全角ASCII转换及中英文标点空白去除，并非完整通用Unicode NFKC。CER以UTF-8解码后的Unicode码点计算，中文主体子集排除code_switch及boundary。数字读法参考不得根据识别结果改写。首批只做冷/初次流统计，后续主体批次用于预热P95，边界不混入主体P95。

EOF→final不等于声学末语音→final；RTF包含流式fbank、ASR和VAD调用、WAV读取及轻量采样，分母只用原始WAV时长、不含补零。`ru_maxrss`是进程生涯峰值RSS；PSS、MemAvailable、SwapFree是采样值，均非完整NPU/系统内存增量。本次外层runner每50ms采样，1.5GiB MemAvailable底线和每case180秒保护只终止自己创建的进程组。

## 固定构建来源

缓存转置和greedy search参考RKNN Model Zoo固定提交 `bad6c7334531becaf90a561988519b7bec34d0ab` 的 `examples/zipformer/cpp/rknpu2/zipformer.cc`。原版权/Apache-2.0 notice保留于源码；许可证见 [LICENSE-Rockchip.txt](../src/asr/LICENSE-Rockchip.txt)。第三方fbank源码没有复制到公共src。

私有依赖基准：`deps/rknn-2.3.2`，`deps/model-prep/speech/sherpa-onnx-v1.12.14-linux-aarch64-shared-cpu`，ORT 1.17.1-patched；来源记录见 [ASR准备](model-prep-asr.md)、[VAD/TTS准备](model-prep-speech.md)。fbank基目录为 `deps/model-prep/asr/upstream/3rdparty/kaldi_native_fbank/`：

| 构建输入 | SHA-256 |
|---|---|
| `Linux/aarch64/libkaldi-native-fbank-core.a` | `f786c4672bc89bb0e1e3a55ca62031e73587ab5a46d8e7b8192df210011882f6` |
| `include/kaldi-native-fbank/csrc/online-feature.h` | `3c9fb7b2ffabbee9756d11199901dadcce549637bc8b8de0128751002720d2ae` |
| `include/kaldi-native-fbank/csrc/feature-window.h` | `74eac85c879f9c8bf3fdb3f028b7d54b4f647bfa46519c418222a3f821762387` |
| `include/kaldi-native-fbank/csrc/feature-fbank.h` | `4be90549f3df404c71cab527fa9ef7fcc3ec14883c33235dfddde234be36a8ce` |

固定fbank静态库非PIC，最终exe显式 `-no-pie`；未重编译或替换该依赖。

## 本次验证

验证日期2026-10-20，Asia/Shanghai。无模型VAD聚帧/reset、UTF-8编辑距离/归一化与CLI拒绝测试3项通过。正式验证证据根目录为 `output/runs/asr-probe-validation-20261020-212111/`，含完整命令、二进制哈希、50ms资源采样、实际退出码和当时源码/配置/清单快照；`comparison.json` 汇总跨分块、边界与失败样本。未触发内存/超时保护。

| 验证 | 证据run（均在output/runs/） | 实际结果 |
|---|---|---|
| 官方连续5次、20ms | `asr-probe-official5-20261020-212111` | 全部全文精确相同；exit0；预热4次P95 RTF 0.10975 |
| 固定26条×3批、20ms | `asr-probe-fixed20ms-20261020-212111` | 每批主体关键词13/20；中文CER 34/213=15.9624%；exit6；预热40条主体P95 RTF 0.20705 |
| 固定26条、10ms | `asr-probe-fixed10ms-20261020-212111` | exit6；与20ms第一批26/26 final完全相同 |
| 固定26条、37ms | `asr-probe-fixed37ms-20261020-212111` | exit6；与20ms第一批26/26 final完全相同 |

三种分块的原始ASR文本、发布文本与VAD段/状态全部一致；三批repeat、finish幂等及reset后的空新流均通过。所有正常长度语音在finish前已经执行NPU块，特征缓存实测峰值105帧。固定模型仍为candidate，**本轮A03未通过**：关键词未到18/20，中文CER超过15%，尾部测试的完整参考亦不符。不改参考、不降阈值，不为合成集调参。

首批7个主体关键词失败（其他两批相同）：

| 样本 | 类别 | 参考与主要错误 |
|---|---|---|
| `zh_01` | chinese_common | “请把客厅…”识别为“行吧课听…” |
| `zh_04` | chinese_common | “请先保存文件…”识别含“你先保存问件 SIL…” |
| `zh_07` | chinese_common | “这段语音只用于离线功能测试。”仅输出“渔民现功能测试” |
| `zh_08` | chinese_common | “会议结束以后，请把椅子摆整齐。”输出“你把椅子摆整齐” |
| `num_01` | numbers_dates | “今天是二零二六年十月二十日。”末尾缺“日” |
| `mix_02` | code_switch | “请播放 music，然后调低音量。”输出“洗播放明仔 SIL然后掉第一辆” |
| `mix_03` | code_switch | “本次 test 已经完成，请保存结果。”输出“已经完成英保存结果” |

边界：empty、3秒静音、80ms短音频、固定低幅噪声均返回empty且VAD无语音。15秒文件完整消费240000样本，finish前15个encoder块，关键词命中；内容仍有识别错误，不代表整段全文正确。尾部文件输出“行吧客厅的窗户打开让新鲜空气进来”，末尾“让新鲜空气进来”存在、关键词全对，但开头“请把”错误，严格完整参考检查失败。

默认硬max端点与start_timeout计数均为0：15秒文件的实际语音约13秒，3秒静音也未达到默认5秒等待开口，因此本批没有覆盖这两条触发路径。跨chunk一致不代表自然语音端点精度通过。

4个正式进程的最大wait4 RSS为196948 KiB（约192.33 MiB），采样PSS峰值193789 KiB，最低MemAvailable为6470048 KiB；没有把这些数值称为全部NPU内存。fixture为同源Melo合成且未人工试听，参考是冻结的预期读法，不能据此声称自然/噪声/真实录音准确率。


针对明显丢开头的 `zh_07` 与 `zh_01`，随后用已编译固定官方demo（先整WAV送fbank）复查，未修改输入、参考或模型。官方输出分别为“渔民现功能测试”“行吧课听的窗户打开让新鲜空气进来”，与本工程增量raw输出逐字相同，两次退出0。证据为 `output/runs/asr-probe-native-compare-20261020-212111/comparison.json` 及对应log/metrics。这两例未观察到增量改造相对官方实现的文本回归；模型与合成输入的质量仍需诊断，不能外推全部失败均已定位。

正式性能批次后仅加固fixture预检错误分类：缺失JSON、非法路径及非16k PCM16 WAV头在模型初始化前退出2；新增无模型负例。推理、特征、VAD算法未因质量结果修改，正式批次源码/二进制哈希以冻结快照为准；最终 `build/model-probes` 同时开启ASR/TTS构建通过，15/15非音频CTest通过；`asr_probe_cli`含这3个负例，耗时6.75秒，VAD聚帧与ASR指标测试亦通过。整合证据为 `output/runs/file-probes-merge-20261020-205226/combined-checks.json`、`combined-check-2.log`。未为这类预检修复重复完整性能批次。
