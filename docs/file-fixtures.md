# 固定文件测试集

合成材料生成于 2026-10-20；2026-10-22 完成校验加固、20 条自然人声录制与人工审核（Asia/Shanghai）。版本化材料目录为 `tests/fixtures/`，自然 WAV 只保存在板端被忽略的私有证据目录。全部操作通过现有 SSH 在 `/home/orangepi/projects/rk3588_voice_engine` 完成；本轮结果见[环境记录 §46](environment.md#46-p1-03-自然人声与人工材料审核)，此前自动化回归见[§44](environment.md#44-p1-自动化回归与固定集校验加固)。

当前维护两组分别使用的固定材料：**MeloTTS 合成工程回归集**和**耳机自然人声集**。自然 20 条均经回听确认；合成 20 条审核完成，19 条通过，`mix_02` 存在“调低→吊低”发音缺陷。输入、参考读法与关键词均先冻结，未用 ASR 输出反推或筛选。自然录音与合成集使用相同文本，只增加独立的人声波形；两组不得合并准确率，也不能据材料审核宣称 A03/A05 通过。当前A03正式质量验收采用自然人声20条，旧合成识别失败作为非阻塞回归保留；材料人工审核和模型验收仍分别记录。完整条件见 [ACCEPTANCE.md](ACCEPTANCE.md#a03-正式集与旧回归集的定位)。

## 内容及文件职责

| 文件 | 内容 |
|---|---|
| [manifest.json](../tests/fixtures/manifest.json) | 冻结的 20 条合成主体与 6 条边界 WAV；保留生成时的参考与人工审核历史字段 |
| [natural-corpus.json](../tests/fixtures/natural-corpus.json) | 自然集公共目录清单：20 条元数据、参考/关键词、人工审核、私有 manifest 位置与哈希；不是 probe 输入 |
| [synthetic-human-review.json](../tests/fixtures/synthetic-human-review.json) | 绑定原 manifest 和 WAV 哈希的后续人工审核；19 条通过、1 条发音问题，另 2 条有句子边界通过 |
| [model-acceptance-review.json](../tests/fixtures/model-acceptance-review.json) | 范围调整前的 A03/A05 模型审核快照与私有证据哈希；当时A03未关闭、A05试听9/10；不是probe输入 |
| [asr-natural-acceptance.json](../tests/fixtures/asr-natural-acceptance.json) | 当前A03自然人声正式范围、既有通过证据及旧合成失败非阻塞决定；P2-01完成；不是probe输入 |
| [tts-model-acceptance.json](../tests/fixtures/tts-model-acceptance.json) | 当前A05/P2-03修正后正式验收、三批性能与十条试听证据；旧9/10记录保留；不是probe输入 |
| [speech_cases.json](../tests/fixtures/speech_cases.json) | 生成前冻结的 20 条输入文本、预期口语参考、关键词与类别 |
| [audio/](../tests/fixtures/audio/) | 实际交付的 26 个 16 kHz、单声道、PCM S16LE WAV，包含有效空 WAV |
| [tts_cases.json](../tests/fixtures/tts_cases.json) | 10 条 TTS 文本：普通中文 3、数字日期 3、标点 2、中英 2 |
| [llm_cases.json](../tests/fixtures/llm_cases.json) | 20 个固定短问题及短答检查约定；备料阶段未执行，后续已授权的 P2 结果见下方审核入口 |
| [generator-lock.json](../tests/fixtures/generator-lock.json) | 26 个生成依赖文件的大小与 SHA-256、固定版本、合成参数与来源 |
| [licenses/MeloTTS-LICENSE](../tests/fixtures/licenses/MeloTTS-LICENSE) | 原始 MeloTTS MIT 许可证，保留 MyShell.ai 版权声明 |
| [prepare_fixtures.py](../scripts/prepare_fixtures.py) | 板端批量生成及原生 WAV 重建入口，拒绝覆盖已有清单与音频 |
| [check_fixtures.py](../tests/integration/check_fixtures.py) | 无模型、无网络、无音频设备的完整性、覆盖和构造关系校验 |
| [check_fixtures_negative.py](../tests/integration/check_fixtures_negative.py) | 临时副本的正例及故障注入回归；CTest 名称 `fixed_fixtures_negative` |

原合成主体类别固定为普通中文 `chinese_common` 8 条、数字日期 `numbers_dates` 5 条、短问句 `short_question` 4 条、中英混说 `code_switch` 3 条。20 条 WAV 时长为 **2.000000～3.308875 秒**。`q_02` 原始重采样时长 1.7995625 秒，补 3207 个零；`q_04` 原始时长 1.8691875 秒，补 2093 个零。清单同时记录 `unpadded_duration_sec` 与 `trailing_padding_samples`，不把静音补齐视为新增有效语音。其他主体样本没有补零。

26 个 WAV 的 PCM 数据合计 2,431,404 字节；包含文件头的字节数以各项 `size_bytes` 和 `delivery.json` 为准。原生 44.1 kHz PCM16 WAV 共 4,345,712 字节，保留于证据目录 `native/`，不混入固定 16 kHz 输入。

## 自然人声材料

自然集 ID 为 `natural-headset-zh-v1-20261022`。20 条 `natural_*` 样本按普通中文 8、数字日期 5、短问句 4、中英混说 3 冻结；均为 16 kHz、单声道、PCM S16LE，时长 **2.98～4.68 s**，合计 **75.42 s**。用户戴 vivo TWS 4，在确认安静的环境中朗读；profile 为 `headset-head-unit-msbc`，麦克风音量 74%。波形只从原录音按样本位置裁剪，保留约 0.25 s 句前和 0.35 s 句后原音频；未补零、降噪或调整样本增益。

私有 bundle：`output/runs/p1-natural-review-20261022-104035/natural/`，包含 `manifest.json` 和 `audio/natural_<原用例ID>.wav`。所有音频路径均在 manifest 目录内。清单 SHA-256 为 `f51b14f9e6e23a9af935ab534094c95cba09dcc21c5b4dacd3e96b1e2abd8142`；[公共 catalog](../tests/fixtures/natural-corpus.json) 标明 `catalog_only: true` / `probe_input: false`，不能代替私有 manifest 传给 `asr_probe`。仅取得 Git 仓库不能取得这些录音，未获公开再分发授权。

`recording-plan.json` 在录音前固定文字、归一化参考与关键词。第 9 条 `num_01`、第 20 条 `mix_03` 因用户自报读错各重录一次，最终版本回放后才选用；原始失败录音及原因保留，没有运行 ASR 或按识别成绩选样。用户以五句一组回听裁剪后的文件，20 条全部确认文字一致、首尾完整；这是同一说话者的文本对照试听，并非盲审或另一位审核者。与合成集同文本、单说话人、安静佩戴耳机的限制随清单记录，不外推多说话人、独立文本或噪声泛化；本轮未作为量化校准输入。

`natural_q_02` 有 1/47680 个 `abs(PCM)>=32760` 的近满幅样本，约 0.0021%；其余 19 条无此类样本。用户确认该组声音正常。记录这一信号事实，不把单点近满幅等同于持续失真，也不宣称整集零削顶。

## 人工审核覆盖与已知问题

[合成人工审核记录](../tests/fixtures/synthetic-human-review.json) 通过原 manifest SHA-256、样本 ID、WAV SHA-256 与私有决策记录绑定试听结果；它是后续审核记录，不重写冻结 manifest 中的 `human_audio_review: not_run` 或 `reference_kind` 历史字段。

| 范围 | 2026-10-22 人工结论 |
|---|---|
| 20 条自然人声 | 20/20 参考一致、首尾完整，含两条经重录确认的最终版本 |
| 20 条原合成主体 | 全部审完；19 条通过，`mix_02` 发音问题保留 |
| `mix_02` 第 19 句 | 原参考“请播放 music，然后调低音量。”；用户单独复听报告“调低”听成“吊低”，其余第 16/17/18/20 句正常。只记录问题片段，不凭片段改写全文转写 |
| `boundary_long15` / `boundary_tail` | 用户确认四遍完整、尾部“新鲜空气进来”完整；不等于音素边界标注或端点算法验收 |
| 空、静音、80 ms 短片段、噪声 | 由原结构/构造校验覆盖；未伪称完整句子人工发音审核 |

`mix_02` 在材料审核时尚未确定前端/词典/模型中的具体原因；后续 P2 已确认原“调低”的前端读音为 diào dī，并验证可选词级修正，见[P2 诊断](tts-probe.md#p2-流式队列与补充读音2026-10-22)。原 WAV、参考/关键词与准确率分母不改，不因本轮发现剔除失败样本或追改历史 ASR 13/20、CER 15.96%。该分数不能全归因为识别模型错误。P1-03 的材料与审核工作已完成；后续 A03 自然集评测及 A05 正式十条试听已执行，分别见[ASR 结果](asr-probe.md#2026-10-22-自然人声评测)和[TTS最新复验](tts-probe.md#2026-10-22-搜索修正与-a05-复验)。冻结 catalog 的 `asr_evaluation_status: not_run` 仅指材料冻结时；当前模型审核另存独立记录，不修改原材料清单。

## 清单接口与评分约定

原合成及私有自然音频 `manifest.json` 根包含整数 `schema_version: 1` 和 `samples` 数组。每个样本包含：

- `sample_id`、`path`、`sha256`、`size_bytes`；`path` 必须相对于当前 manifest 所在目录，当前均为 `audio/<sample_id>.wav`。
- `sample_rate`、`channels`、`num_samples`、`duration_sec`、`sample_width_bytes`、`encoding`，均来自实际 WAV，而非预计值。
- `reference_text`、`normalized_reference`、`keywords`、`category`、`expected_behavior`。
- `synthetic`、`source`、`human_audio_review`、`reference_kind`；原合成主体 `source` 记录原生 WAV 的仓库相对证据路径及 SHA-256，合成条件引用同目录 `generator-lock.json`。

自然 manifest 的 `synthetic` 为 false，`human_audio_review` 为 passed，`source` 记录原录音哈希与裁剪样本边界；原合成 manifest 保留生成时字段。公共 catalog 使用 `catalog_schema_version`，没有 probe 要求的顶层 `schema_version`，避免误当音频 manifest。

主体 `expected_behavior` 为 `speech`。边界 `category` 为 `boundary`，不加入 20 条主体的准确率分母。读取方应保留未知扩展字段。

文本归一化版本为 `nfkc-casefold-strip-punctuation-space-v1`：NFKC、casefold、去 Unicode `P*` 标点及空白；**不转换数字，不抹掉数值差别**。例如 `num_02.input_text` 为“当前温度是23.5度。”，合成前手工固定的参考读法为“当前温度是二十三点五度。”。数字参考不是 ASR 输出；原始 TTS 输入和前端日志均保留。采用其他数字表示的 ASR 输出若需等价处理，应另行制定版本化评分规则，不能偷偷修改本集参考或现有归一化口径。

`tts_cases.json` 和 `llm_cases.json` 根为 `schema_version: 1`、`cases`；每条至少包含 `id`、`text`、`category`。LLM 根保留 `execution_status: not_run` 和 `execution_prohibited_in_this_round: true` 两个材料冻结阶段的历史字段；当前执行授权和 P2 结果另存独立审核，不据静态题目本身判断是否通过。

## 边界样本

| ID / expected_behavior | 实际构造及预期 |
|---|---|
| `boundary_empty` / `empty` | 有效 WAV 头、0 样本；结束与 reset 不应制造语音文本 |
| `boundary_silence` / `silence` | 精确 3 秒全零；不应报告语音或最终转写 |
| `boundary_short` / `short_audio` | `zh_01` 中点起 1280 样本，即 80 ms；没有完整词参考，只检查无崩溃、挂起或缓存串轮 |
| `boundary_long15` / `long_audio` | 完整重复 `zh_01` 四次，末尾补 28232 个零，精确 240000 样本 / 15 秒；参考也重复四次，不截掉单词 |
| `boundary_tail` / `tail_speech` | 2 秒前导静音，加 `zh_01` 到最后 `abs(PCM)>32` 的样本并最多保留 20 ms 尾部；本次 5.308875 秒，末 100 ms 内存在信号，检查 EOF flush；不是人工音素边界标注 |
| `boundary_noise` / `noise` | 3 秒固定 xorshift32 噪声，种子 `0x13579BDF`、整数幅度 [-512,512]；无语音参考，单独检查误识别 |

15 秒样本是完整短句重复加尾部静音的工程边界，不代表 15 秒自然连续发言；尾部边界由幅值规则构造，生成时未经人工核听；2026-10-22 已确认尾音完整，但仍不是音素边界标注。空、静音、短片段、噪声的参考与关键词均为空，不计算 CER。

## 合成材料来源、许可证与生成条件（2026-10-20）

已检查 [AISHELL 官方入口](https://www.openslr.org/33)：其公开主音频/转写包约 15 GB。原生成轮次没有为这组小规模覆盖下载该完整语料；当时按授权使用已在板端验证的 MeloTTS 合成，未使用用户录音。此选择不表示已穷尽全部公开小语料候选。

MeloTTS 来源为 [sherpa 官方发布包](https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-melo-tts-zh_en.tar.bz2)，资产 ID **203769960**，原包 SHA-256 **`e58351ed7149f290a54534538badd4077cdbe6fddc964b24d0bee870415d1514`**。原始模型 commit 和 exporter commit 未由发布者提供，不能编造；以提取文件的逐项哈希锁定实际输入。该包包含 MeloTTS MIT 许可证，本集附原文；测试文本为本轮编写、音频由本地模型生成，没有把第三方自然录音的许可证套到合成音频上。模型来源和限制另见 [模型准备记录](model-prep-speech.md)。

生成组合：sherpa-onnx **1.12.14** / commit **`26aa2fa93210376a89de3a65a1a4dd320c37f5e9`**，配套 ORT **1.17.1-patched**，Melo FP32，CPU 2 线程、sid 0、speed 1、noise_scale 0.667、noise_scale_w 0.8、length_scale 1、silence_scale 1、max_num_sentences 1，依次应用 date/number/phone/new_heteronym FST。

生成器使用配套 **C API**；官方 C++ 包装库的 `std::string` 符号采用旧 ABI，普通默认 C++17 ABI 直接链接失败，因此没有修改工程全局 ABI。初始化与 20 条合成在同一驻留进程完成，并由 `flock -x /tmp/rk3588_voice_engine_model_validation.lock` 包住整个模型与资源采集进程。实际加载的 sherpa/ORT 均来自项目私有目录；路径见 `generation.metrics.json`。原生成轮次没有改系统库、设备路由、音频服务或配置，也没有播放样本；后续试听见上文审核记录。

模型输出 float32，由官方 writer 保存原生 44.1 kHz PCM16；FFmpeg 真正重采样并导出 16 kHz PCM16，不是仅修改 WAV 头。完整 argv 见 `conversion-commands.json`。固定 WAV 与原生 WAV 的 SHA-256 定义本集身份。Melo 带随机噪声，重新合成不承诺逐字节一致；同条件重合成是新候选，必须新目录、新哈希，不能静默替换固定语料。从保留原生 WAV 重建已实测 26/26 哈希相同。

## 校验与复现

在板端工程根目录离线校验原合成工程集：

```bash
python3 tests/integration/check_fixtures.py
```

成功退出 0，输出 `status: passed`；清单、哈希、覆盖、格式或边界构造不满足要求时退出 2，并给出 `FIXTURE_CHECK_FAILED`。校验器检查路径逃逸、JSON 对象与整数类型、重复键/ID、数据截断、帧数/时长、主体与 TTS 类别数量、冻结参考及文本数量。长语音/尾部语音的参考和关键词必须与源句对应；固定噪声须逐样本符合既定 xorshift32 配方；短片段、重复次数、尾部阈值和补零元数据须与 PCM 构造一致。未知扩展字段和 manifest 样本排序仍可保留。

新增负例测试可直接执行 `python3 tests/integration/check_fixtures_negative.py`，也随 CTest 运行。它只复制并修改临时文件，原固定集不变，不运行模型或访问音频设备。补零字段的内部一致性不能独立证明原生音频长度；生成来源仍依靠保留的原生 WAV 与生成证据，该校验器不判断发音或参考转写，人工结果见上文审核记录。

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

## 2026-10-20 生成验证与证据

主证据目录：`output/runs/file-fixtures-20261020-210044/`。

- `speech_cases.json` 在运行前冻结；SHA-256 `0d6cc610d7967ff4340fda02e2d2b8c5db0ef47d196fb6bebf9a1d97fa8777b9`。
- 已交付 `manifest.json` SHA-256 `49cd7104e1eb635c4a428c6d52788b0694d3e4d0fd3caf513fd7288374472369`。
- `generation.log`、`generation.metrics.json`、`generation-command.json`、`batch_synthesize.cpp`、编译命令/日志、`native-index.json` 保留原始生成证据与全部 20 份原生 WAV。单次加载 9.015495 秒、整进程 73.031386 秒、峰值 RSS 554604 KiB；这些是语料准备开销，不代替 TTS probe 的预热性能验收。
- `check-fixtures.json`：26 WAV、20 主体 / 6 边界、10 TTS / 20 静态 LLM，全量离线校验通过。
- `negative_checks.py` / `negative-checks.json`：9 个反例均返回 2，覆盖哈希损坏、路径逃逸、重复 ID、缺样本、冻结文本改写、时长错误、格式元数据错误、更新哈希后仍截断的 WAV、重复 JSON 键。
- `reconstruction-check.json` 及 `output/runs/file-fixtures-20261020-210044-rebuild/`：从原生重建后 26/26 WAV 哈希完全相同；重建未加载模型。

当时自然人声、人工试听、数字与英文发音确认均未完成；以上为原生成阶段证据，不因后续审核改写历史结论。参考文本/关键词不因识别结果改变，失败保留在 probe 报告中。


## 2026-10-22 自然材料与审核证据

证据根目录为 `output/runs/p1-natural-review-20261022-104035/`。`recording-plan.json` 保存录前固定用例，`captures/` 保存六次录音及十一轮回放的原始报告，`clips/` / `natural/` 保存裁剪与冻结材料，`decisions/` 保存用户确认和两次读错记录。`freeze_materials.py` 拒绝覆盖冻结包；`verify_materials.py` 在不加载模型的情况下检查格式、哈希、类别、原始 PCM 裁剪、人工确认绑定、重录选择、私有忽略规则和原输入保护，结果为 `material-verification.json`。合成原集校验结果另存 `check-fixtures.json`。

在保留完整私有证据的板端可重新核对材料：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 output/runs/p1-natural-review-20261022-104035/verify_materials.py
```

此脚本带本轮 HEAD、配置和既有差异保护，后续工程演进导致保护检查失败时应记录新验证批次，不覆盖或放宽原证据。它不是无私有录音时可运行的公共 CTest；原 `check_fixtures.py` 仅用于合成 26 条清单。后续模型评测使用 `natural/manifest.json` 作为自然集输入，并独立保存评分；本节材料轮次没有运行模型。随后已完成的模型评测及试听见[环境记录 §47](environment.md#47-a03-自然人声评测与-a05-正式试听)，自然与合成分数不合并。

17 次录放正常结束，软件计数检查通过；用户要求后输出音量从 74% 降到 50%，麦克风保持不变。所有录放和材料校验共用执行锁；期间未并行编译或做大文件校验。本轮仅增补材料/审核元数据与文档，没有提交或推送。详细数值、范围与历史结果边界见[环境记录 §46](environment.md#46-p1-03-自然人声与人工材料审核)。

## P2后续审核入口（2026-10-22）

P2端点/常驻LLM/流式TTS的本轮结果见[审核摘要](../tests/fixtures/p2-adapter-review.json)和[环境 §48](environment.md#48-p2-模型适配端点与流式合成)。原 natural-corpus、synthetic-human-review、model-acceptance-review 及原WAV/参考/关键词继续作为其历史阶段证据，不追改为本轮结果；原LLM题目顶部的not_run/禁止字段也是备料阶段快照，本轮已获用户执行授权，实际状态另存。补充读音的新WAV仅作候选诊断，没有替换原mix_02或剔除失败样本。

当前A03/P2-01状态由后续[自然集验收记录](../tests/fixtures/asr-natural-acceptance.json)及[环境 §49](environment.md#49-a03-自然人声正式验收范围)承接：自然20条成为正式质量集，旧合成质量失败不再阻塞完成，所有历史清单/评分及既有回归工具保留。本次不替换、删除或重新生成材料。
