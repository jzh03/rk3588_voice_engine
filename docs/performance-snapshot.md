# 性能快照

本页对应2026-10-27的发布 `p7-m2-20261027-r2`，由已有记录整理，导出日期为2026-10-28（Asia/Shanghai）。结果见[项目首页](../README.md#性能快照)，机器可读数据见[精简证据](evidence/performance-snapshot-20261027.json)。

## 环境与参数

| 项目 | 测量条件 |
|---|---|
| 开发板 | Orange Pi 5 / RK3588S，aarch64 |
| 系统 | Ubuntu 22.04.5，内核6.1.99-rockchip-rk3588 |
| 推理依赖 | RKNPU 0.9.8、RKNN 2.3.2、RKLLM 1.3.0；Sherpa-ONNX 1.12.14、ORT 1.17.1-patched |
| 耳机 | vivo TWS 4，mSBC双向录放 |
| 引擎音频格式 | 16 kHz、单声道、20 ms帧 |
| TTS | 单实例4线程、语速0.85；分段min/target/max为12/12/40，lookahead为2，flush为300 ms，预缓冲1500 ms |
| 对话 | LLM最多输出128 token；“你好小派”唤醒，15秒连续窗口，支持插话 |
| 播放 | keepalive开启，lead-in为0 |

完整模型ID、线程、分段和对话参数保存在JSON的 `parameters` 中。发布二进制与配置以各自SHA-256标识；`repository_revision_at_export` 是导出时仓库版本，测量产物身份以 `engine_binary_sha256` 为准。

## 用例与计时

- **连续10轮**：既有语音输入经虚拟音频端点自动回放。每轮均有非空回答、播放完成和会话提交事件。
- **耳机重连3次**：分别在空闲、录音、播放时断开并接回，恢复后真人说“你好小派”，再问“请说一句欢迎回来”。三次均听到完整新回答；播放状态用例确认旧回答未续播。
- **三条响应样例**：取上述三次重连后的回答，使用同一会话和代次中的识别结束事件与首个有效回答帧。

```text
响应时间（ms）=
  (first_playback_submit.data.playback_submit_ns
   - asr_final.data.asr_final_ns) / 1,000,000
```

计时起点为识别结果产生，终点为引擎成功向音频后端提交第一帧回答，使用软件时间戳。每个数值对应表中的一次回答。

## 离线复算

在工程根目录运行：

```bash
python3 scripts/verify_performance_snapshot.py
```

脚本只读取公开JSON，检查事件顺序、会话一致性、完成计数并复算时间差，无需模型、开发板或耳机。应得到：

```text
Continuous turns: 10/10
Headset reconnects: 3/3
reconnect-idle: 2276.004663 ms (2.276 s)
reconnect-recording: 2717.233134 ms (2.717 s)
reconnect-playback: 2208.565375 ms (2.209 s)
```

证据包含事件序号、单调时间点、完成状态和来源文件SHA-256；真人用例提供固定提问文字。这里复算的是已记录的计时与计数，不执行语音推理或重放真人录音。
