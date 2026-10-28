# 第三方来源与许可

本项目自有代码和文档采用根目录的 [MIT License](LICENSE)，版权署名为 jzh03。下列第三方内容保留自己的版权和许可；项目许可证不替代模型、SDK、系统库或其依赖的许可。

## 随源码保留的第三方内容

| 内容 | 来源及修改范围 | 许可与声明 |
|---|---|---|
| `src/asr/zipformer.cpp` | Rockchip RKNN Model Zoo，固定提交 `bad6c7334531becaf90a561988519b7bec34d0ab`；张量反馈、布局和贪心解码来自上游，本项目增加增量特征消费、RAII、接口约束与重置处理 | Apache-2.0；保留文件头的 Copyright (c) 2024 by Rockchip Electronics Co., Ltd. All Rights Reserved. 及修改说明，全文见 [LICENSE-Rockchip.txt](src/asr/LICENSE-Rockchip.txt) |
| 固定合成回归材料的生成器许可 | 使用 MeloTTS 生成；生成条件、模型身份与材料哈希见 [generator-lock.json](tests/fixtures/generator-lock.json) 和 [材料清单](tests/fixtures/manifest.json) | 保留 MyShell.ai 的 [MeloTTS-LICENSE](tests/fixtures/licenses/MeloTTS-LICENSE)；这些材料是合成回归输入，不是真人录音或自然语音质量评测集 |

Rockchip 上游：[RKNN Model Zoo 固定版本](https://github.com/airockchip/rknn_model_zoo/tree/bad6c7334531becaf90a561988519b7bec34d0ab)。

## 单独获取的模型与运行依赖

模型权重、运行库及本机配置不随本项目源码包分发。精确版本、下载来源、文件哈希及原始许可位置由[模型清单](models/manifest.json)、[依赖清单](deps/manifest.json)和[资产准备说明](docs/assets-setup.md)维护。

| 组件 | 清单登记的许可/来源说明 |
|---|---|
| Qwen3.5-2B RKLLM 转换模型 | Apache-2.0；同时保留原模型 LICENSE 与转换发布者模型说明 |
| Zipformer 双语模型 | 上游模型说明及 RKNN Model Zoo 标注 Apache-2.0 |
| Silero VAD v5 | MIT |
| MeloTTS 模型包 | MyShell.ai MIT；归一化 FST 的来源说明标注 Apache-2.0；字典保留 MIT 仓库参考。现有清单仍记录“精确历史字典来源未完全确认”，本次加入项目 MIT 许可不改变该状态 |
| Sherpa KWS 模型 | 随包模型说明标注 Apache-2.0 |
| RKNN Runtime、RKLLM Runtime | 分别以固定上游版本附带的 LICENSE 为准；不将项目 MIT 许可套用到 Rockchip SDK |
| Sherpa-ONNX、Kaldi Native Fbank | 保留各自上游版本的许可和声明；获取位置见资产准备说明 |
| ONNX Runtime 1.17.1-patched | 保留包内 MIT LICENSE 与 ThirdPartyNotices.txt；固定修订包不能仅以版本号相同的其他构建替代 |
| Ubuntu 系统库与工具 | 通过系统包管理器安装；各包的许可证及版权说明随发行版包提供 |

新增或更新第三方来源时，同步维护对应文件头、许可证副本、清单来源与此文档。准备可执行程序、模型或运行库分发包时，还需按实际包含的文件保留相应上游声明；当前发布流程仅生成项目源码包。
