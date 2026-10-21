# Qwen3.5-2B 模型准备记录

日期：2026-10-20（Asia/Shanghai）。范围：Qwen3.5-2B、RK3588、W8A8、纯文本；证据目录 `output/runs/model-prep-llm-20261020-200730/`。此记录是准备与短测证据，不是 A04/A08 或完整语音链路验收。

当前结果：2026-10-21 已完成最小正常推理、固定十问/私有对照及[选定 R 的取消恢复收尾](#2026-10-21-r-配置取消恢复与-p0-收尾)。P0/M0 已通过，verified 仅限 SDK 最小基线；用户明确使用社区预转换模型，不再追查历史转换步骤，未知项保留。以下各阶段保留其原有历史范围。

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

## 2026-10-21 P0 十问、取消与短答复核

用户明确授权固定十问、一次主动取消及一次取消后恢复。主代理安排三个 Astra / xhigh 子代理，只有 LLM 子代理执行模型；SDK/资产重 I/O 结束后才进入采样窗口，期间没有并行编译或大文件校验。LLM 代码改动只在板端私有示例和证据目录，公共清单与文档由主代理合并；未实现正式 adapter、共驻、实时对话或长稳。

十问取 `tests/fixtures/llm_cases.json` 的 llm_01～llm_10，原文、顺序、预期和评分维度在运行前冻结，每题上限 64 tokens；ctx=2048、keep_history=0。原 fixture 顶层的禁执行标志属于 2026-10-20 准备快照，本轮授权仅覆盖本次范围，冻结原文件保持不变。取消专用长输出上限 128，恢复问答上限 64。未更换模型、SDK、量化、采样参数或非思考后缀。

首轮 `p0-llm-20261021-144257` 完成一次初始化、十问、一次取消、一次恢复及销毁，运行 83.517 s。十问均 run=0、FINISH 一次、idle=0、无错误回调。冻结四维评分为 **6/10**：03/04/05 没有交付完整答案，09 首答100厘米但又错误声称厘米是米的千分之一。03/04/05/07/08/10 有明显截断；07/08/10 的主答满足原四维评分，完整性缺陷另列，不能因此声称六条都可用。原 brief 维度主要是 SDK tokens≤64，数字约束本身不证明完整短答。

针对已观察的展开后截断，仅在新的私有副本增加通用短答 system ChatML：一句简短中文直接回答、不铺垫解释或 Markdown、指定数量在同句列出、无实时信息如实说明。没有放入题目答案，也没有改变十问、预算或评分。修正轮 `p0-llm-20261021-145026-short-answer` 只重跑同十问，运行 29.214 s，不重复取消/恢复。十条均以248046后FINISH结束且未用满64预算，无截断；评分 **9/10**，第01题对当前尚未接入的实时识别、朗读和播放音乐能力作出断言，仍记为失败。只作一次此类修正，没有再调题、加预算或挑选成功结果。

| ID | 修正轮回答原文 | 四维评分 |
|---|---|---|
| llm_01 | 我是离线中文语音助手，能实时识别并朗读文字、播放音乐及进行简单对话。 | 失败：未实现能力声明 |
| llm_02 | 离线语音助手是指无需联网即可独立运行并处理语音指令的智能设备。 | 通过 |
| llm_03 | 桌面整理只需将不常用物品收纳进抽屉或柜子，并定期清理杂物即可。 | 通过 |
| llm_04 | 出门前应该检查钥匙、手机和钱包这三样东西。 | 通过 |
| llm_05 | 做几个深呼吸，然后缓慢地伸展手臂、肩膀和颈部。 | 通过 |
| llm_06 | 一加一等于二。 | 通过 |
| llm_07 | 十分钟等于 600 秒。 | 通过 |
| llm_08 | 一年通常有 12 个月。 | 通过 |
| llm_09 | 一米等于一百厘米。 | 通过 |
| llm_10 | 无法提供实时天气信息，请查看当地气象网站或应用。 | 通过 |

评分原文、每项理由和两次完整输出均保留在各 run 的 report.json；修正轮的9/10不能换算为 P2 的18/20×3批通过。

### 取消、恢复与结束

首轮第11次 run 产生8个 token 回调且已有非空文本、is_running=1后，由独立控制线程请求 rkllm_abort；调用时不持有回调状态锁。abort 返回0，请求到abort调用/返回约11.623 ms，请求到run返回 **102.011 ms**、到idle确认 **102.016 ms**，满足本次模型2秒确认要求。请求后仍有1个尾token“时”，约101.845 ms后到达，不能宣称取消请求后零输出。API调用耗时记录为0μs只是低于记录分辨率，不是模型零时延停止。

确认空闲后显式 rkllm_clear_kv_cache 返回0，在**同一模型实例**恢复问答得到“恢复成功。”；旧标记未混入、stale callback计数为0。两次运行的destroy和进程均返回0，无残留，锁可重新取得。此证据仅为一次模型取消/清缓存恢复；没有测试 PCM 提交停止、耳机尾音、应用 new-session 或完整 A08 竞态，修正后的新system模板没有再次取消测试。

原始首轮 result.json 的 generation_limit_reached 由 SDK generate≥64 推导，六条64回调样本却报告 generate=63，因此该派生布尔值不能证明未触上限。原 JSON 保留，report.json 给出更正口径：观察到64个带token ID的NORMAL回调，与配置上限一致；没有SDK显式stop_reason。速度始终按SDK generate_tokens/generate_time计算，不用字符或回调数。正常结束标记与内部 EOS 配置仍分开说明。

### 性能、资源与证据

| 指标 | 首轮十问＋取消恢复 | 通用短答修正轮十问 |
|---|---:|---:|
| 初始化 API 墙钟 | 31.258 s | 3.124 s |
| 十问首 token min～max | 339.591～411.325 ms | 792.238～4953.233 ms |
| SDK generate token/s min～max | 10.742～11.338 | 6.863～11.290 |
| 采样峰值 RSS | 2,195,792 KiB | 2,195,636 KiB |
| 最低 MemAvailable | 5,308,332 KiB | 5,252,200 KiB |
| SwapUsed / VmSwap | 0 / 0 | 0 / 0 |

修正轮第05题TTFT为4.953 s，低速度值也保留。system提示增加了prefill长度，但不能将该异常值或两轮初始化差异全部归因于提示、缓存或调度；未作受控冷/热启动测试，未清页缓存，首次初始化期磁盘I/O增量未取得。两轮各有1条Wi-Fi扫描内核信息，没有新增NPU/OOM错误。RSS/PSS不是全部NPU/驱动内存。只报告这些短例的范围，不报告P95或长稳结论。

实际加载仍是私有 `deps/rkllm-1.3.0/lib/librkllmrt.so`（SHA-256 `6a9e4fc5324c68921c3a900340361e107af7599fe34dc8fa7759b2c5ae22a6e6`）；loader报告runtime1.3.0、driver0.9.8、Toolkit1.3.0、RK3588/W8A8。来源核查仍是相同 HanzoHuang 固定revision/权重哈希，未取得原始转换权重revision、精确命令/SDK构建、完整校准来源/张量/生成流程和哈希。当前参考源不等于实际转换输入。

本机私有目录分别是 `deps/model-prep/llm/p0-20261021-144257/` 与 `deps/model-prep/llm/p0-20261021-145026-short-answer/`；各自 CMake 原生构建。运行命令如下，仅为本轮实际执行记录；证据目录禁止覆盖，不能直接重跑同名run：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock timeout --signal=TERM --kill-after=10s 195s python3 deps/model-prep/llm/p0-20261021-144257/run_minimal.py output/runs/p0-llm-20261021-144257
flock -x /tmp/rk3588_voice_engine_model_validation.lock timeout --signal=TERM --kill-after=10s 195s python3 deps/model-prep/llm/p0-20261021-145026-short-answer/run_minimal.py output/runs/p0-llm-20261021-145026-short-answer
```

各run含 report.md/json、frozen-plan.json、commands.txt、实际源码/二进制哈希、stdout/stderr、逐token与资源记录、maps、锁和清理证据。主代理核对原始回调拼接、返回值、速度计算及取消时间；统一审计在 `output/runs/p0-closeout-20261021-144042/`。

**P0执行部分完成，但P0-04/M0未关闭，模型仍candidate。** 当前具体缺项是第01题的能力声明边界和转换溯源，不能冻结完整verified组合；完整A04/A08、正式adapter、共驻、实时和长稳仍按后续阶段要求保留。总状态见[环境记录 §41](environment.md#41-p0-剩余任务核查与十问取消验证)。

## 2026-10-21 私有提示、采样对照与 prefill 定位

本轮按用户授权由三个 Astra / Max 子代理完成私有例程、独立评分和遥测；接口使用 priority 服务层，没有独立 fast 开关。只有例程代理运行模型，公共记录由主代理合并。仍为同一 Qwen3.5-2B RK3588 W8A8、RKNPU 0.9.8 / RKNN 2.3.2 / RKLLM 1.3.0。

### 固定对照

私有源码：`deps/model-prep/llm/tuning-20261021-153941/runner/`；遥测为同级 `telemetry/`。证据：`output/runs/llm-tuning-20261021-153941/`。[protocol.json](../output/runs/llm-tuning-20261021-153941/grader/protocol.json) 在执行前冻结，SHA-256为 `adf564340e4c8ffe498c75ee31fa504dfb8ee3e7c916687210d5063d36a55edd`。沿用 llm_01～llm_10 原文、顺序、预期、四维评分和64-token预算，原fixture及历史失败保留。

五组各两批，顺序 L→S→K→R→P，再 P→R→K→S→L。每组每批独立进程/句柄，共 **10次初始化/释放、100条正式回答、10条固定预热**。每进程先用原 llm_01 预热一次，原文保存但排除正式统计；新进程不等于冷盘启动，未清系统页缓存。110次请求前清KV返回0、查询大小0，keep_history=0；每句柄只设置一次模板。

L保持上一轮长system；S仅将system正文替换为：

> 你是本地中文问答助手，只生成文字，不能联网、播放音乐或操作设备。用一句简短完整的话直接回答；不知道就说明，不展开解释。

ChatML包装和闭合 `<think>\n\n</think>\n\n` 后缀不变。自定义 `rkllm_set_chat_template` 接管模板，非思考格式由显式后缀提供，不能只凭enable_thinking=false认定SDK自动开关生效。S/L比较整个system替换，没有分别隔离长度与能力措辞。

S保持top_k=1、top_p=0.95、temperature=0.8、repeat_penalty=1.1、presence/frequency=0；K/R/P分别只改top_k=20、repeat_penalty=1.0、presence_penalty=2.0。全部ctx=2048、embed_flash=1、CPU4～7、三核NPU；完整显式参数见协议。固定SDK未提供seed，两批不是种子匹配试验。没有组合改多个采样参数，也未依据结果再次调提示。

两次私有C++17/CMake/Ninja编译均成功，编译和源码/小二进制校验在性能窗口前结束；模型/runtime大哈希复用当日有效证据并检查stat/身份未变。**15:48:08～15:52:06（UTC+8）** 串行推理及统一遥测，期间无并行编译或大文件校验。整个序列持同一flock，结束后重新取得锁。实际命令如下；已有证据不能覆盖，复现须按私有README新建run并先准备：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock timeout --signal=TERM --kill-after=10s 1950s python3 /home/orangepi/projects/rk3588_voice_engine/deps/model-prep/llm/tuning-20261021-153941/runner/run_tuning.py --run-series
```

### 内容和速度

表内各组为20条正式回答的合并中位数，包含内容失败；不是只挑成功样本，也不是P95。原四维评分、语义完整和额外无依据事实分别报告。

| 配置 | 两批原四维评分 | TTFT 中位 ms | 全部回答返回中位 ms | SDK 生成 tokens/s 中位 | 内容复核 |
|---|---|---:|---:|---:|---|
| L：原长提示 | 9/10；9/10 | 770.245 | 1935.878 | 11.649 | 两批均夸称音频和音乐能力 |
| S：精简能力提示 | 10/10；10/10 | 436.902 | 1785.972 | 11.462 | 两批均无依据自报知识截止2026年 |
| K：S＋top_k=20 | 6/10；10/10 | 437.198 | 1889.721 | 11.494 | 首批拒答、数量、算术及天气失败 |
| R：S＋repeat_penalty=1.0 | 10/10；10/10 | 437.489 | 1641.532 | 11.599 | 本固定集未见上述额外事实问题 |
| P：S＋presence_penalty=2.0 | 10/10；10/10 | 435.337 | 1776.863 | 11.590 | 两批均无依据自报知识截止2026年 |

全部100条已逐条阅读：原四维通过94条，语义完整99条，无可见截断。K首批llm_01虽然正常结束却拒绝能力介绍，未交付任务；“十分钟等于六十秒”和虚构北京天气均保留为失败。S/P共四条自报“我的数据截止时间是2026年”，没有来源依据；原题拒绝编造实时天气门槛通过不代表该额外事实可信，未事后修改评分。

**R仅作为本固定十问上的私有后续候选**：两批回答逐字一致、20/20按原四维通过，未出现上述截止年份问题。llm_01为“我是一名本地中文问答助手，能直接生成文字回答你的问题。”；llm_10为“无法提供实时天气信息，因为我的数据无法联网更新。”。没有写入公共配置，不能外推一般正确率或完整验收。

L→S输入86～90降至56～60tokens，TTFT中位770.245→436.903ms，降低约43.3%；同题同批配对TTFT差中位−335.399ms、全部回答耗时差中位−277.554ms。平均输出反而由12.4增至14.1tokens，精简提示不保证回答更短。各组生成速率中位约11.46～11.65tokens/s，未证明解码吞吐明显提高。R相对S的同题回答耗时差中位 **+1.335ms**；不能只看各组中位数不同就称repeat=1.0另外明显提速，选R主要基于本固定集内容。

### prefill 定位与资源

历史llm_05 TTFT=4953.233ms、SDK prefill=4948.303ms，差4.930ms，主要等待在SDK prefill计时段；同旧批、同87输入tokens的其他题约819～905ms，提示长度单独不足解释离群。旧日志缺少逐题绝对时钟与I/O/频率/调度证据，无法补出历史根因。

新100条TTFT最大861.472ms、SDK prefill最大857.257ms；未超过预先用于诊断的2秒标记线，这不是新的验收阈值。**历史4.95秒未复现，不能宣称已修复。** R第二批llm_09 prefill=681.308ms，其包围区间leader/worker线程等待分别增长109.742/81.608ms，缺页、全局磁盘读和iowait无增量，NPU1GHz；存在调度等待相关线索，不能相加线程等待当墙钟归因或回推旧异常。争用来源及关键路径仍未知。

遥测初期对sched_schedstats=0的推断已更正：本机CONFIG_SCHED_INFO=y，实际per-task schedstat第二字段仍有非零runqueue等待；固定Linux v6.1源码和原始两帧逐TID值在[调度更正](../output/runs/llm-tuning-20261021-153941/telemetry/scheduler-interpretation-correction.json)。未改原始采样器或日志。[遥测报告](../output/runs/llm-tuning-20261021-153941/telemetry/report.md)保留热策略、假设矩阵和来源。未追加flash0、改系统参数或长测。

10进程init=2.550～3.373s、destroy=0.875～0.903s，均退出0；maps仅加载项目私有 `deps/rkllm-1.3.0/lib/librkllmrt.so`，loader确认原组合/Toolkit/W8A8。模型自身getrusage及采样峰值RSS均最大2,195,548KiB（约2.094GiB），PSS采样峰2,192,909KiB；遥测最低MemAvailable5,273,624KiB（安全采样器为5,274,712KiB，时间点不同），swap0，无新增NPU/OOM错误或残留模型。RSS/PSS不是全部NPU/驱动内存。100正式回答均观察到248046后一次FINISH、run=0/idle=0，无预算命中；SDK仍不暴露显式stop_reason，内部EOS配置独立核实边界不变。

964次遥测，周期250ms、PSS每5s，采样线程CPU0～3；线程总CPU占单核4.414～5.059%，加权4.746%，读取中位8.806ms/最大65.495ms，无deadline overrun。所有配置同采样器，无无采样配对，不能称开销可忽略。PID I/O和PSI不可读，全局磁盘计数不能归因模型。SOC峰84.076°C、大核88.555°C；NPU/policy4在正式prefill保持1GHz/2.352GHz，policy6动态变化，缺运行期cooling-state记录，不能确认热限频。准备期policy0最低频从408MHz变为1.8GHz的来源未定位，collector未写sysfs，全部模型组初始配置相同；未自动回写频率。

### 交付与状态

[例程与命令](../output/runs/llm-tuning-20261021-153941/runner/report.md)、[全部逐题评分](../output/runs/llm-tuning-20261021-153941/grader/grading.md)、[遥测报告](../output/runs/llm-tuning-20261021-153941/telemetry/report.md)均已交付。主代理独立核对原始回调字节、参数、清KV、库路径和统计，见[执行审计](../output/runs/llm-tuning-20261021-153941/execution-audit.json)、[评分核查](../output/runs/llm-tuning-20261021-153941/grading-audit.json)、[最终差异审计](../output/runs/llm-tuning-20261021-153941/final-audit.json)。首次执行审计脚本对raw events与增加5个派生字段的result.events直接判等，已更正为原字段一致和派生值重算；失败留证，没有重跑模型。

本轮三项私有实验完成，历史prefill根因仍未确定。公共源码/CMake、local配置、原fixture/评分、模型/SDK、参考总结及历史证据保留，未实现正式adapter，没有提交/推送。P0-01/02仍完成，P0-03/04仍进行中，所有模型candidate、P0/M0未关闭：Qwen实际转换输入/recipe/校准embedding、Silero/Melo历史来源仍缺。两批十问不替代P2的20题×3批、多轮/超长/超时及完整A08；新候选模板未重做取消，仅复用原配置取消102.016ms及同句柄恢复证据。

## 2026-10-21 用户选定后续私有基线

用户确认按推荐的R候选继续。已将 `R_repeat1` 固定为后续私有验证基线，完整提示、采样/CPU参数、非思考后缀、64-token推理预算和原证据引用保存在 `deps/model-prep/llm/selected-candidate.json`，其SHA-256及选择记录进入models清单。参数与此前 `04-b1-R_repeat1.json` / 冻结协议逐值一致；这份选择记录不是可覆盖旧run的运行命令。

本次仅记录已确认的选择，没有重跑模型、改正式配置或把candidate提升为verified。P0-01/02完成，P0-03缺Qwen实际转换输入/命令/校准embedding、Silero历史导出和Melo权重/导出/字典来源；P0-04的最小推理执行已有证据，但完整可追溯组合仍未冻结。选定R配置尚需补一次取消及恢复，旧配置102.016ms的取消证据保持原范围。后续继续使用公共模型锁，性能采样期间避免并行编译或大文件校验，暂不扩展正式adapter。

## 2026-10-21 R 配置取消恢复与 P0 收尾

本轮由 Astra / Max 的执行、来源和独立复核三个子代理完成，主代理统一更新公共记录。模型、SDK、R 模板/采样、ctx 2048、初始化预算 128 / 请求预算 64、无思考/无历史和原四维评分均未改。历史选择文件 `deps/model-prep/llm/selected-candidate.json` 保持原 SHA；它的 candidate/cancel 未验证字段是当时快照，当前验证结论以清单和本节为准。固定十问两批 10/10 复用原始证据，不重跑十问。

只执行一次初始化、一次生成中取消及一次同句柄恢复，无预热或选择性重试。第 8 个带 token ID 回调且已有非空文本、SDK 仍 running 时，从控制线程调用 abort；实际调用早于 FINISH/run 返回。清 KV 成功且大小 90→0 后，恢复题“请只回答：恢复成功。”得到精确“恢复成功。”。两次 run 均一次 FINISH、返回 0 并确认空闲，destroy=0、进程=0，无 ERROR/过期回调、NPU/OOM 错误或残留进程。恢复末 token 248046 是观测，不等于 SDK 暴露了内部 EOS 原因。

| 本次指标 | 实测 |
|---|---:|
| 初始化 / 释放 | 2981.123 / 874.910 ms |
| 取消请求 → abort 调用 / 返回 | 0.198 / 0.198 ms；同 clock 读数不代表模型立即停止 |
| 取消请求 → run 返回 / 确认空闲 | 97.463 / 97.467 ms；通过原 2000 ms 条件 |
| 请求后尾 token | 1 个“的”，在请求后 97.272 ms 到达 |
| 恢复首 token / 完整回答 | 460.100 / 751.676 ms |
| 恢复 SDK 生成速度 | 3 tokens / 291.880 ms = 10.278 tokens/s；短句单样本 |
| 采样 RSS / PSS 峰值 | 2,195,960 / 2,192,952 KiB |
| 最低 MemAvailable / swap | 5,279,600 KiB / 0 |

资源按 250 ms 采样，可能漏峰且 RSS/PSS 不包含全部 NPU/驱动分配；不据此给 P95 或共驻保证。模型侧取消也不等于 PCM 停止/耳机静音。之前的历史失败、4.95 s prefill 未定因、完整 A04/A08 缺项保持原记录。

实际验证命令（在板端项目根目录执行，本次已经完成）：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock timeout --signal=TERM --kill-after=10s 195s python3 deps/model-prep/llm/p0-r-closeout-20261021-161053/runner/run_cancel.py --run-once
```

锁覆盖整个验证进程，性能窗口内未并行编译或大文件校验；完成后已确认锁可重新取得。私有例程在 `deps/model-prep/llm/p0-r-closeout-20261021-161053/runner/`；[命令和构建记录](../output/runs/p0-r-closeout-20261021-161053/runner/commands.txt)、[运行报告](../output/runs/p0-r-closeout-20261021-161053/runner/report.md)、[原始事件](../output/runs/p0-r-closeout-20261021-161053/runner/attempt-1/events.jsonl)、[独立审计](../output/runs/p0-r-closeout-20261021-161053/review/raw-event-audit.json)、[主代理重算](../output/runs/p0-r-closeout-20261021-161053/parent-execution-audit.json)均保留。

来源方面，社区固定 revision、权重大小/SHA、模型卡及 loader 的 1.3.0/RK3588/W8A8 证据成立。用户明确该文件为下载的预转换模型，不需要继续追查转换步骤；原始权重 revision、完整 recipe/校准/embedding 等未公开字段原样留 unknown，不再作为 P0 阻塞。核查终点及来源等级见[来源报告](../output/runs/p0-r-closeout-20261021-161053/sources/report.md)，没有用参考 metadata 或运行配置冒充实际转换输入。

P0-03/04 与 M0 已关闭，清单冻结首个限定 verified 组合；完整应用验收仍按原矩阵。总进度见[环境记录 §43](environment.md#43-p0-收尾与首个已验证组合)。
