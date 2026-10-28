# 模型与运行库准备

本页用于已经满足[支持环境](supported-platform.md)的新开发板。所有命令在板端源码目录执行，沿用 C++17 / CMake / Ninja；模型转换也可以在该 ARM64 板端完成。网络只用于首次获取资产，日常推理使用本机文件。

项目提供固定来源、逐文件大小与 SHA-256、下载与解包脚本，以及 Zipformer 的转换配方。模型权重和运行库由各上游分发，不包含在本项目源码包中。许可证范围见[第三方声明](../THIRD_PARTY_NOTICES.md)。

## 固定资产与获取方式

| 组件 | 固定来源 | 准备方式及目标 |
|---|---|---|
| RKNN Runtime 2.3.2 | [rknn-toolkit2 固定提交](https://github.com/airockchip/rknn-toolkit2/tree/42aa1d426c0a9e0869b6374edba009f7208a1926) | Linux aarch64 库及头文件放到 `deps/rknn-2.3.2/` |
| RKLLM Runtime 1.3.0 | [rknn-llm 固定提交](https://github.com/airockchip/rknn-llm/tree/878f9361fd3afa7e167b7079918918f78d2c1c2a) | Linux aarch64 库及头文件放到 `deps/rkllm-1.3.0/` |
| Sherpa-ONNX 1.12.14 | [官方 CPU ARM64 shared 包](https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.12.14)，源码提交 `26aa2fa93210376a89de3a65a1a4dd320c37f5e9` | 解包库并另外获取同版本 C/C++ API 头文件，放到 `deps/model-prep/speech/` |
| ONNX Runtime 1.17.1-patched | [Sherpa 配套 ARM64 发布包](https://github.com/csukuangfj/onnxruntime-libs/releases/tag/v1.17.1) | 保留头文件、许可证、ThirdPartyNotices 和库；不是任意最新版 ORT |
| kaldi-native-fbank | [Model Zoo 固定提交](https://github.com/airockchip/rknn_model_zoo/tree/bad6c7334531becaf90a561988519b7bec34d0ab/3rdparty/kaldi_native_fbank) | ARM64 静态库及头文件放到 `deps/model-prep/asr/upstream/3rdparty/kaldi_native_fbank/`；现有 CMake 使用 `-no-pie` |
| Qwen3.5-2B RK3588 W8A8 | [社区转换模型固定 revision](https://huggingface.co/HanzoHuang/Qwen3.5-2B-RKLLM/tree/888bdd28aa55fc77ce135ab3e1b0c6c395f5b108) | 下载约 2.94 GB 的 RKLLM 文件、模型卡与来源声明到 `models/llm/qwen3.5-2b/`；仅文本模式 |
| Zipformer 双语 FP16 | [固定 ONNX exp/96](https://huggingface.co/csukuangfj/k2fsa-zipformer-bilingual-zh-en-t/tree/e2382758de9a0219b4efe682b95af30b399db3b8/exp/96) | 下载 encoder / decoder / joiner、词表与特征参数，再在板端用 Toolkit2 2.3.2 转换 |
| Silero VAD v5 | [Sherpa 发布文件](https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad_v5.onnx) | ONNX 和 v5.0 MIT 许可证放到 `models/vad/silero-v5/` |
| MeloTTS 中文/英文 FP32 | [完整模型包](https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-melo-tts-zh_en.tar.bz2) | 解包到 `models/tts/vits-melo-tts-zh_en/`，保留 `dict/`、lexicon、tokens、四个 FST 与许可声明 |
| 中文关键词唤醒 KWS | [Wenetspeech 3.3M 包](https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01.tar.bz2) | 解包到 `models/kws/`；唤醒词配置使用仓库 `config/wake-keywords.txt` |

可执行的逐文件 URL、归档成员与目标路径在 [sources.json](../scripts/assets/sources.json)，内容身份来自[模型清单](../models/manifest.json)与[依赖清单](../deps/manifest.json)。发布 tag 链接的内容可能变化，因此下载仍必须通过哈希校验；不在失败后自动退回其他版本。

Qwen 社区转换者没有完整公开其原始权重 revision 和校准张量，当前可复现的是固定转换产物的获取。Melo 使用完整 FP32 `model.onnx`；包里的 `model.int8.onnx` 是 133 字节 LFS 指针，不是可加载模型。字典直接来自已锁定的完整包，无须临时生成或替换；其历史来源限制保留在第三方声明中。

## 查看计划与下载

需要系统提供 Python 3.10、curl 和 util-linux 的 flock。先运行只读计划：

```bash
python3 scripts/prepare_assets.py plan
```

默认 `runtime` 组包含全部模型和构建/运行依赖。可用 `--group deps`、`--group models` 分开准备；`--group converter` 仅取得可选转换工具 wheel。计划不联网、不扫描大文件、不创建目录。建议为首次下载、转换环境、解包和构建预留至少 10 GiB 可用磁盘；这不是运行时内存要求。

确认来源和所需空间后显式获取：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 scripts/prepare_assets.py fetch
```

脚本逐项下载、核对哈希并仅复制清单列出的归档普通文件。已有正确文件会复用；已有不同文件会报错并保留，不覆盖。下载中断留下 `.part`，再次运行可以续传；完整文件哈希不符时先检查来源与残留文件，不修改清单来掩盖问题。下载和解包拒绝路径越界、符号链接及归档中的链接/特殊文件。

首次获取结束出现三条 `CONVERSION_REQUIRED` 是预期结果：公开来源没有本项目冻结哈希对应的三份预转换 RKNN。此时已经取得原始 ONNX，继续下一节。程序不会把 ONNX 误当作 NPU 产物，也不会自动初始化模型。

## 在板端转换 Zipformer

此配方使用 ARM64 Ubuntu 22.04、Python 3.10、完整 RKNN Toolkit2 2.3.2。Toolkit Lite 不能替代转换工具。输入 ONNX 固定为 epoch-99-avg-1 / exp/96；batch=1，目标 rk3588，`do_quantization=False`。转换不需要另建 Windows 开发副本。

先取得固定 wheel，再建立一个尚不存在的项目内 Python 环境。安装过程会从 PyPI 获取版本锁列出的依赖，onnxoptimizer 0.3.8 可能需要本机编译；需要支持环境页列出的编译工具及 `python3-dev`、`python3-venv`。只使用该虚拟环境，不安装全局 Python 包。

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 scripts/prepare_assets.py fetch --group converter

test ! -e deps/model-prep/asr/venv-public
python3 -m venv deps/model-prep/asr/venv-public
deps/model-prep/asr/venv-public/bin/python -m pip install \
  pip==24.3.1 setuptools==75.1.0 wheel==0.44.0
deps/model-prep/asr/venv-public/bin/python -m pip install --only-binary=:all: \
  numpy==1.26.4 protobuf==3.20.3 typing_extensions==4.16.0 onnx==1.16.1
deps/model-prep/asr/venv-public/bin/python -m pip install --no-deps \
  deps/model-prep/asr/downloads/rknn_toolkit2-2.3.2-cp310-cp310-manylinux_2_17_aarch64.manylinux2014_aarch64.whl
CMAKE_BUILD_PARALLEL_LEVEL=2 MAX_JOBS=2 \
  deps/model-prep/asr/venv-public/bin/python -m pip install --no-build-isolation \
  -r scripts/assets/rknn-conversion-requirements.txt
deps/model-prep/asr/venv-public/bin/python -m pip check
```

以上代码块按顺序执行；如前一步失败，先修复再继续。先安装 NumPy、ONNX 及其固定依赖，再执行带源码构建的完整安装，避免依赖安装顺序受 pip 的构建阶段影响。onnxoptimizer 0.3.8 使用 CMake 并读取 MAX_JOBS，上面的双任务限制覆盖其原生编译。版本锁是转换环境记录，不涉及已固定的 C++ 推理 ORT：虚拟环境中的 Python onnxruntime 版本与推理库 1.17.1-patched 用途不同。

空闲板端执行转换，按 decoder、joiner、encoder 串行完成。输出目录和本地清单必须尚不存在；重试时选择新的名字，保留失败材料：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  timeout 1800 deps/model-prep/asr/venv-public/bin/python scripts/convert_zipformer.py \
  --output-dir models/asr/zipformer-local-01 \
  --candidate-manifest models/manifest.local.json
```

转换器先核对三个输入哈希，拒绝未知动态维度，再导出到新目录。结果中包含 `conversion-report.json` 和本地候选清单。每个输出记录新哈希，并标出是否与公开冻结产物逐字节相同。未证明 Toolkit 在不同机器/构建环境下字节确定性，因此不自动修改受版本管理的清单，也不把重转换产物标为 `verified`。

本地清单将 ASR 状态设为 `candidate`，改为指向新产物；其他固定模型、运行库要求保持不变。首次使用需要显式选择此清单并完成自己的文件推理与耳机验证。转换完成、哈希检查和配置检查均不代表识别质量或实机交互已通过。

`scripts/assets/zipformer-feature-config.json` 保持冻结参数文件的字节身份。其 `streaming_integration` 历史字段描述模型准备时的官方参考例程，不表示本项目当前未实现实时识别；运行适配器使用其中的特征数值与缓存契约。

## 构建与首次配置

库保留在 `deps/`，现有 CMake 的 RPATH 和部署打包器负责加载；不需要把运行库覆盖到 `/usr/lib`。仓库旧 `install_npu_runtimes.py` 针对原设备的一次特定迁移，含原路径和迁移前哈希，不是新板通用安装器。系统内核与 RKNPU 驱动应先满足支持环境页的固定组合。

按照[构建与运行](build-and-run.md#模型文件诊断)打开 ASR/TTS/LLM 三个开关。只运行默认构建不会得到完整模型链路。

已有与公开冻结哈希相同的资产可以直接验证：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 scripts/prepare_assets.py verify
```

使用自己转换的 ASR 时，默认 `verify` 仍检查公开冻结清单，不应当据它自动替换原哈希。先验证固定依赖，再使用配置入口检查所选本地模型清单：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 scripts/prepare_assets.py verify --group deps

test ! -e config/barge-in-local.yaml
cp -n config/wake.yaml config/barge-in-local.yaml
```

编辑刚复制的完整 YAML，按[音频诊断](audio-diagnostics.md)填写本机 `audio.source`、`audio.sink`、双向 `audio.profile`，将 `dialogue.barge_in` 改为 `true`。使用本地转换时设置：

```yaml
assets:
  models_manifest: ../models/manifest.local.json
  deps_manifest: ../deps/manifest.json
```

随后执行：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  build/model-probes/bin/voice_engine --config config/barge-in-local.yaml --check-config
```

预期看到 `CONFIG_OK` 和 `ASSETS_OK`。这一步只检查配置/资产身份。接着使用同一份本地配置完成首次 ASR 文件推理；不要换成仍指向冻结清单的 `config/file-probes.yaml`：

```bash
ASR_FIRST_RUN="first-asr-$(date +%Y%m%d-%H%M%S)"
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  timeout 180 build/model-probes/bin/asr_probe \
  --config config/barge-in-local.yaml --official \
  --output-dir output/runs --run-id "$ASR_FIRST_RUN"
```

每次运行使用新的 run ID；如果同名目录已存在，应换一个新 ID，工具会拒绝覆盖旧结果。`--official` 使用清单中的官方 WAV 和所选 ASR 角色路径，因此这里实际检查的是本地候选产物。退出 0 表示该工具检查通过，仍需查看识别结果并完成自己的耳机交互检查，不能据此宣称全部模型或真人交互验收通过。其他诊断选项见[模型文件诊断](build-and-run.md#模型文件诊断)。

确认自己的首次验证通过后再按[本地部署](build-and-run.md#本地部署与控制)安装服务。不要将 `config/barge-in-local.yaml` 或 `models/manifest.local.json` 提交；它们已由忽略规则覆盖。

## 清单边界和本次验证范围

运行清单不再要求历史测量脚本、本机编译的检查器、准备日志或私有 provenance 文件；它保留运行/构建资产、上游来源与许可证，并显式登记 fbank 构建依赖。原本机资料和已安装版本不因这次整理删除或改写。

准备工具的离线检查命令：

```bash
python3 tests/integration/asset_setup.py
```

该检查覆盖哈希错误、已有文件保护、路径/符号链接边界、归档白名单和候选清单语义，不使用网络、SDK 或真实模型。首次安装流程仍需在另一套干净环境执行才能称为外部复现验收；本页没有把本次文档与工具整理登记为新的实机验收。
