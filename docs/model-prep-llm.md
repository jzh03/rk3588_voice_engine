# Qwen3.5-2B 模型准备记录

日期：2026-10-20（Asia/Shanghai）。范围：Qwen3.5-2B、RK3588、W8A8、纯文本；证据目录 `output/runs/model-prep-llm-20261020-200730/`。此记录是准备与短测证据，不是 A04/A08 或完整语音链路验收。

后续结果：2026-10-21 已按新授权完成最小正常推理，见文末同名章节；以下 2026-10-20 准备及本日先前静态复查保持各自历史范围。

## 2026-10-20 准备阶段状态（历史）

- 已核验板端 RKNPU 0.9.8，以及 RKLLM 1.3.0 系统库和头文件与既有版本锁一致。未更换系统、内核或 runtime。
- 已取得并以 C++17/CMake/Ninja `-j2` 原生编译固定 1.3.0 官方 `rkllm_api_demo`，另编译专属插桩副本。公共项目源码及构建文件未变。
- 官方模型库入口在本轮浏览器和板端均无法访问；未从该库取得官方 2B 产物。
- 同一 Qwen3.5-2B 社区权重已完整下载并校验：**2,942,188,412 B，SHA-256 `946daf4377bbf5913d8d5226180991b8e6b4a3068d8b5959bdc81a9492311a09`**。实际结果与固定 revision 的 LFS 和发布模型卡一致；从 `.rkllm.part` 原子改名为 `models/llm/qwen3.5-2b/Qwen3.5-2B_RK3588_w8a8.rkllm`。
- 初次下载于 20:20:05 因 curl 56 / TLS unexpected EOF 中断，保留 1,487,530,008 B。用户随后明确授权自动续传，20:24:37 从该偏移恢复，313.16 s 后完成，curl=0；重试上限 5 次，间隔 5/10/20/40 s，本次恢复无需第二次尝试。证据 `model-download.json`、`model-download-attempts.json` 与 `model-download.log`。
- 用户最终明确要求“LLM 下载完不要进行推理验证”；因此 **LLM 初始化、中文问答、首 token/速度/峰值内存及主动取消均未执行，也未占用公共模型验证锁**。状态保持 candidate，下载或编译成功不代表推理通过。

## 来源与版本锁

| 资料 | 固定版本与结论 |
|---|---|
| 官方 RKLLM SDK/示例 | [airockchip/rknn-llm release-v1.3.0](https://github.com/airockchip/rknn-llm/tree/878f9361fd3afa7e167b7079918918f78d2c1c2a)，commit `878f9361fd3afa7e167b7079918918f78d2c1c2a` |
| 官方预转换入口 | 上述固定 README 指向 [rkllm_model_zoo](https://console.box.lenovo.com/l/l0tXb8)，公开提取码 `rkllm`。浏览器为 `ERR_CONNECTION_CLOSED`，板端 HTTPS 为 TLS EOF，HTTP 为 502；Filez 同路径尝试也为 TLS EOF。仅证明本轮连接失败，不证明官方库没有 2B 文件 |
| 社区预转换候选 | [HanzoHuang/Qwen3.5-2B-RKLLM](https://huggingface.co/HanzoHuang/Qwen3.5-2B-RKLLM/tree/888bdd28aa55fc77ce135ab3e1b0c6c395f5b108)，revision `888bdd28aa55fc77ce135ab3e1b0c6c395f5b108`；模型卡声明 Toolkit 1.3.0、RK3588 W8A8、Apache-2.0 |
| 目标权重 | `RK3588/Qwen3.5-2B_RK3588_w8a8.rkllm`；发布大小 2,942,188,412 B；模型卡与 LFS 记录的 SHA-256 均为 `946daf4377bbf5913d8d5226180991b8e6b4a3068d8b5959bdc81a9492311a09`。上述大小与哈希已实测一致，见 `model-download.json` |
| 官方 Qwen 配套资料 | [Qwen/Qwen3.5-2B](https://huggingface.co/Qwen/Qwen3.5-2B/tree/15852e8c16360a2fea060d615a32b45270f8a8fc)，本次读取的 metadata revision 为 `15852e8c16360a2fea060d615a32b45270f8a8fc`；仅下载许可证、模型卡、配置、聊天模板及权重索引等小文件 |

社区发布者**未披露实际转换所用原始权重 revision**，该值为 unknown；上表另取的 Qwen metadata revision 不代表社区转换输入版本。未取得完整转换命令、SDK 具体构建提交或校准张量哈希。发布的 `Qwen3.5-2B_data_quant.json` 包含 20 个发布者本地 sample 路径及 token 数；仓库未提供对应 embedding 张量，不能仅凭此 JSON 复现量化。二进制前 65,536 B 已留证且与完整下载文件的相同前缀一致，见 `header-integrity.json`；头部不是明文，尚未从官方 loader 取得内部生成版本；模型卡版本声明和文件名不能独立证明 runtime 兼容性。

同时读取了 t-firefly、Qengineering 两份同模型候选卡和文件元数据用于比较，未下载其权重。前者未写转换 toolkit 版本，后者声明 runtime 1.3.0，但没有补齐本轮关注的转换来源信息。当前只保留上述一份完整社区权重，不自动替换模型。

## 纯文本模板与结束符

`models/llm/qwen3.5-2b/source/` 保存官方参考资料，`provenance.json` 区分官方参考 revision 与未知转换输入 revision。Qwen 的非思考单用户模板为：

```text
<|im_start|>user
{中文问题}<|im_end|>
<|im_start|>assistant
<think>

</think>

```

私有示例调用 `rkllm_set_chat_template` 设置这一单轮格式，`RKLLMInput.enable_thinking=false`、`ignore_eos_token=false`、`keep_history=0`、总上下文 2048、输出上限 128。它只使用 `RKLLM_INPUT_PROMPT`；不下载或加载视觉编码器。

官方参考配置存在需要分别记录的两项：`text_config.eos_token_id=248044` 对应 `<|endoftext|>`；`tokenizer_config.eos_token=<|im_end|>` 对应 token ID 248046。另有 `<|im_start|>=248045`、`<think>=248068`、`</think>=248069`。后续需核对转换文件内置 EOS，并通过短问答的 `RKLLM_RUN_FINISH`、正常 `rkllm_run` 返回和回收验证实际停止行为；目前未验证。

## 官方示例与准备好的验证方法

原始上游文件位于 `deps/model-prep/llm/upstream-1.3.0/`，私有 C++17 构建入口为 `deps/model-prep/llm/native-demo/CMakeLists.txt`。生成 `build/llm_demo`（原始官方源文件）与 `build/llm_probe`（该源文件的专属插桩副本），差异见 `deps/model-prep/llm/native-probe.patch`。本次只调整私有构建，使用 `deps/rkllm-1.3.0/include/rkllm.h` 和同版 `lib/librkllmrt.so`；没有运行上游交叉编译脚本或其 `make -j4`。

已执行并通过：

```bash
cmake -S deps/model-prep/llm/native-demo -B deps/model-prep/llm/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build deps/model-prep/llm/build -j2
```

构建日志为本次证据目录下 `cmake-configure.log`、`cmake-build.log`。11 份已下载的官方/社区源码或说明逐项 Git blob SHA-1 与固定仓库元数据一致，见 `source-integrity.json`；交付资产另外记录 SHA-256。`binary-linkage.txt` 是静态 `ldd/readelf` 结果，解析到私有同版库；这不等于运行时实际加载证据。系统实路径 `/usr/lib/librkllmrt.so` 的 SHA-256 为 `6a9e4fc5324c68921c3a900340361e107af7599fe34dc8fa7759b2c5ae22a6e6`，头文件为 `80596a578f7f8e70df6eda1c2cbead3bfced14623a190258f2bd009a3d1f72cf`。

下列命令仅作为未来复现入口保存，**用户本轮明确要求不执行推理验证；本轮不会调用，后续需新的明确授权**：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock python3 deps/model-prep/llm/validate_llm.py normal
flock -x /tmp/rk3588_voice_engine_model_validation.lock python3 deps/model-prep/llm/validate_llm.py cancel
```

锁覆盖整个验证进程。`lock_guard.py` 比对设备号与 inode，并要求 owner 为当前进程或 `flock` 祖先进程，防止误用其他任务的锁；锁 owner、祖先关系随验证结果留证。用本次 run 内独立测试锁验证“无锁拒绝、祖先 flock 接受、flock -F 当前 owner 接受、其他进程锁拒绝”四项均通过，见 `lock-ownership-tests.json`，未初始化模型。程序限时 180 s，保留 stdout/stderr、初始化时长、首 token 回调及首个完整文本时刻、官方 perf 的 prefill/generate tokens 与耗时、`getrusage` 和 `/usr/bin/time` 的峰值 RSS、实际 `/proc/self/maps`、finish/error 回调、run/destroy 返回值。主动取消在输出到第 8 个 token 回调后请求 `rkllm_abort`，记录请求到 FINISH/run 返回的时差及 `rkllm_is_running`；不能只用 abort 返回成功判断取消完成。未安排长时间稳定性测试。

## 若需重新转换的条件与资源

固定 SDK 提供 Python 3.9/3.10/3.11/3.12 的 `linux_x86_64` Toolkit 1.3.0 wheel；板端为 AArch64，不能将该 wheel 当作板端原生转换环境。包依赖原文已保存，包含 torch 2.6.0、transformers 5.8.0 等。当前未安装任何训练/转换环境，也未部署 WSL、容器或其他电脑环境。

官方 `examples/multimodal_model_demo/export/export_rkllm.py` 提供 `load_huggingface(device=cpu)`、W8A8、`optimization_level=1`、`quantized_algorithm=normal`、`target_platform=rk3588`、`num_npu_core=3` 路线；脚本使用 `data/inputs.json`。其 `--savepath` 实际未被使用，输出按模型目录名写入 `./rkllm/`，实施时须核对这一行为。官方 README 的校准过程涉及图像 embedding，不应直接当作本项目文本专用校准集；文本语料、输入格式、许可证和哈希仍需另行冻结。

本次官方权重索引声明 tensor 数据合计 4,548,144,832 B（约 4.24 GiB），未下载原始权重。仅“原始权重 + 当前预转换文件”已约 6.98 GiB，另需 Python 环境、临时转换副本、校准数据、导出文件和至少 3 GiB 空闲余量。板端初始可用盘约 23.88 GB、可用 RAM 6.4 GiB；这些数字足以容纳一次候选下载，但不是转换可行性证明。转换工具内部临时内存与峰值未实测，不能将 4.24 GiB 权重大小视为转换 RAM 需求；如另获转换环境授权，应优先预留 16–32 GiB 主机 RAM并测量峰值（规划估计，非官方保证），同时保留至少一份原始权重和导出产物的磁盘空间。

清单片段 `output/runs/model-prep-llm-20261020-200730/models.fragment.json` 已登记 1 个 candidate：`qwen3.5-2b-rk3588-w8a8`，依赖 `rkllm-runtime=1.3.0`，所有 `files[].path` 按未来 `models/manifest.json` 所在目录计算，均为实际文件。模型卡声明 toolkit 1.3.0，二进制内部版本未验证；元数据扩展字段保留这一差别。`files.inventory.json` 记录本轮交付文件、源码、构建二进制和证据的大小及 SHA-256，编译中间文件和清单自身不计入。最终完整文件没有 `.part` 副本；初次中断部分文件的 SHA-256 已留在下载历史。

公共 README、计划、环境、兼容性摘要及两个公共 manifest 由主对话合并，本子任务未修改公共文件。未提交或推送 Git。剩余条件是后续明确授权下的内部版本/模板/EOS/正常结束/性能/取消实测，以及转换原始 revision、完整校准张量和生成过程来源补充；本次下载校验没有解决这些限制。

## 2026-10-21 来源与运行条件静态复查

按用户要求经现有 SSH 复查当前板端，未调用 `llm_demo`、`llm_probe` 或模型验证脚本，未初始化/推理 LLM。证据：`output/runs/llm-readiness-check-20261021-113001/readiness.json`，同目录保存 ELF 依赖解析及工具链日志。此处“准备齐全”仅指官方 C++ 最小示例的已知文件与运行环境条件，不代表模型实跑或实时引擎集成通过。

- 下载命令记录确认权重来自 Hugging Face `HanzoHuang/Qwen3.5-2B-RKLLM` 的固定 revision `888bdd28aa55fc77ce135ab3e1b0c6c395f5b108`，目标为 `RK3588/Qwen3.5-2B_RK3588_w8a8.rkllm`。它是社区预转换版本；官方 Qwen 小文件仅为配套参考。此前瑞芯微官方模型库连接失败是 2026-10-20 的历史结果，本次未重新检查该站点可访问性。
- 完整重新读取 2,942,188,412 B 权重计算 SHA-256，仍为 `946daf4377bbf5913d8d5226180991b8e6b4a3068d8b5959bdc81a9492311a09`。模型及 RKLLM/RKNN 依赖共 22 条文件记录的大小和哈希全部一致，无重新下载。
- 当前系统 Ubuntu 22.04.5 / aarch64，内核 6.1.99-rockchip-rk3588，RKNPU 0.9.8。当前 SSH 用户对 RKNPU 的 `/dev/dri/card1` 与 `/dev/dri/renderD129` 有读写权限；仅检查权限，没有打开设备或提交 NPU 工作。
- RKLLM 1.3.0 私有库、系统库及配套 `rkllm.h` 完整且哈希对应；RKNN 2.3.2 也保持锁定版本。两个已有 ARM64 示例和私有 runtime 的 `ldd -r` 均无缺库或未解析符号，示例解析到项目私有 `deps/rkllm-1.3.0/lib/librkllmrt.so`。这是 loader 依赖解析检查，尚无模型进程的实际加载/初始化结果。
- GCC 11.4.0、CMake 3.22.1、Ninja 1.10.1、glibc 2.35、Python 3、flock、timeout 和准备好的验证入口均在。原复查 JSON 的 time_program_exists=false，此前文字误列 GNU time；后续最小运行在 exec 前确认 /usr/bin/time 不存在并改用 /proc / getrusage 统计，未安装软件。已转换的 `.rkllm` 可由现有 C++ runtime 路线执行，执行它不以安装 x86_64 RKLLM Toolkit/PyTorch/Transformers 转换环境为前置。
- 复查采样 MemAvailable 7,518,536 KiB（约 7.17 GiB）、磁盘余量 18,537,807,872 B（约 17.26 GiB）；这只是当前空闲值，模型初始化峰值、KV cache、速度和共驻余量仍未测量。

结论：已知最小运行环境的静态前置条件通过，未发现缺失依赖。社区模型的内部生成版本、模板/EOS、实际初始化、正常结束、峰值内存和取消仍待用户明确允许后的验证；转换原始 revision 与完整校准材料仍缺。模型继续 candidate，`config/local.yaml` 中 LLM 仍禁用，实时引擎的 LLM adapter 尚未实现。本轮仅新增核查证据并同步文档，未更改资产、代码、配置、系统环境或 Git 暂存内容，未提交/推送。

## 2026-10-21 最小正常推理验证

本次用户明确授权启动子代理检查初始化、简短中文问答、正常结束及内存/速度；新授权仅解除本次最小正常推理的暂停。2026-10-20 下载阶段和本日先前静态复查“未初始化/未推理”的历史结论保持不变。未做主动取消、共驻、长稳、实时引擎或音频测试，模型继续 **candidate**。

证据：`output/runs/llm-minimal-20261021-113633/`；实际模型执行在 `normal-attempt-2/`，本地时间 11:39:26–11:39:33，模型进程及采样历时 7.582 s。只启动 **1 个实际模型进程、初始化 1 次、执行 3 条独立问答**。此前 `normal/` 因 `/usr/bin/time` 不存在，在 Popen 的 exec 前失败，未启动或初始化模型；原计划、输入与错误保留。新运行器直接启动探针，以 /proc、探针 getrusage 和父进程 RUSAGE_CHILDREN 统计，无安装包操作。

### 私有实现与运行命令

旧官方源码、`native-demo/llm_probe.cpp` 和 `validate_llm.py` 均未覆盖。本次新增 `deps/model-prep/llm/minimal-20261021/`，包含专用 C++17 探针、CMake 入口、Python 运行器和私有构建。新探针沿用固定 1.3.0 API 调用，增加逐条问题/原文/token ID/回调时刻、SDK perf、返回值及实际 maps，没有主动取消入口。初次编译因官方头文件未自行包含 size_t 所需头而失败，私有探针补 `<cstddef>` 后编译通过；未修改 SDK 头文件。失败、修正差异及成功日志均保留。

实际编译命令：

`cmake -S deps/model-prep/llm/minimal-20261021 -B deps/model-prep/llm/minimal-20261021/build -G Ninja -DCMAKE_BUILD_TYPE=Release`

`cmake --build deps/model-prep/llm/minimal-20261021/build -j2`

实际执行命令（板端工程根目录）：

`flock -x /tmp/rk3588_voice_engine_model_validation.lock timeout --signal=TERM --kill-after=10s 195s python3 deps/model-prep/llm/minimal-20261021/run_minimal.py output/runs/llm-minimal-20261021-113633`

运行器拒绝覆盖已存在的 `normal-attempt-2/`；再次验证须另建新的日期匹配证据目录，并确认授权范围。lock_evidence 记录锁 inode 与 flock → timeout → python3 祖先关系。锁覆盖初始化、问答、销毁、子进程回收和采样结束。内部模型期限 180 s，外部 195 s 为后备。每约 0.25 s 采样 RSS/PSS、MemAvailable 与 swap；可用内存连续 3 s 低于 1.5 GiB，或明确 OOM/驱动错误时停止并回收进程。本次保护未触发，无残留进程组成员，内核日志无新增条目。

固定参数：纯文本 RKLLM_INPUT_PROMPT、同步调用、输出最多 128、上下文 2048、keep_history=0、skip_special_token=true、ignore_eos_token=false、top_k=1、top_p=0.95、temperature=0.8、repeat_penalty=1.1，frequency/presence penalty 均为 0。embed_flash=1、base_domain_id=0，没有视觉编码器。实际 SDK 默认 enabled_cpus_num=4、mask=240，日志显示 CPU 4–7；本次未自行绑核、调频或修改系统配置。

rkllm_set_chat_template 返回 0。SDK 警告自定义模板会关闭内部自动模板解析（包括 enable_thinking 的自动处理），因此本次非思考格式由前文完整显式 `<think>\n\n</think>\n\n` 后缀提供，不能仅凭 enable_thinking=false 声称自动开关生效。三条回答未输出思考过程。

### 初始化、回答与速度

rkllm_init 返回 0，API 调用墙钟 **2998.914 ms**；SDK 另报内部 Model init time=1912.26 ms，二者口径不同。loader 实际报告 runtime **1.3.0**、RKNPU driver **0.9.8**、平台 **RK3588**；模型内部 Toolkit **1.3.0**、目标 **RK3588**、量化 **W8A8**、npu_core_num=3、max_context_limit=4096、Using mrope。本次实际请求上下文仍为 2048。

| 序号 | 固定问题 | 回答原文 | 首 token / 首可见文本 ms | SDK generate 阶段 tokens | SDK generate ms | SDK tokens/s |
|---|---|---|---:|---:|---:|---:|
| 1 | 请只用一句中文回答：中国的首都是哪里？ | 中国的首都是北京。 | 324.238 / 324.238 | 5 | 433.590 | 11.532 |
| 2 | 请只用一句中文回答：一加一等于几？ | 一加一等于二。 | 342.642 / 342.642 | 5 | 466.115 | 10.727 |
| 3 | 请用一句中文说明，为什么要节约用水？ | 节约用水是为了保障水资源可持续利用，确保未来世代也能享有清洁的饮用水。 | 351.505 / 351.505 | 16 | 1424.421 | 11.233 |

首 token 从每次 rkllm_run 调用前的单调时钟起，记录首个 NORMAL/WAITING 且 token_id>=0 的回调入口。首非空文本、首可见文本另记，本次三者相同，未见 WAITING 回调。速度严格按 `perf.generate_tokens × 1000 / perf.generate_time_ms` 计算，不用中文字数或回调数。包含终止标记的 NORMAL 回调数为 **6 / 6 / 17**，与 SDK generate 阶段 **5 / 5 / 16** 分开保留。prefill 为 23 / 23 / 20 tokens、320.303 / 339.426 / 348.193 ms。

第 1 条是新初始化进程的首轮，第 2、3 条复用驻留模型，每条不保留对话历史；未清系统文件页缓存、未专门暖机，不是严格冷盘启动或受控冷/热对照。仅 3 个不同问题，不计算有意义的 P95。

### 正常结束与 EOS 证据边界

三条最后一个 NORMAL 回调均为 token_id=248046、文本为空，随后恰好一次 RKLLM_RUN_FINISH；rkllm_run=0、rkllm_is_running=0、错误回调=0。run 返回耗时 756.757 / 808.515 / 1776.265 ms。最终 rkllm_destroy=0，耗时 **882.241 ms**，模型进程退出码 0。

本地官方参考 tokenizer_config.json 将 **248046 映射为 <|im_end|> 并声明为 eos_token**，因此确实观察到参考 tokenizer 的结束标记后正常结束，且远未达到上限 128，也未调用 abort。同时，RKLLM 1.3.0 公开结果结构没有 stop_reason 或内置 EOS 查询字段，参考 text_config.eos_token_id=248044 的差别仍存在。result.json 保守保留 eos_proven=false，表示**尚未独立核实转换文件内部 EOS 配置或显式停止原因**，不否认观察到 248046。不能把 FINISH 本身或 FINISH 回调 token_id=0 当作 EOS 证明。

### 资源、库与证据

- getrusage / RUSAGE_CHILDREN 峰值 RSS **2,195,032 KiB（2.093 GiB）**；SDK memory_usage_mb 最大 **2143.586**，头文件将其定义为 VmHWM，不是全部 NPU 内存。
- 28 个资源记录（含结束后采样）；smaps_rollup 采样峰值 RSS **2,195,576 KiB（2.094 GiB）**、PSS **2,192,761 KiB（2.091 GiB）**。不同 /proc 文件不是原子快照，计数口径和采样时刻不同，不要求与 ru_maxrss 完全一致。
- MemAvailable 初始 **7,515,808 KiB**，最低 **5,314,940 KiB（5.069 GiB）**，结束后 **7,511,668 KiB**；SwapUsed 与模型 VmSwap 全程采样为 **0**。进程 RSS/PSS 与系统可用内存不等于全部 NPU/驱动分配，不能据此承诺共驻余量。
- maps 实际加载 `/home/orangepi/projects/rk3588_voice_engine/deps/rkllm-1.3.0/lib/librkllmrt.so`，SHA-256 `6a9e4fc5324c68921c3a900340361e107af7599fe34dc8fa7759b2c5ae22a6e6`；未加载另一 RKLLM/RKNN runtime，未替换系统库。
- 模型是本日完整静态校验通过的同一文件（2,942,188,412 B，SHA-256 `946daf4377bbf5913d8d5226180991b8e6b4a3068d8b5959bdc81a9492311a09`）。采样期间未重复大文件哈希，未修改模型或 provenance 快照。

| 本次私有文件 | SHA-256 |
|---|---|
| llm_minimal_probe.cpp | e28a55e34bfaa44c3bde06b2a6b46bfa7d87c2d9f779ae4d819dba4ad180167f |
| build/llm_minimal_probe | 255c531db345f3e87d9e31808cb413f4db282827ba0b7b075e0d285d6ecc26ee |
| run_minimal.py | a230b9d3c925f0d10b7fa7b4002948c77d623bdb3d231770f25856d8b972e82f |
| CMakeLists.txt | 5e08bc603b9b85f65b992bd8d06454d3681006cad25436277ea1d35583520607 |

原始记录在 normal-attempt-2/：stdout.log、stderr.log、input.txt、plan.json、result.json、resources.jsonl、maps-*.txt、kernel-before.txt 与 kernel-after.txt。source-binary-inventory.json 为源码/二进制/库/头文件哈希，executed-source/ 保存实际执行源码，before/ 保存旧文件；build-attempt-1/、build-fix.patch、runner-time-fix.patch 保留修正。review.json 汇总证据解释和交付检查。没有提交、推送或修改 Git 暂存区。

结论：固定组合的 **初始化 → 3 条短中文问答 → 正常 FINISH → 销毁/退出最小流程通过**，内部 Toolkit/平台/量化已获 loader 实证。尚缺 A04/A08 完整次数及边界、主动取消、共驻、长稳、实时 LLM adapter，以及转换原始 revision、完整校准资料与内置 EOS 配置独立核实；模型不改为 verified。
