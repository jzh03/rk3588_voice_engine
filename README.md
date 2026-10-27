# RK3588 本地语音助手

在香橙派5上离线完成“听懂问题 → 生成回答 → 耳机播报”的C++17语音引擎。识别、语言模型和语音合成都在板端运行，支持蓝牙耳机双向录放。

<a id="当前进度"></a>

## 能做什么

- 说“你好小派”唤醒，提问并听取回答。
- 回答结束后有15秒连续对话窗口，无声超时后重新等待唤醒。
- 播报时直接插话，停止旧回答并接续新问题。
- 支持取消、清空会话、耳机断线恢复、本地部署、版本回退和进程异常恢复。

## 日常使用

以下用于已经准备好模型、依赖、本机配置和已安装服务的开发板。先连接耳机，再通过现有SSH入口进入板端：

```powershell
ssh orangepi5
```

在板端工程根目录执行：

```bash
systemctl --user start rk3588-voice-engine
systemctl --user status rk3588-voice-engine
python3 scripts/deploy.py status
```

等待资产检查和模型加载完成后，说“你好小派”，停约2秒，再提问。对话窗口内可继续提问；播放中插话不需要重复唤醒词。

```bash
# 取消当前回答
python3 scripts/deploy.py send cancel

# 用完后停止
systemctl --user stop rk3588-voice-engine
```

服务按需手动启停。首次搭建、开发配置与部署命令见[构建与运行](docs/build-and-run.md)。

## 性能快照

2026-10-27 · Orange Pi 5 / RK3588S · 发布 `p7-m2-20261027-r2`。

| 验证项目 | 结果 |
|---|---|
| 自动化连续对话 | 10/10轮完成 |
| 耳机重连：空闲、录音、播放各1次 | 3/3项通过 |
| 重连后的响应样例：空闲 / 录音 / 播放 | 2.276 / 2.717 / 2.209秒 |

响应时间从识别结束计到音频后端接受首帧回答，采用软件时间戳。三条样例均提问“请说一句欢迎回来”；TTS为4线程、语速0.85。完整参数、计时口径和离线复算命令见[快照说明](docs/performance-snapshot.md)，数值对应[精简证据](docs/evidence/performance-snapshot-20261027.json)。

## 文档入口

| 内容 | 入口 |
|---|---|
| 编译、配置、CLI与资产清单 | [构建与运行](docs/build-and-run.md) |
| 耳机录放与设备排查 | [音频诊断](docs/audio-diagnostics.md) |
| 性能快照、参数与证据复算 | [快照说明](docs/performance-snapshot.md) |
| 公开文档和工具入口 | [文档索引](docs/README.md) |

模型权重与运行库按[资产清单](models/manifest.json)准备；仓库提供固定合成输入和相应工具。

## 项目目录

```text
src/、include/       引擎实现与接口
tools/               独立诊断工具
config/              配置模板；本机配置不随Git发布
scripts/、packaging/  部署脚本与用户服务模板
tests/               回归代码与固定合成输入
models/、deps/        资产身份清单；权重与运行库实体不随Git发布
docs/                使用说明、性能快照与官方参考资料
output/              本机运行记录，不随Git发布
AGENTS.md            协作和文件同步约定
```

日常开发通过Windows上的Codex与SSH操作板端工程；Windows目录仅作参考。

## 开发与旧章节入口

| 章节 | 位置 |
|---|---|
| <a id="基础环境检查"></a>基础环境检查 | [构建与依赖](docs/build-and-run.md#基础环境检查) |
| <a id="配置检查"></a>配置检查 | [配置字段](docs/build-and-run.md#配置检查) |
| <a id="资产清单"></a>资产清单 | [资产规则](docs/build-and-run.md#资产清单) |
| <a id="终端控制与-jsonl-日志"></a>终端控制与JSONL日志 | [控制入口](docs/build-and-run.md#终端控制与-jsonl-日志) |
| <a id="模型文件诊断"></a>模型文件诊断 | [文件工具](docs/build-and-run.md#模型文件诊断) |
| <a id="真实音频诊断"></a>真实音频诊断 | [耳机排查](docs/audio-diagnostics.md#真实音频诊断) |
| <a id="版本管理约定"></a>版本管理约定 | [版本管理](docs/build-and-run.md#版本管理约定) |
