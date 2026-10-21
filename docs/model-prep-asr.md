# Zipformer 模型准备与官方文件验证

记录日期：2026-10-20（Asia/Shanghai）。本轮唯一证据目录：`output/runs/model-prep-asr-20261020-200730/`。当前状态：源模型、配套资源、三份 FP16 RKNN 与原生 C++ 示例已备齐；在固定 runtime 上完成一次官方 WAV 推理，识别文本精确匹配。这里不代表主引擎已接入流式 ASR。

## 来源与固定版本

- [RKNN Model Zoo v2.3.2](https://github.com/airockchip/rknn_model_zoo/tree/bad6c7334531becaf90a561988519b7bec34d0ab/examples/zipformer)：tag 解析到 `bad6c7334531becaf90a561988519b7bec34d0ab`。该版本依赖表要求 RKNPU2 SDK >=2.3.2，release 没有二进制附件。32 份所需上游源码、头文件、静态库、WAV、词表和许可证逐个核对 Git blob 与 SHA-256，见 `upstream-files.json`。
- [原始模型](https://huggingface.co/csukuangfj/k2fsa-zipformer-bilingual-zh-en-t/tree/e2382758de9a0219b4efe682b95af30b399db3b8/exp/96)：固定 revision `e2382758de9a0219b4efe682b95af30b399db3b8`，同一 `exp/96` 下 epoch-99-avg-1 encoder / decoder / joiner 浮点 ONNX。模型卡声明 Apache-2.0。三文件均匹配 HF LFS 提供的 SHA-256。配套 HF tokens 与 Model Zoo vocab 的6257行逐行一致，仅文件换行格式不同；比对见 `vocab-comparison.json`。
- Model Zoo 的 Filez 下载地址在本板以 urllib 和 curl 均返回 SSL EOF。本轮回退到该例程明确指向的上游 HF 模型，**没有证明与不可访问的 Filez 副本逐字节相同**。本轮没有获得可追溯且版本匹配的预转换 RKNN 三件套，原始 ONNX 不能直接当成 NPU 模型。
- [RKNN Toolkit2 v2.3.2](https://github.com/airockchip/rknn-toolkit2/tree/42aa1d426c0a9e0869b6374edba009f7208a1926)：固定提交 `42aa1d426c0a9e0869b6374edba009f7208a1926`，CPython 3.10 / manylinux2014 aarch64 wheel 为 37,641,091 B，包内版本 2.3.2、build commit `c7d6ffcf`。wheel SHA-256 为 `73d0bb55d30ee00bbd9603331a4118da73a4e12fa9d9196200460d07d67c6a12`，同时核对了固定仓库中的 Git blob。官方用户手册 §2 明确 ARM64 支持 ONNX/PyTorch 转换，wheel 元数据列出 Ubuntu 22.04 / Python 3.10；不是 Toolkit Lite。
- runtime 使用已固定的 `deps/rknn-2.3.2/lib/librknnrt.so`（SHA-256 `d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8`），不替换系统库。运行内核/驱动为 `6.1.99-rockchip-rk3588` / RKNPU 0.9.8。

模型资产目录为 `models/asr/zipformer-bilingual-zh-en-epoch99-exp96/`，工具与私有示例目录为 `deps/model-prep/asr/`。源 ONNX 共 115,187,227 B：encoder 88,476,276 B、decoder 13,877,276 B、joiner 12,833,675 B。具体逐文件哈希以本轮 `models.fragment.json` 和 `provenance.json` 为准。

## 特征、词表与缓存契约

`feature-config.json` 和 `onnx-io.json` 记录源文件的完整参数、输入输出和 ONNX 元数据。源图中动态 batch 为 N；转换只取 N=1，保留原始 ONNX。

| 参数 | 固定值 |
|---|---|
| 输入 WAV | 16 kHz、单声道、PCM S16LE；89,784 帧，5.6115 秒 |
| 音频幅度 | 官方 `sf_readf_float` 的归一化 float32，不乘 32768 |
| fbank | 80 mel；25 ms 窗、10 ms 帧移；povey 窗；低频20 Hz、高频 Nyquist-400 Hz；dither=0；snip_edges=false；预加重0.97；去 DC；FFT 补到2次幂；log power |
| 块处理 | encoder 输入 [1,103,80]，每次前进96帧；输出24步，每步512维 |
| decoder | int64 token 历史 [1,2] → [1,512]；初始两个 blank=0 |
| joiner | 两路 [1,512] → [1,6254]；greedy search，blank=0、unk=2 |
| 词表 | 官方 `vocab.txt` 6257行；模型输出维6254，额外特殊条目保留；词首标志 `▁` 替换为空格 |

Encoder 共有36个输入和36个输出：1个特征张量及35个缓存。5个 stage 的层数均为2，左上下文 L 依次为192、96、48、24、96；N=1时缓存如下：

| 每个 stage 的缓存 | 形状与源类型 |
|---|---|
| cached_len | [2,1]，int64 |
| cached_avg | [2,1,256]，float32 |
| cached_key | [2,L,1,192]，float32 |
| cached_val / cached_val2 | 各 [2,L,1,96]，float32 |
| cached_conv1 / cached_conv2 | 各 [2,1,256,30]，float32 |

输出 `new_cached_*` 回传下一块对应输入。所有缓存初始置零；RKNN 运行时可能把4维输入报告为 NHWC，官方 C++ 根据查询到的格式进行缓存转置，不能直接照抄 ONNX 维序。文件尾按官方示例补零至足够一块。该示例先把整段 WAV 交给 `OnlineFbank`，再按块推理；麦克风增量、reset/cancel/finish_input 等主引擎接口尚未实现。

## 板端原生编译

在板端项目根目录执行：

```bash
cmake -S deps/model-prep/asr/native -B deps/model-prep/asr/native/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build deps/model-prep/asr/native/build -j2
```

本轮使用 GCC 11.4 / C++17 / Ninja 和系统 libsndfile 1.0.31，已编译成功。官方 kaldi-native-fbank aarch64 静态库不是 PIC；私有 CMake 添加 `-no-pie` 解决默认 PIE 链接失败。私有示例仅增加 SDK/driver 版本查询，并修正 main 在失败后仍返回0的退出码；原始源码保持在 `upstream/`，差异保留为本轮 `*.patch`。公共项目 CMake/源码未修改。`native-ldd.txt` 确认固定私有 runtime 解析路径。

## 转换环境与资源控制

完整 Toolkit2 装在 `deps/model-prep/asr/venv/`，正式依赖安装的 pip cache 与临时构建使用专属目录；没有全局 pip、WSL、容器或新增电脑转换环境。固定包的原始依赖表保存在 `arm64_requirements_cp310.txt`。其中 onnxoptimizer 0.3.8 无匹配 aarch64 wheel，须在私有目录以最多2个编译任务构建；其余依赖核对为 aarch64 或纯 Python wheel。实际版本锁与安装结果见本轮 pip 记录。

所查官方手册未给出本模型最低 host RAM。本板约7.7 GiB RAM，转换按单模型依次执行，并使用 `run_bounded.py` 监控自己创建的进程组：RSS 上限4 GiB、全机 MemAvailable 下限1.5 GiB、开始前至少2.5 GiB可用、每次最多1200秒。达到阈值只终止本次进程组，保留日志与结果；不能把这些保护阈值说成已证明的转换峰值。

所有模型加载、转换和推理必须持有 `/tmp/rk3588_voice_engine_model_validation.lock` 的独占 flock。纯下载、安装和 C++ 编译不占锁。`convert_zipformer.py` 固定 toolkit=2.3.2、target=rk3588、do_quantization=False；不使用 CPU 推理替代 NPU 验证。

## 本轮结果

已完成一次官方 C++ 文件推理，退出码0，未触发内存/超时阈值。实际识别文本与官方 README 完全一致：

> 对我做了介绍那么我想说的是大家如果对我的研究感兴趣呢

| 核对项 | 实测结果 |
|---|---|
| 实际加载 runtime | `/home/orangepi/projects/rk3588_voice_engine/deps/rknn-2.3.2/lib/librknnrt.so`；由运行中的 `/proc/<pid>/maps` 采集，SHA-256 与固定2.3.2一致 |
| SDK / driver | `2.3.2 (429f97ae6b@2025-04-09T09:09:27)` / `0.9.8` |
| 模型版本 | 同源 epoch-99-avg-1 / exp/96；源 ONNX metadata version=1；三份 RKNN 均嵌入 `2.3.2(compiler version: 2.3.2 (@2025-04-03T08:26:16))` 和 rk3588 标识 |
| encoder / decoder / joiner 初始化 | 328.605 / 129.326 / 114.774 ms |
| 推理段（含官方特征与解码循环） | 981.136 ms |
| 全进程墙钟 | 1.855205 s，含初始化/读取/释放与进程开销；由监测器采集，退出检测分辨率0.2秒 |
| 推理进程峰值 RSS | 160,232 KiB，约156.48 MiB；这是 RSS，不是全部 NPU/系统内存增量 |
| 原始 WAV 分母 RTF | 0.981135986 / 5.6115 = **0.17484**；全进程 / 原始 WAV = **0.33061** |
| 官方示例打印 RTF | `0.981 / 5.841 = 0.168`；5.841为示例打印的补零后时长，不能与原始5.6115秒混用 |

三份转换均成功，N=1 输入形状由脚本显式传入 `load_onnx`；未修改源 ONNX，FP16 浮点张量与 int64 缓存/token 类型由实际 C++ 查询确认。转换耗时及资源如下，RSS 为 `getrusage(RUSAGE_CHILDREN).ru_maxrss`，采样进程组数值也保存在 JSON 内：

| 转换模型 | 耗时 | 峰值 RSS | RKNN 大小 | RKNN SHA-256 |
|---|---:|---:|---:|---|
| encoder | 114.541 s | 1,766,000 KiB，约1.684 GiB | 113,933,362 B | `be438262794958868b2296537fca17c0bc8fa758d8e8da4ed6f0795f646e7fe5` |
| decoder | 3.111 s | 394,828 KiB | 8,017,769 B | `715c669b1cd05ab54b9185dcb6fec9c882fb1bc2be491f6838a5696be18d6dba` |
| joiner | 3.669 s | 376,344 KiB | 6,495,894 B | `1093b40f3f690bfc1a19d64310f860aae99fadac1c3d7dc59c8d2f379df18e29` |

encoder 转换期间最低 MemAvailable 为5,257,445,376 B，未触发保护。实际磁盘占用：私有转换工具/环境/缓存约1.5 GiB，模型目录233 MiB，证据目录59 MiB。保留30个安装归档（含官方 toolkit、源码包与本机编译 wheel），共269,480,753 B，精确哈希见 `wheelhouse/catalog.json`；没有升级系统 runtime。onnxoptimizer首次原生编译约10分钟，后续可复用已记录的 aarch64 wheel。

复现命令在板端项目根目录执行，选择新的运行目录避免覆盖本次证据：

```bash
# 当前 venv 已安装且 pip check / toolkit import 通过。
# 如需离线重新建立私有环境，目标路径必须尚不存在：
python3 deps/model-prep/asr/install_private_env.py /home/orangepi/projects/rk3588_voice_engine/deps/model-prep/asr/venv-repro

# 以下默认使用本轮固定的 deps/model-prep/asr/venv：
bash deps/model-prep/asr/convert_all.sh /home/orangepi/projects/rk3588_voice_engine/output/runs/model-prep-asr-YYYYMMDD-HHMMSS
bash deps/model-prep/asr/validate_file.sh /home/orangepi/projects/rk3588_voice_engine/output/runs/model-prep-asr-YYYYMMDD-HHMMSS
```

脚本内部用同一把独占 `flock` 覆盖完整模型进程，并将命令、退出码、耗时、内存和实际映射库写入对应 JSON。再次转换可能因编译时间元数据等产生新的 RKNN 哈希，必须重新生成资产清单并验证，不能复用本次产物的哈希。

关键证据：`provenance.json`、`pip-plan.json`、`pip-install.json`、`pip-freeze.txt`、`pip-check.txt`、`conversion-package-files.json`、`wheelhouse-catalog.json`、`native-build-fixed.log`、`native-ldd.txt`、`convert-{decoder,joiner,encoder}.{log,json}`、`inference.log`、`inference.json`、`result.json`、`rknn-tensor-io.txt`、`rknn-embedded-metadata.json` 和 `models.fragment.json`。

清单片段根为 schema_version=1，模型 id 为 `zipformer-bilingual-zh-en-rk3588-fp16`，文件路径按未来 `models/manifest.json` 所在目录计算，依赖 `rknn-runtime` 2.3.2。**状态保持 candidate**：本轮只验证1次官方中文文件，没有完成 A03 的更广泛验收、英文样本、增量麦克风接入、取消/reset、多模型同进程共存或长稳测试。模型准备与最小 ASR NPU 文件验证已完成；无遗留转换阻塞。
