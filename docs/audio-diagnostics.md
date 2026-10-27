# 耳机与音频诊断

本页提供耳机设备、录放和重连排查方法。按本机source/sink及profile配置运行；日常启动见[项目首页](../README.md#日常使用)。仅在需要排查时使用这些工具。

## 真实音频诊断

使用现有 `config/local.yaml` 的耳机参数，在开发板终端执行：

```bash
./build/bin/audio_probe --config config/local.yaml --duplex --duration-sec 10
```

`--duplex` 同时采集与播放，另可选择 `--capture-only` 或 `--playback-only`，三者必须且只能指定一个。默认运行 10 s，`--duration-sec` 接受 1～3600 s，从首次设备就绪时开始计时；`--wait-device-sec` 设置首次等待设备的上限，范围 1～60 s、默认 10 s。自动模式在设备首次就绪后开始录放。

若输出 `exit_code=3 reason=device_unavailable`，先在系统中连接耳机，确认蓝牙设备的 `Connected` 为 `yes`；已配对、已信任仍可能处于断开状态。再用 `pactl list short sources` 和 `pactl list short sinks` 核对配置中的两个节点均已出现、活动 profile 与配置一致，随后重试。具体等待原因记录在本次 `events.jsonl` 的 `device_wait` 事件中。

程序只连接配置中的 source/sink，并核验设备身份及当前 profile；禁止 monitor，不修改系统默认路由或 profile，不回退到 HDMI/其他声卡。即使只录音或只播放，也核验完整设备对。原生设备采样率/声道与内部 16 kHz、单声道、20 ms、归一化 float32 帧之间执行流式转换；最后不足一帧时补零并保留有效样本数。采集与播放使用配置中的有界队列，采集回调不推理或写磁盘。

播放默认每 5 s 发出一段 300 ms 的低音量提示音；也可用 `--play-wav FILE` 播放本机测试 WAV。默认采集仅在内存中处理，只有显式添加 `--record-wav` 才保存本次目录内的 `capture.wav`（16 kHz 单声道 PCM16）；交互模式的多次录音会连续写入同一文件。

交互诊断从空闲状态启动，便于检查重连：

```bash
./build/bin/audio_probe --config config/local.yaml --duplex \
  --duration-sec 60 --control stdin
```

| 命令 | 真实音频诊断行为 |
|---|---|
| `start` | 设备 READY 后启动新 generation，清空旧缓冲并开始所选模式 |
| `stop` | 停止采集和生产新播放帧，排空已提交音频后回到空闲 |
| `cancel` | 使旧 generation 失效并 flush 待播放音频 |
| `status` / `help` | 输出设备、格式、队列及计数 / 查看命令 |
| `quit` | 结束本次诊断；EOF 也正常结束 |

断线或 profile 变化后取消旧轮次、清空 PCM 和转换状态，按 1/2/5 s 退避重新查找；重建流后回到 READY，必须重新输入 `start`，不续播旧音频。非交互模式中途断线会使本次诊断失败，即使设备重连也不计为连续录放完成。

运行目录沿用上文的 `--run-id`、`--output-dir` 和不覆盖旧目录规则。`run.json` 保存配置/可执行文件哈希及参数；`events.jsonl` 保存设备属性、协商格式/缓冲、状态事件与每秒计数；`summary.json` 汇总有效样本、时长误差、RMS/峰值、队列峰值及溢出/断流/重连计数。成功返回 `0`，参数/配置错误 `2`，设备不可用或非交互模式中途断线 `3`，排空超时 `5`，软件计数异常或长录音时长误差超标 `6`，其他运行错误 `1`；信号退出沿用 `130/143`。

软件检查只报告程序观测，不能自动确认真人听感或物理路由。实际设备测试由使用者按需要安排，私人测试记录不随仓库提供。
