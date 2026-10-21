# 固定文件测试集

本轮日期：2026-10-20，Asia/Shanghai。交付目录为 `tests/fixtures/`；板端证据为 `output/runs/file-fixtures-20261020-210044/`。全部工作通过现有 SSH 在 `/home/orangepi/projects/rk3588_voice_engine` 完成。

这是一套 **MeloTTS 合成工程回归集**，与被测 TTS 使用同一模型，不是自然语音准确率测试集。输入文本、预期读法及关键词在合成和 ASR 测试前冻结；没有从 ASR 输出反推参考答案，没有依据识别结果筛选或删除样本。参考文本表示预期读法，尚无人工试听逐字核对，不能宣称 A03 自然语音准确率或 A05 声音质量通过。完整验收条件仍见 [ACCEPTANCE.md](ACCEPTANCE.md)。

## 内容及文件职责

| 文件 | 内容 |
|---|---|
| [manifest.json](../tests/fixtures/manifest.json) | 20 条主体语音与 6 条边界 WAV 的实际格式、哈希、参考文本及来源 |
| [speech_cases.json](../tests/fixtures/speech_cases.json) | 生成前冻结的 20 条输入文本、预期口语参考、关键词与类别 |
| [audio/](../tests/fixtures/audio/) | 实际交付的 26 个 16 kHz、单声道、PCM S16LE WAV，包含有效空 WAV |
| [tts_cases.json](../tests/fixtures/tts_cases.json) | 10 条 TTS 文本：普通中文 3、数字日期 3、标点 2、中英 2 |
| [llm_cases.json](../tests/fixtures/llm_cases.json) | 20 个短问题及短答检查约定，仅静态用例；本轮禁止初始化或执行 LLM |
| [generator-lock.json](../tests/fixtures/generator-lock.json) | 26 个生成依赖文件的大小与 SHA-256、固定版本、合成参数与来源 |
| [licenses/MeloTTS-LICENSE](../tests/fixtures/licenses/MeloTTS-LICENSE) | 原始 MeloTTS MIT 许可证，保留 MyShell.ai 版权声明 |
| [prepare_fixtures.py](../scripts/prepare_fixtures.py) | 板端批量生成及原生 WAV 重建入口，拒绝覆盖已有清单与音频 |
| [check_fixtures.py](../tests/integration/check_fixtures.py) | 无模型、无网络、无音频设备的完整性和覆盖校验 |

主体类别固定为普通中文 `chinese_common` 8 条、数字日期 `numbers_dates` 5 条、短问句 `short_question` 4 条、中英混说 `code_switch` 3 条。20 条 WAV 时长为 **2.000000～3.308875 秒**。`q_02` 原始重采样时长 1.7995625 秒，补 3207 个零；`q_04` 原始时长 1.8691875 秒，补 2093 个零。清单同时记录 `unpadded_duration_sec` 与 `trailing_padding_samples`，不把静音补齐视为新增有效语音。其他主体样本没有补零。

26 个 WAV 的 PCM 数据合计 2,431,404 字节；包含文件头的字节数以各项 `size_bytes` 和 `delivery.json` 为准。原生 44.1 kHz PCM16 WAV 共 4,345,712 字节，保留于证据目录 `native/`，不混入固定 16 kHz 输入。

## 清单接口与评分约定

`manifest.json` 根包含整数 `schema_version: 1` 和 `samples` 数组。每个样本包含：

- `sample_id`、`path`、`sha256`、`size_bytes`；`path` 必须相对于当前 manifest 所在目录，当前均为 `audio/<sample_id>.wav`。
- `sample_rate`、`channels`、`num_samples`、`duration_sec`、`sample_width_bytes`、`encoding`，均来自实际 WAV，而非预计值。
- `reference_text`、`normalized_reference`、`keywords`、`category`、`expected_behavior`。
- `synthetic`、`source`、`human_audio_review`、`reference_kind`；主体 `source` 记录原生 WAV 的仓库相对证据路径及 SHA-256，合成条件引用同目录 `generator-lock.json`。

主体 `expected_behavior` 为 `speech`。边界 `category` 为 `boundary`，不加入 20 条主体的准确率分母。读取方应保留未知扩展字段。

文本归一化版本为 `nfkc-casefold-strip-punctuation-space-v1`：NFKC、casefold、去 Unicode `P*` 标点及空白；**不转换数字，不抹掉数值差别**。例如 `num_02.input_text` 为“当前温度是23.5度。”，合成前手工固定的参考读法为“当前温度是二十三点五度。”。数字参考不是 ASR 输出；原始 TTS 输入和前端日志均保留。采用其他数字表示的 ASR 输出若需等价处理，应另行制定版本化评分规则，不能偷偷修改本集参考或现有归一化口径。

`tts_cases.json` 和 `llm_cases.json` 根为 `schema_version: 1`、`cases`；每条至少包含 `id`、`text`、`category`。LLM 根额外明确 `execution_status: not_run` 和 `execution_prohibited_in_this_round: true`；存在静态问题不代表已执行 LLM。

## 边界样本

| ID / expected_behavior | 实际构造及预期 |
|---|---|
| `boundary_empty` / `empty` | 有效 WAV 头、0 样本；结束与 reset 不应制造语音文本 |
| `boundary_silence` / `silence` | 精确 3 秒全零；不应报告语音或最终转写 |
| `boundary_short` / `short_audio` | `zh_01` 中点起 1280 样本，即 80 ms；没有完整词参考，只检查无崩溃、挂起或缓存串轮 |
| `boundary_long15` / `long_audio` | 完整重复 `zh_01` 四次，末尾补 28232 个零，精确 240000 样本 / 15 秒；参考也重复四次，不截掉单词 |
| `boundary_tail` / `tail_speech` | 2 秒前导静音，加 `zh_01` 到最后 `abs(PCM)>32` 的样本并最多保留 20 ms 尾部；本次 5.308875 秒，末 100 ms 内存在信号，检查 EOF flush；不是人工音素边界标注 |
| `boundary_noise` / `noise` | 3 秒固定 xorshift32 噪声，种子 `0x13579BDF`、整数幅度 [-512,512]；无语音参考，单独检查误识别 |

15 秒样本是完整短句重复加尾部静音的工程边界，不代表 15 秒自然连续发言；尾部边界由幅值规则构造，未人工确认尾音质量。空、静音、短片段、噪声的参考与关键词均为空，不计算 CER。

## 来源、许可证与生成条件

已检查 [AISHELL 官方入口](https://www.openslr.org/33)：其公开主音频/转写包约 15 GB。本轮不为这组小规模覆盖下载该完整语料；按本轮授权使用已在板端验证的 MeloTTS 合成，也未使用用户录音。此选择不表示已穷尽全部公开小语料候选。

MeloTTS 来源为 [sherpa 官方发布包](https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-melo-tts-zh_en.tar.bz2)，资产 ID **203769960**，原包 SHA-256 **`e58351ed7149f290a54534538badd4077cdbe6fddc964b24d0bee870415d1514`**。原始模型 commit 和 exporter commit 未由发布者提供，不能编造；以提取文件的逐项哈希锁定实际输入。该包包含 MeloTTS MIT 许可证，本集附原文；测试文本为本轮编写、音频由本地模型生成，没有把第三方自然录音的许可证套到合成音频上。模型来源和限制另见 [模型准备记录](model-prep-speech.md)。

生成组合：sherpa-onnx **1.12.14** / commit **`26aa2fa93210376a89de3a65a1a4dd320c37f5e9`**，配套 ORT **1.17.1-patched**，Melo FP32，CPU 2 线程、sid 0、speed 1、noise_scale 0.667、noise_scale_w 0.8、length_scale 1、silence_scale 1、max_num_sentences 1，依次应用 date/number/phone/new_heteronym FST。

生成器使用配套 **C API**；官方 C++ 包装库的 `std::string` 符号采用旧 ABI，普通默认 C++17 ABI 直接链接失败，因此没有修改工程全局 ABI。初始化与 20 条合成在同一驻留进程完成，并由 `flock -x /tmp/rk3588_voice_engine_model_validation.lock` 包住整个模型与资源采集进程。实际加载的 sherpa/ORT 均来自项目私有目录；路径见 `generation.metrics.json`。没有改系统库、设备路由、音频服务或配置，也没有播放任何样本。

模型输出 float32，由官方 writer 保存原生 44.1 kHz PCM16；FFmpeg 真正重采样并导出 16 kHz PCM16，不是仅修改 WAV 头。完整 argv 见 `conversion-commands.json`。固定 WAV 与原生 WAV 的 SHA-256 定义本集身份。Melo 带随机噪声，重新合成不承诺逐字节一致；同条件重合成是新候选，必须新目录、新哈希，不能静默替换固定语料。从保留原生 WAV 重建已实测 26/26 哈希相同。

## 校验与复现

在板端工程根目录离线校验：

```bash
python3 tests/integration/check_fixtures.py
```

成功退出 0，输出 `status: passed`；清单、哈希、覆盖、格式或边界构造不满足要求时退出 2。校验器检查路径逃逸、重复 JSON 键/ID、数据截断、帧数/时长、主体类别数、冻结参考、TTS/LLM 文本数量，以及边界的源切片/重复/尾部信号关系。它不运行模型、不判断实际发音质量。

使用新 run ID 从本次原生 WAV 重新构造（不初始化模型）：

```bash
python3 scripts/prepare_fixtures.py \
  --run-id file-fixtures-NEW \
  --destination output/runs/file-fixtures-NEW/reconstructed \
  --reuse-native-run output/runs/file-fixtures-20261020-210044
python3 tests/integration/check_fixtures.py \
  --manifest output/runs/file-fixtures-NEW/reconstructed/manifest.json
```

`NEW` 换为未使用的时间标识。保留原生重建流程当前仍校验生成锁列出的本机依赖哈希；它不会加载模型。需要同条件新合成时省略 `--reuse-native-run`，使用另一个新的 `--run-id` 与 `--destination`。默认目标为固定 `tests/fixtures`，已有 manifest/audio 时明确拒绝覆盖。脚本不下载模型，缺少或变更锁定依赖时失败。

## 本轮验证与证据

主证据目录：`output/runs/file-fixtures-20261020-210044/`。

- `speech_cases.json` 在运行前冻结；SHA-256 `0d6cc610d7967ff4340fda02e2d2b8c5db0ef47d196fb6bebf9a1d97fa8777b9`。
- 已交付 `manifest.json` SHA-256 `49cd7104e1eb635c4a428c6d52788b0694d3e4d0fd3caf513fd7288374472369`。
- `generation.log`、`generation.metrics.json`、`generation-command.json`、`batch_synthesize.cpp`、编译命令/日志、`native-index.json` 保留原始生成证据与全部 20 份原生 WAV。单次加载 9.015495 秒、整进程 73.031386 秒、峰值 RSS 554604 KiB；这些是语料准备开销，不代替 TTS probe 的预热性能验收。
- `check-fixtures.json`：26 WAV、20 主体 / 6 边界、10 TTS / 20 静态 LLM，全量离线校验通过。
- `negative_checks.py` / `negative-checks.json`：9 个反例均返回 2，覆盖哈希损坏、路径逃逸、重复 ID、缺样本、冻结文本改写、时长错误、格式元数据错误、更新哈希后仍截断的 WAV、重复 JSON 键。
- `reconstruction-check.json` 及 `output/runs/file-fixtures-20261020-210044-rebuild/`：从原生重建后 26/26 WAV 哈希完全相同；重建未加载模型。

参考文本/关键词不因后续识别结果改变；失败应留在 probe 报告中。自然语音独立测试集、用户试听、数字与英文的实际发音确认、自然录音准确率及实机链路仍未验收。本轮未初始化或推理 LLM，没有 Git 提交或推送。
