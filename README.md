# RK3588 原生语音引擎

面向 Orange Pi 5（RK3588S）的本地语音助手。使用 Windows + SSH/SCP 开发，程序在开发板 Ubuntu 24.04 上原生编译。

## 当前进度

- 基础 C++ 编译工具和音频/YAML/JSON 开发库已安装。
- 最小 C++17 编译链接及运行检查已通过，输出 `ENV_CHECK_OK`。
- vivo TWS 4 蓝牙连接、同时录放基线及默认输入输出设置已完成。
- 已初始化 Git，分支为 `main`；[GitHub 私有仓库](https://github.com/jzh03/rk3588_voice_engine)已建立并完成首次推送。
- 语音引擎尚未实现；NPU 运行环境及模型推理尚未验收。

## 文档入口

- [执行计划](EXECUTION_PLAN.md)：任务依赖、里程碑与交付条件。
- [环境记录](docs/environment.md)：系统、依赖版本和音频设备设置。
- [SDK 兼容性](docs/compatibility.md)：驱动与 RKLLM 要求。
- [验收用例](docs/ACCEPTANCE.md)：功能、延迟、资源和恢复验证。
- [规划审阅记录](docs/PLAN_REVIEW.md)：问题和修订依据。

## 基础环境检查

在开发板仓库根目录执行：

```bash
mkdir -p build
g++ -std=c++17 -Wall -Wextra tools/env_check.cpp -o build/env_check \
  $(pkg-config --cflags --libs libpulse samplerate sndfile yaml-cpp)
./build/env_check
```

输出 `ENV_CHECK_OK` 表示该检查完成。它只验证编译和库调用，不打开录音、播放或 NPU。CMake/Ninja 的工程构建后续建立。

## 版本管理约定

模型、运行库、录音、构建产物、临时文件及本机配置通过 `.gitignore` 排除，模型和依赖只提交清单。基础环境检查源码保存在 `tools/env_check.cpp`。

Windows 与开发板采用同一仓库历史，通过 GitHub 同步；进入任一工作目录后先检查 `git status`，提交本机修改，再执行 `git pull --ff-only`。云端认证分别在各机器配置，不传输私人认证文件；开发板可以先通过 Windows 中转 Git bundle 同步，后续按实际需要配置直接推送。

