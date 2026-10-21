# CPU VAD / TTS 模型准备与最小验证

当前结论（2026-10-21）：P0/M0 最小基线已验证并冻结；下列准备阶段及历史 candidate/缺项结论保留当时语义，最新范围见本文末尾收尾记录。

本次日期为 **2026-10-20，Asia/Shanghai**。运行标识 `model-prep-speech-20261020-201140`，证据目录为 `output/runs/model-prep-speech-20261020-201140/`。在 Orange Pi 5 的 `/home/orangepi/projects/rk3588_voice_engine` 经 SSH 完成下载、编译小型检查程序与验证；没有修改系统运行库、NPU SDK、音频服务或 Windows 参考副本。模型仍标 `candidate`。

## 固定组合及来源

| 组件 | 固定来源 / 标识 | 本机路径 |
|---|---|---|
| sherpa-onnx | 官方 `v1.12.14`，commit `26aa2fa93210376a89de3a65a1a4dd320c37f5e9`；CPU ARM64 shared 发布资产 ID `294564096` | `deps/model-prep/speech/sherpa-onnx-v1.12.14-linux-aarch64-shared-cpu/` |
| C/C++ API 头文件 | 上述精确 commit 的 `c-api.h`、`cxx-api.h`；二进制发布包未包含头文件 | `deps/model-prep/speech/include/sherpa-onnx/c-api/` |
| ONNX Runtime | `1.17.1-patched`，`GIT_COMMIT_ID=8f5c79cb63f09ef1302e85081093a3fe4da1bc7d`；sherpa 固定 CMake 选择的 ARM64 glibc2.17 包 | `deps/model-prep/speech/onnxruntime/onnxruntime-linux-aarch64-glibc2_17-Release-1.17.1-patched/` |
| Silero VAD | 上游 `v5.0` commit `5cd2ba54db059f961e7545d4f211b44f005a6dfb`；sherpa `asr-models` 资产 ID `176977129` | `models/vad/silero-v5/silero_vad_v5.onnx` |
| MeloTTS | sherpa `tts-models` 资产 ID `203769960`，2024-11-03 发布包；模型元数据 `melo-vits version=2` | `models/tts/vits-melo-tts-zh_en/` |

原始来源：

- [sherpa v1.12.14 官方发布](https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.12.14)、[固定 ARM64 构建流程](https://github.com/k2-fsa/sherpa-onnx/blob/26aa2fa93210376a89de3a65a1a4dd320c37f5e9/.github/workflows/aarch64-linux-gnu-shared.yaml)。
- [固定 ORT CMake 来源](https://github.com/k2-fsa/sherpa-onnx/blob/26aa2fa93210376a89de3a65a1a4dd320c37f5e9/cmake/onnxruntime-linux-aarch64.cmake)。
- [Silero v5.0 原始模型](https://github.com/snakers4/silero-vad/blob/5cd2ba54db059f961e7545d4f211b44f005a6dfb/files/silero_vad.onnx)、[sherpa VAD 模型说明](https://k2-fsa.github.io/sherpa/onnx/vad/silero-vad.html)。
- [完整 TTS 包](https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-melo-tts-zh_en.tar.bz2)、[TTS 官方说明](https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/vits.html)。

发布资产 tag 本身不是不可变内容锁，因此另存 GitHub API 资产 ID、更新时间、下载字节数及 SHA-256。VAD 下载模型与上游 v5.0 文件逐字节 SHA-256 相同。TTS 包未提供原始权重 commit 或此次转换所用 exporter commit/工具版本；不能把所选 sherpa runtime 版本当作模型导出版本，清单如实记为上游未提供。未自行转换或量化。

| 下载文件 | 字节数 | SHA-256 |
|---|---:|---|
| sherpa ARM64 shared CPU `.tar.bz2` | 23484407 | `ad3e41f19899754735a074938db955a6818153b305fd2959bffc33405976e180` |
| ORT patched `.zip` | 5073164 | `6e0e68985f8dd1f643e5a4dbe7cd54c9e176a0cc62249c6bee0699b87fc6d4fb` |
| VAD ONNX | 2313101 | `6b99cbfd39246b6706f98ec13c7c50c6b299181f2474fa05cbc8046acc274396` |
| TTS 完整 `.tar.bz2` | 167006755 | `e58351ed7149f290a54534538badd4077cdbe6fddc964b24d0bee870415d1514` |

sherpa 为 Apache-2.0；Silero、MeloTTS 和 ORT 为 MIT，原始许可证均保留。ORT 包同时保留 `ThirdPartyNotices.txt`。TTS 包保留 `tokens.txt`、`lexicon.txt`、全部 `dict/`、`date.fst`、`number.fst`、`phone.fst`、`new_heteronym.fst`、README 与 LICENSE。

**包内 `model.int8.onnx` 是 133 字节 Git LFS 指针，不是 ONNX 权重。** 指针声明实际对象为 53517430 字节、SHA-256 `f085f5079e05f039b800aeb542f5253c26a303211b0c6465d0d9387977855a63`；该对象未下载，也未验证。保留原始指针作为包来源证据，清单角色标为 `upstream_lfs_pointer_not_loadable`。本次可加载候选是完整 FP32 `model.onnx`（170429550 字节）。

## 兼容性与加载证据

实机为 Ubuntu 22.04.5 / aarch64 / glibc 2.35。官方固定 workflow 的 CPU job 使用 `ubuntu-22.04-arm`。三个共享库实际 ELF GLIBC 要求最高为 2.17，`ldd -r` 未发现缺失库或未解析符号。C++17 检查程序使用精确配套头文件链接成功，运行输出：`sherpa=1.12.14 git=26aa2fa9 ort=1.17.1`。

- 实际 ORT：`deps/model-prep/speech/sherpa-onnx-v1.12.14-linux-aarch64-shared-cpu/lib/libonnxruntime.so`，13056112 字节，SHA-256 `89abc6da8d7ac84e5270349fe8a44ee7469d9a958797eed004f1a3f5c7df21d4`。与独立配套 ORT 包中的库一致。
- 同目录 `libsherpa-onnx-c-api.so` / `libsherpa-onnx-cxx-api.so` 已通过解析检查。官方 CLI 内嵌 sherpa 实现，只动态加载私有 ORT；不要把 CLI 成功等同于工程已链接 C++ API。
- 包内 RPATH 为相对库位置；未设置全局 `LD_LIBRARY_PATH`、安装全局 Python 环境或复制文件到 `/usr/lib`。
- glibc、libstdc++、libgcc 等操作系统 ABI 依赖沿用 Jammy 系统库，实际路径和哈希记录于 `runtime-loading.json`。模型运行库、ORT、头文件和许可证均在项目私有目录；未捆绑或替换系统 glibc。
- `*.metrics.json` 的 `/proc/<pid>/maps` 采样保存了实际进程加载路径；`*.ldd.txt`、`*.versions.txt`、`*.dynamic.txt` 保存静态解析证据。

## VAD 输入与结果

上游 Silero v5 ONNX 支持 8/16 kHz；**本次固定 sherpa 实现和官方 CLI 限定 16 kHz**，8 kHz 未验证。输入为单声道、归一化 float32 PCM（约 [-1,1]）。原始 ONNX 输入名为 `input`、`state`、`sr`，类型分别 float32、float32、int64；实测张量声明见 `vad-onnx-info.json`。

固定 sherpa VAD 每步推进 **512 样本 / 32 ms**，内部保留 64 样本上下文，向模型提交 `[1,576]`，状态为 `[2,1,128]` float32，初始为零，随后将 `stateN` 反馈至下一步；新流应 Reset，末尾应 Flush。实际音频采样率值为 16000。ONNX 的 `sr` 声明是 int64 标量，sherpa 当前实现传递单元素张量并已在本组合运行。工程 20 ms / 320 样本帧需要独立缓存聚帧，不能据此改写模型窗口。尚未实现工程适配器。

验证配置：CPU 1 线程，threshold=0.5，min_speech=0.2 s，min_silence=0.6 s，max_speech=15 s。没有录制用户声音。

| 输入 | 检测结果 | 全进程耗时 | 峰值 RSS |
|---|---|---:|---:|
| 5 s 合成全零静音 | 0 段 | 0.3113 s | 39120 KiB |
| 1 s 静音 + 合成短句 + 1 s 静音 + 同短句 + 1 s 静音 | 2 段：1.648–3.912 s，5.648–7.912 s | 0.3611 s | 38288 KiB |

合成短句先由 FFmpeg 重采样为 16 kHz PCM16，仅用于 VAD 测试，原始 TTS WAV 保留。插入的完整短句区域是约 1–3.9838 s、4.9838–7.9676 s；没有人工音素级语音边界标注。上述通过条件只检查预期分段数量，不代表起点/终点准确性、短词召回率、自然语音或噪声场景已通过。

## TTS 输出与性能

本次使用官方 C++ `sherpa-onnx-offline-tts`，FP32 模型、CPU 2 线程、sid=0、speed=1（工具参数固定），`--tts-silence-scale=1`；依次应用 date、number、phone、new_heteronym FST。模型原生输出 float32 单声道 **44100 Hz**，官方 WAV writer 写出 PCM16。未播放耳机或执行蓝牙测试。

| 文本 / 文件 | WAV 时长 | Generate 耗时 | 合成 RTF | 全进程耗时 | 峰值 RSS |
|---|---:|---:|---:|---:|---:|
| 你好，我是小派，很高兴为你服务。 / `tts-chinese.wav` | 2.983764 s | 3.856 s | 1.292 | 13.315951 s | 552880 KiB |
| 今天是2026年10月20日，温度是23.5度，一共有123个样本。 / `tts-numbers-date.wav` | 6.060408 s | 7.876 s | 1.300 | 17.925902 s | 553132 KiB |

两份 WAV 均为 44100 Hz / 1 声道 / PCM16，帧数分别 131584、267264。日志证实数字日期文本归一化为“今天是二零二六年十月二十日，温度是二十三点五度，一共有一百二十三个样本。”，这只确认文字归一化和生成完成，不代表实际发音、声音质量已经用户确认。英文受 lexicon 覆盖限制，本次未测试英文。

官方 `Elapsed seconds` 只计 `Generate`；全进程耗时另含加载模型、初始化词典/FST 和写 WAV。两例各启动一次新进程，属于单次加载测试，不是预热样本分位数。两次合成 RTF 均大于 1，尚未满足 A05 预热 P95 RTF < 1 的验收目标。峰值 RSS 来自 Linux `wait4.ru_maxrss`，单位 KiB；同时以 50 ms 周期采集 RSS/PSS、线程数、MemAvailable、SwapFree 和映射路径，采样峰值不等同于严格 PSS 峰值。

后续接入 16 kHz 内部链路时，应从原生 44.1 kHz float32 使用有状态重采样器转换至 16 kHz，再按 320 样本/20 ms 分帧并标记尾帧有效长度；保留跨块滤波状态，不能只改采样率字段。若播放器接受原生采样率，可在播放边界转换至设备格式。这里仅说明适配要求，尚未接入项目实时引擎。

## 复查命令及证据

所有模型初始化、推理、性能采集使用同一互斥锁。官方 CLI 的完整 argv 保存在每份 `*.metrics.json`，包含原生输出路径和文本；`validate_tts.py`、`validate_vad.py` 是本次步骤的可读脚本，默认指向本次目录，已有证据禁止覆盖，重新采样应更换 run 目录。

在工程根目录查看版本（不打开模型或设备）：

```bash
deps/model-prep/speech/sherpa-onnx-v1.12.14-linux-aarch64-shared-cpu/bin/sherpa-onnx-version
flock -x /tmp/rk3588_voice_engine_model_validation.lock deps/model-prep/speech/runtime_probe
```

复现推理时，取 `*.metrics.json` 中的 `command` 并把输出文件改为新的证据目录；运行形态为：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock   python3 deps/model-prep/speech/measure.py NEW_RUN/CASE   deps/model-prep/speech/sherpa-onnx-v1.12.14-linux-aarch64-shared-cpu/bin/sherpa-onnx-offline-tts [已记录参数]
```

证据包括下载 API 元数据、`downloads.json`、`onnxruntime-download.json`、`runtime-loading.json`、两个 ONNX 信息 JSON、编译命令/结果、TTS 日志/WAV/指标、VAD 输入/输出/日志/summary，以及 `files.sha256.json`。

清单片段位于本 run 下 `models.fragment.json` 和 `deps.fragment.json`，路径分别以未来 `models/manifest.json`、`deps/manifest.json` 的目录为基准。每个模型直接声明 sherpa 和 ORT 两项依赖；每个文件记录实际字节数与 SHA-256。主对话负责合并公共清单及入口文档。

## 尚未验证

自然录音 VAD 误检/漏检/边界、TTS 声音质量与用户确认、英文与词典外词、INT8 权重、持续热运行/预热分位数、取消延迟、多模型共驻与同进程 SDK 共存、耳机播放和实时引擎接入均未验证。KWS 模型本轮未准备。短例通过不代表完整语音链路或发布验收通过。

## 2026-10-21 P0-03 资产复核

本轮只复核资产与来源，复用此前 VAD/TTS 的运行证据，没有再执行 CPU 模型或改变 sherpa-onnx 1.12.14 / ORT 1.17.1-patched。现有 Melo 包内 20 个文件及完整归档均已登记；FP32 权重、tokens、lexicon、dict 和四个 FST 齐备。公共清单补充原始 ONNX 输入/输出、UTF-8 前端、sid=0、speed=1 及 `date → number → phone → new_heteronym` 规则顺序，不改变实际运行参数。

来源补充保存在 `models/tts/vits-melo-tts-zh_en/third-party/`：FST 来源模型卡、Apache-2.0 文本、CppJieba MIT 参考文本及 `provenance.json`，均有大小和 SHA-256。四个现存 FST 的 Git blob 与发布者仓库 `csukuangfj/icefall-tts-aishell3-vits-low-2024-04-06` 固定 revision `57345c004e13ed640e408c4c29ab56187b18f065` 内容一致，模型卡声明 Apache-2.0；这证明相同内容的固定来源，不证明该 revision 就是历史 Melo 打包时下载的 revision。原 MyShell MIT 许可证保留，清单按组件分别说明。

CppJieba 参考 tag 固定到 `f03dd931a00330c279c8590275850fd1f6749a18`，其仓库 MIT 文本已保留；尚未证明实际词典归档的历史内容身份，不用该参考声称词典完整溯源已解决。固定 sherpa 导出工作流仅为路线参考，不能冒充此模型包的实际导出记录。

Silero v5.0 原始模型提交已固定，但实际历史 ONNX exporter 版本和命令仍未知；Melo 原始权重 revision、历史 exporter revision/命令/工具链和词典完整历史来源仍缺。FP32 的量化校准不适用，runtime 版本不等于 exporter 版本。上述缺项按执行计划 §7 保留，**P0-03 尚不能关闭**；TTS 未试听及完整 A05 留在后续阶段。详细逐项判定和小型来源获取记录见 `output/runs/p0-assets-20261021-144234/`，总状态见[环境记录 §41](environment.md#41-p0-剩余任务核查与十问取消验证)。

## 2026-10-21 P0 来源收尾与范围

本轮复用 CPU VAD 的静音 0 段/合成语音 2 段及中文、数字日期 TTS WAV 证据，未再次运行 VAD/TTS或试听。完整包、私有 sherpa-onnx 1.12.14 / ORT 1.17.1 和输入输出约定不变。P0/M0 最小基线 verified 不代表完整 A05、发音或声音质量获确认。

新增[固定发布快照](https://huggingface.co/csukuangfj/vits-melo-tts-zh_en/tree/a0d5c6a264c0ef92d70d8661d8cc502d79627cd6)与现有 Melo FP32 的 LFS SHA/大小相符；这是发布内容身份，不是原始 checkpoint revision。14 个小配套文件及 133 B INT8 指针另核 Git blob，4 个大配套仅核大小；INT8 指针仍不可加载，没有换用 INT8。

从现存 ONNX 小范围读取的 ModelProto 自述：Melo producer `pytorch 2.3.1`、Silero producer `spox`（未写版本），两者 IR version=8。它们补充产物元数据，不证明完整历史生成工具链；精确导出命令、Melo 原权重与词典历史仍 unknown。按用户确认的预转换产物使用范围，本轮有界来源核查完成，不再以未公开历史阻塞 P0。发布者声明、匹配内容、参考许可和未知项继续分开；详见[来源报告](../output/runs/p0-r-closeout-20261021-161053/sources/report.md)及[元数据证据](../output/runs/p0-r-closeout-20261021-161053/sources/onnx-producer-metadata.json)。总状态见[环境记录 §43](environment.md#43-p0-收尾与首个已验证组合)。
