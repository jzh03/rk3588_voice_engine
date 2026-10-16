# RK3588 原生语音引擎

面向 Orange Pi 5（RK3588S）的本地语音助手。开发板作为唯一的代码工作目录和 Git 提交端；Windows 通过 SSH 操作开发板，程序在板端 Ubuntu 24.04 上原生编译。

## 当前进度

- 基础 C++ 编译工具和音频/YAML/JSON 开发库已安装。
- 最小 C++17 编译链接及运行检查已通过，输出 `ENV_CHECK_OK`。
- vivo TWS 4 蓝牙连接、同时录放基线及默认输入输出设置已完成。
- 已初始化 Git，分支为 `main`；[GitHub 私有仓库](https://github.com/jzh03/rk3588_voice_engine)已建立并完成首次推送。
- 开发板已安装 GitHub CLI 2.45.0，使用账号 `jzh03` 完成 HTTPS 浏览器授权；用户已验证 fetch 和推送 dry-run 成功。
- 当前基础环境准备完成；下一步建立 CMake/Ninja 构建文件并验证构建。语音引擎、NPU 运行环境及模型推理尚未验收。

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

后续统一在开发板 `/home/jzh/projects/rk3588_voice_engine` 编辑、构建、提交和推送，GitHub 私有仓库保存远程版本。Windows 用作 SSH 终端；本地 `F:\AI\RK3588音频引擎` 在本次进度同步推送后保留作参考，不再作为日常修改或提交端，也无需持续双端同步。

板端使用 GitHub CLI 已登录的 `jzh03` 账号，通过 HTTPS 直接认证，不需要 Windows 中转 Git bundle。开始工作先检查 `git status`；工作区干净时执行 `git pull --ff-only`。完成修改后只暂存本次相关文件，再提交和推送。日常命令均在开发板执行，不使用 sudo：

```bash
cd ~/projects/rk3588_voice_engine
git status --short --branch
git pull --ff-only
# 编辑并验证后，按具体文件名执行 git add、git commit 和 git push
```

Git 是按命令运行的工具，不是需要关闭的后台服务；Windows 本地的 `.git` 和登录凭据可保留。本次 Windows 推送完成后，让开发板执行一次 `git pull --ff-only` 完成交接。后续文档也在板端仓库更新。


