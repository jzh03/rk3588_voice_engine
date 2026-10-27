# 四模型共驻与顺序 NPU 调度

`coexist_probe` 对应 P2-04 / A06：在一个进程内保持 CPU Silero VAD、RKNN Zipformer ASR、RKLLM Qwen3.5-2B 和 CPU Melo TTS 常驻，重复执行模型轮次。ASR 完成后才启动 LLM；确认 LLM 返回、清 KV 并空闲后，才允许下一轮 ASR。CPU TTS 接收本轮 LLM 文本流，可以与 LLM 重叠。

输入复用固定语音 WAV，LLM 采用工具记录的独立固定短问题。它验证模块共驻和调度，不能代替实际识别文本驱动的 P3/A07；完整文件链路现由 [voice_file_demo](voice-file-demo.md) 单独验收。实时录放与软件取消由P4证据覆盖；P4已按确认范围结项，外部耳机尾音计时已从后续验收移除。工具不打开音频设备。

## 构建与配置

在板端项目根目录执行，保留现有构建目录和固定依赖：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake -S . -B build/model-probes -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DVOICE_ENGINE_BUILD_ASR_PROBE=ON \
  -DVOICE_ENGINE_BUILD_LLM_PROBE=ON -DVOICE_ENGINE_BUILD_TTS_PROBE=ON
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake --build build/model-probes -j2
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  ctest --test-dir build/model-probes --output-on-failure
```

三个模型探针开关同时开启时，额外生成 `build/model-probes/bin/coexist_probe`；默认构建不查询其私有 SDK。固定依赖继续使用已有 RPATH、manifest 和运行时检查。

专用 [config/coexist-probe.yaml](../config/coexist-probe.yaml) 同时启用四类模型，禁用 KWS；采用文件输入输出标签、TTS 4 线程和 LLM 64 token 输出预算。它不修改 `config/local.yaml` 的真实耳机设置，也不启用可选读音词典。队列、上下文及超时沿用共享配置的有界默认值。

## 命令与范围

```text
coexist_probe --config PATH --manifest FIXTURES_JSON --output-dir ROOT --run-id ID
  [--sample ID ...] [--duration-sec 300] [--min-rounds 5] [--warmup-rounds 5] [--smoke]
  [--swap-policy review|zero] [--validate-input-only]
```

正式模式要求时长至少 300 s 且至少完成 5 轮；单独达到其中一项不会结束。资产校验、模型加载和完整预热均在计时窗口之前。默认 `--warmup-rounds 5`（允许 1～20）按正式工作负载循环五个固定问题及自然语音输入，每轮均经过四个模型；所有预热时间和轮数从正式计数排除。正式窗口持续执行完整轮次，达到两个下限后收完当前轮，实际时长可能略大于指定值。

`--smoke` 允许 1 s、1 轮的短检查，但无论实际运行多久都记录 A06 为 `not_run`。`--validate-input-only` 仅校验参数、配置和 WAV/清单结构，不加载模型、不做大文件哈希，也不创建运行证据目录。目录已经存在时拒绝覆盖。

先做静态输入检查：

```bash
build/model-probes/bin/coexist_probe \
  --config config/coexist-probe.yaml \
  --manifest output/runs/p1-natural-review-20261022-104035/natural/manifest.json \
  --validate-input-only
```

真实运行使用 [coexist_validate.py](../tests/integration/coexist_validate.py) 监督整个进程组。输出目录必须是新目录，工具结果保存在其中的 `results/`。例如：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  python3 tests/integration/coexist_validate.py \
  --config config/coexist-probe.yaml \
  --manifest output/runs/p1-natural-review-20261022-104035/natural/manifest.json \
  --duration-sec 300 --min-rounds 5 --warmup-rounds 5 --swap-policy review --timeout 900 \
  --output output/runs/coexist-formal-example
```

外层锁覆盖监督器及完整子进程，不再嵌套获取同一把锁。编译、CTest、大文件校验同样遵守该锁；正式性能窗口不并行运行这些工作。监督器记录真实退出码，超时、内存跌破 1.5 GiB 或收到中断时终止受管进程组，不影响其他进程。

## 证据与通过条件

以 [A06 矩阵](ACCEPTANCE.md#2-用例矩阵) 为正式范围。没有 ABI/符号加载错误、崩溃或 OOM，实际模型库路径与冻结依赖一致，MemAvailable 始终不低于 1572864 KiB，队列有界且全部轮次完成。默认 `--swap-policy review` 完整保留 `pswpin/pswpout` 页计数、逐样本增量和累计增量，按原 A06 条件独立审查是否持续换页；单次活动不会提前终止测量。`--swap-policy zero` 保留更严格的诊断模式，任何测量期增量都以 6 退出；它不等同于计划中的“没有持续换页”。倒退的无效计数在两个模式都失败。

每秒记录队列深度与峰值、RSS/PSS（可读时）、MemAvailable、swap、线程/FD、CPU 占用、CPU/NPU 频率及温度。`LlmResult` 的 `queue_current_bytes/queue_current_packets` 与 `TtsStreamStats` 的 `current_input_bytes/current_segments/current_pcm_frames/segmenter_pending_bytes` 提供加锁只读快照；当前队列计数和累计峰值分别命名。生成函数正在持有的临时文本/音频另受原段长与临时音频上限约束。NPU 专用内存计数不可得时明确标注，RSS 不代表所有 NPU 内存。

监督层保存 `command.json`、`supervisor.json`、stdout/stderr、资源曲线和传感器来源；探针保存配置/资产/二进制标识、实际库映射、预热与测量时间点、逐轮结果、事件、队列/资源样本及汇总。预热记录包含请求/完成轮数、问题与输入覆盖数、逐轮结果及起止事件。正式退出成功时 `A06=pending_external_review`；验收需核对原始记录和监督器结果，不能仅凭退出 0 或某个汇总字段。

直接探针退出码：`0` 为本模式软件检查通过，`2` 参数/配置/资产/锁错误，`4` 模型或运行错误，`5` 超时，`6` 资源或验收条件不满足，`128+signal` 为中断。监督器在自身中断、整体超时或保护失败时返回 `5`，真实子进程退出码另存 `supervisor.json`；正常监督结束则转交子进程退出码。短测及静态检查不会关闭 A06。

本轮独立审核采用运行前固定的三态规则，详见[环境记录 §51](environment.md#51-p2-04-四模型共驻与五分钟验收)及私有测量计划。连续活动或密集活动判失败，周期性稀疏活动或尾段未稳定判待审；不得据退出 0 把证据不足写成通过。

五分钟共驻结果不替代 A14 的一小时稳定性检查；正式结果、限制与证据路径集中记录在 [环境记录](environment.md)。
