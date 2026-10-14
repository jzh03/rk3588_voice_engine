# 开发环境与已完成基线

更新日期：2026-10-17（Asia/Shanghai）。系统与设备基线来自 2026-10-16 的 Windows 本机只读检查、现有 `ssh orangepi5` 会话的只读命令，以及用户明确确认；后续工程验证按下文日期记录。

## 1. 记录范围与证据边界

本文件保存系统、工具库和设备快照，以及 Git 交接与工程验证的历史证据。当前任务状态与后续动作统一见 [执行计划](../EXECUTION_PLAN.md#5-工作分解依赖与交付)，构建和使用方法见 [README](../README.md#基础环境检查)，SDK 官方前提见 [兼容性依据](compatibility.md)。

蓝牙连接和耳机同时录放基线由用户于 2026-10-16 明确确认完成，无需重复基础验证。用户未提供测试时长或测量文件，因此不补造“已连续测 10 分钟”等细节；后续 `audio_probe` 验收针对新程序，不替代或改写这条系统基线记录。

## 2. 开发板实况

| 项目 | 本轮结果 | 来源/边界 |
|---|---|---|
| 型号 | Orange Pi 5 | `/proc/device-tree/model` |
| SoC 兼容标识 | `rockchip,rk3588s-orangepi-5`、`rockchip,rk3588` | SDK 转换目标仍按例程使用 `rk3588` |
| 系统 | Ubuntu 24.04.1 LTS，Noble | `/etc/os-release` |
| 内核/架构 | `6.1.0-1025-rockchip` / aarch64 | `uname -a` |
| 登录 | `jzh`，uid/gid 1000；SSH alias `orangepi5` → `192.168.137.2:22` | 现有配置连接成功 |
| 用户组 | jzh、adm、cdrom、sudo、dip、video、plugdev、users、lpadmin、sambashare | `id`；未列出 render 组 |
| 内存 | 总 8,307,392,512 B（约 7.74 GiB）；本次空闲状态 MemAvailable 7,052,058,624 B（约 6.57 GiB） | `free -b`，资源会变化 |
| swap | 1,073,737,728 B，当前使用 0 B | 不能据此预测模型峰值 |
| 根分区 | `/dev/mmcblk1p1`；可用 20,468,256,768 B（约 19.06 GiB）；占用率 31% | `/` 与 `/home` 在同一文件系统 |
| NPU 驱动 | `0.9.7` | `/sys/module/rknpu/version` |
| NPU 设备 | `renderD129` → `/sys/devices/platform/fdab0000.npu`，驱动 RKNPU | 不能把显示用 `renderD128` 当作 NPU |
| NPU 访问 | 当前 jzh 对 NPU render 节点可读写 | `os.access`，未实际初始化模型；权限可能来自会话 ACL |
| NPU governor | `rknpu_ondemand`；读数时频率 1,000,000,000 Hz | 未改频率；不代表持续频率 |
| 温度 | 各 thermal zone 约 38.8～40.7 °C | 空闲快照，不是负载/散热验收 |
| 用户运行目录 | `/run/user/1000` | SSH 中已有 XDG_RUNTIME_DIR |
| 用户 D-Bus | `unix:path=/run/user/1000/bus` | SSH 中可见 |
| 用户会话 | State=active，Linger=no | 无桌面登录的自启动尚未测试 |

初次 SSH 盘点缺少开发依赖；此后用户已执行基础依赖安装，并提供以下版本查询结果：

| 工具/库 | 版本 | 证据 |
|---|---|---|
| G++ | 13.3.0（Ubuntu 13.3.0-6ubuntu2~24.04.1） | `g++ --version` |
| CMake | 3.28.3 | `cmake --version` |
| Ninja | 1.11.1 | `ninja --version` |
| libpulse | 16.1 | `pkg-config --modversion libpulse` |
| libsamplerate | 0.2.2 | `pkg-config --modversion samplerate` |
| libsndfile | 1.2.2 | `pkg-config --modversion sndfile` |
| yaml-cpp | 0.8.0 | `pkg-config --modversion yaml-cpp` |
| nlohmann-json3-dev | 版本未单独查询；头文件可用 | 编译成功且 JSON 序列化输出正确 |

基础开发依赖清单：`build-essential cmake ninja-build pkg-config libpulse-dev pulseaudio-utils libsamplerate0-dev libsndfile1-dev libyaml-cpp-dev nlohmann-json3-dev`。

`pactl info` 成功连接 `/run/user/1000/pulse/native`，服务名称为 `PulseAudio (on PipeWire 1.0.5)`。上述查询证明工具可用、包元数据可发现，C++17 与依赖实际编译链接已由后续检查通过。安装前 apt 提示下载 95.2 MB、新增 262 MB、升级 43 包、新装 66 包；这不是安装后实测磁盘占用。

`ldconfig -p` 可见 `libpulse.so.0`、`libpulse-simple.so.0` 和 `libsamplerate.so.0`。常规系统库位置及 ldconfig 缓存未发现 `librknnrt`、`librkllmrt`；这不等于扫描过整块磁盘，不排除其他路径有用户保存的副本。NPU 驱动与 SDK 要求的比较见 [兼容性依据](compatibility.md)。早期检查程序的目录迁移见 §7，Git 初始化与交接快照见 §8。

## 3. 耳机当前配置与历史音频快照

PipeWire、pipewire-pulse、WirePlumber 用户服务均为 active；PipeWire 和 pipewire-pulse 包版本为 `1.0.5-1ubuntu1`；BlueZ 包版本 `5.72-0ubuntu5`。PulseAudio 服务包未安装，libpulse 客户端可通过 PipeWire 兼容服务接入。

初次检查时（用户后来已接回耳机），`wpctl status` 仅枚举 DP0、ES8388 和 HDMI0；ES8388 为当时活动默认输出。配置中仍有以前的 `bluez_output.…` 名称，但没有对应蓝牙运行节点。`systemctl is-active bluetooth` 当时返回 inactive，BlueZ 对象树没有适配器对象，rfkill 列表为空，USB 枚举没有明确的蓝牙设备。由此只能判断**本次快照未枚举到可用蓝牙音频设备**，不能据此否定用户之前已经完成的连接和录放测试。

`bluetoothctl show/devices` 的初次非交互查询超时，后改用有界 D-Bus、服务和 PipeWire 枚举取得快照。没有据此重配设备、改 profile 或启动录音。

原始精简结果：[board-audio-2026-10-16.json](evidence/environment/board-audio-2026-10-16.json)。该文件记录当时的状态，不是录放验收日志。

用户后续提供的 vivo TWS 4 配置（2026-10-16）：

| 字段 | 已确认值 |
|---|---|
| 设备名称 | `vivo TWS 4` |
| 蓝牙地址 | `1C:7A:43:7F:83:8F` |
| card | `bluez_card.1C_7A_43_7F_83_8F` |
| 默认输出 sink | `bluez_output.1C_7A_43_7F_83_8F.1` |
| 默认输入 source | `bluez_input.1C_7A_43_7F_83_8F.0`；非 monitor |
| 活动 profile / codec | `headset-head-unit-msbc` / mSBC |
| 输入、输出节点格式 | `s16le 1ch 16000Hz`（16 位小端 PCM、单声道、16 kHz） |
| 默认设备设置 | 用户已执行 set-default-sink / set-default-source，并确认两项 get-default 查询与上述名称一致 |
| 应用缓冲、实际延迟、重连后的名称/默认值保持 | 尚未测量，P1/P4/P7 验收 |

证据为用户粘贴的设备列表和本轮设置完成确认；不是本轮再次 SSH 实测。列表中的 `api.bluez5.connection=disconnected`、`bluez5.profile=off` 与活动 profile/节点共存，保留这一快照差异，不据此撤销用户已完成的基线。当前默认输入已由先前的 ES8388 monitor 改为上述耳机麦克风。程序仍应显式绑定设备，不依赖全局默认；设置成功不等于重连/重启持久性已验证。

## 4. 电脑与模型准备方式

以下为 Windows 电脑的资源快照。用户明确不使用 WSL；模型准备方式与转换条件见 [执行计划](../EXECUTION_PLAN.md#7-工程资产与依赖管理)。

| 项目 | 本轮结果 | 规划影响 |
|---|---|---|
| 系统 | Windows 11 家庭版中文版，`10.0.26300` | 沿用 Windows + SSH 工作方式 |
| RAM | 16,889,036,800 B（约 15.73 GiB），16 逻辑处理器 | 不以总内存保证 2B 模型转换成功 |
| 空闲内存 | 3,255,272 KiB（约 3.10 GiB） | 快照；转换前重新检查实际余量 |
| C 盘空闲 | 26,291,716,096 B（约 24.49 GiB） | 模型下载缓存优先放 F 盘 |
| F 盘空闲 | 575,062,822,912 B（约 535.57 GiB） | 可保存模型下载和校验文件 |
| 其他 Linux 主机/GPU | 未核实 | 不假设已具备可用转换服务器或 CUDA |

原始资源结果：[pc-resources-2026-10-16.json](evidence/environment/pc-resources-2026-10-16.json)。仅说明当前资源，不代表自行转换模型的环境或工具已经具备。

## 5. 复查命令

Windows 终端可使用现有 SSH 配置运行下列只读命令；无需关闭主机密钥校验：

```powershell
ssh orangepi5 'id; uname -a; cat /etc/os-release'
ssh orangepi5 'cat /sys/module/rknpu/version; free -b; df -B1 / /home'
ssh orangepi5 'systemctl --user is-active pipewire pipewire-pulse wireplumber'
ssh orangepi5 'wpctl status'
ssh orangepi5 'systemctl is-active bluetooth; busctl --system --timeout=3 tree org.bluez --list'
ssh orangepi5 'loginctl show-user jzh -p State -p Linger -p RuntimePath'
```

`pactl` 已安装，后续可用以下命令读取配置：

```bash
pactl info
pactl list short sinks
pactl list short sources
pactl list cards
```

以上仅用于读取配置；不把当时设备不在场当作已完成基线失败。基础系统命令的成功也不等于模型或新程序通过验收。

## 6. 尚未验证的事项

本文件的空闲资源、权限和系统录放快照不能作为模型推理、应用录放、同进程共存、实际延迟或部署恢复的验收结果。对应任务与完成条件统一维护在 [执行计划](../EXECUTION_PLAN.md#5-工作分解依赖与交付)，测量规则见 [验收文档](ACCEPTANCE.md)。

原计划中的下载代理 `192.168.137.1:17897` 本轮未验证可用性，首次下载前需检查。

## 7. C++17 与基础依赖检查通过

2026-10-16，用户在 `/home/jzh/projects/rk3588_voice_engine/env-check-Ca991G` 执行：

```bash
g++ -std=c++17 -Wall -Wextra main.cpp -o env_check \
  $(pkg-config --cflags --libs libpulse samplerate sndfile yaml-cpp) \
  && ./env_check
```

用户提供的结果：

```text
libpulse: 16.1.0
samplerate: libsamplerate-0.2.2 (c) 2002-2008 Erik de Castro Lopo
sndfile: libsndfile-1.2.2
{"sample_rate":16000}
ENV_CHECK_OK
```

结论：该最小程序的 C++17 编译、libpulse/libsamplerate/libsndfile/yaml-cpp 链接和运行、YAML 解析及 JSON 序列化通过。未打开音频流，未测试 NPU；当时尚未验证 CMake/Ninja 工程构建，后续验证记录见 §9。

2026-10-16 目录整理：原 `env-check-Ca991G/` 已整体移入 `tmp/env-check-Ca991G/`，保留其中的源码和历史可执行文件。此前已纳入版本管理的 `tools/env_check.cpp` 与临时目录中的 `main.cpp` 内容相同；整理时曾直接编译到 `build/env_check` 并输出 `ENV_CHECK_OK`。后续统一使用该源码，通过 CMake 构建到 `build/bin/env_check`；当前复现命令见 [README](../README.md#基础环境检查)。上方保留用户当时执行检查的原路径和命令。

## 8. Git 与云端托管

以下为 2026-10-16 的初始化、认证与交接记录；日常操作统一见 [README 版本管理约定](../README.md#版本管理约定)。

| 记录项 | 当时确认值 |
|---|---|
| 云端仓库 | [jzh03/rk3588_voice_engine](https://github.com/jzh03/rk3588_voice_engine)，创建时为 private |
| origin / 分支 | `https://github.com/jzh03/rk3588_voice_engine.git` / `main` |
| 项目提交身份 | `jzh03 <1053442572@qq.com>`，通过板端 `git config --local` 配置 |
| 唯一日常工作目录 | `/home/jzh/projects/rk3588_voice_engine`；代码、文档及 Git 操作均在板端完成 |
| Windows 参考副本 | `F:\AI\RK3588音频引擎`；交接后仅作参考，不维护双端写入 |

历史时间线：

1. **板端初始化。**用户执行 `git init -b main` 并建立 `.gitignore`；云端配置前的 SSH 复核为尚无提交和远程地址。这是初始化时的状态。
2. **首次推送。**Windows 规划文件与板端 `tools/env_check.cpp` 合为首次提交，排除模型、编译产物、临时检查目录和临时图片；13 个文件组成提交 `9aea28a0d31d810eceb950c1cb8a3a554b84f643`。Windows 使用已登录对应账号的 Git Credential Manager 推送，`git ls-remote` 核对 main 一致；板端最初通过 Git bundle 导入同一历史。
3. **板端认证。**安装 GitHub CLI 2.45.0（Ubuntu 2.45.0-1ubuntu0.3），使用 `gh auth login --hostname github.com --git-protocol https --web` 和 `gh auth setup-git`。SSH 只读复核中，`gh api user --jq .login` 返回 `jzh03`，凭据助手为 `!/usr/bin/gh auth git-credential`；未读取或记录令牌。用户确认 fetch 成功，`git push --dry-run origin main` 显示 `Everything up-to-date`。
4. **工作目录交接。**交接前 Windows 与板端 main 均为 `b345e545080d023289bdf41e9dfee3ae884ef96d`，工作区均干净；板端 main 跟踪 origin/main。约定由 Windows 最后推送认证及进度说明，随后板端 fast-forward 拉取完成交接，之后由板端直接通过 HTTPS 拉取/推送。该时点尚未开始 P1-01，后续构建验证见 §9。

## 9. CMake 构建与首批音频配置校验

2026-10-16，用户已建立 CMake/Ninja 构建入口，并确认 `build/bin/env_check` 输出 `ENV_CHECK_OK`；随后保存 `config/default.yaml` 和被 Git 忽略的 `config/local.yaml`。当前配置命令、字段约束与退出码见 [README 配置检查](../README.md#配置检查)。

本轮以 Release 配置执行 CMake/Ninja 构建和 `ctest --test-dir build --output-on-failure -V`，结果为 `All 56 config checks passed`；环境检查输出 `ENV_CHECK_OK`。实际 `config/local.yaml` 检查输出 `CONFIG_OK`、退出 `0`；设备字段留空的默认模板提示 `audio.source` 错误、退出 `2`。运行记录保存在本机被忽略的 `output/runs/config-check-2026-10-16/{run.json,summary.json}`。

2026-10-17 继续完成配置类型校验：修复超出数值范围的数字被当作设备字符串接受的问题，并补充未加引号与显式字符串的回归用例。Release 构建成功，全部 `62` 项配置检查通过；环境检查、本机配置和空设备模板的退出结果与上述约定一致。新增运行记录保存在 `output/runs/config-check-2026-10-17/{run.json,summary.json}`，其中包含被验证源码和配置的 SHA-256。

上述结果仅为配置检查证据，不含在线设备、模型资产、录放或 NPU 验证。该时点尚未验证 JSONL 和 mock 控制，后续记录见 §10。

## 10. JSONL 日志与 mock 终端控制

2026-10-17，完成 JSONL 日志与无硬件 mock 控制验证。启动方式、命令行为与日志格式统一见 [README 终端控制与 JSONL 日志](../README.md#终端控制与-jsonl-日志)。

Release 构建与三组 CTest 均通过：62 项配置检查、日志单元测试（`LOGGER_TEST_OK`）和 39 项控制检查。覆盖输入长度限制、取消与会话代次、EOF、SIGINT/SIGTERM 收尾、轮转上限、已有目录保护和日志 I/O 失败后拒绝继续写入。实际本机配置检查仍输出 `CONFIG_OK`。

使用本机配置执行 `status → start → stop → start → cancel → new-session → quit` 演示，25 行事件均可解析，最终会话编号为 2、generation 为 4，正常退出。日志及验证脚本保存的报告位于 `output/runs/p1-01-control-2026-10-17/{events.jsonl,ctest.log,verification.json}`；报告记录了本轮源码与配置 SHA-256。

这些结果不计为应用录放、模型取消或 A01～A14 实机验收通过；当前任务状态及剩余项见 [执行计划](../EXECUTION_PLAN.md#5-工作分解依赖与交付)。

## 11. P1-01 完整工程骨架验证

2026-10-14（本次会话提供的日期，Asia/Shanghai），完成 P1-01 的完整配置与资产检查、音频接口/mock 和独立构建验证。原有历史记录的日期保留。当前用法与契约集中在 [README](../README.md#基础环境检查)，回归覆盖要求见 [验收文档](ACCEPTANCE.md#6-当前命令与后续约定)。

Release 配置和 `cmake --build build -j2` 成功。分两次运行 CTest：先执行除 `build_boundaries` 外的回归，再执行该独立构建检查，7 组全部通过：

| 检查 | 实测结果 |
|---|---|
| `check_config` / `full_config` | 原有 62 项和完整分组 149 项检查通过 |
| `check_assets` | 93 项检查通过；使用临时合成文件验证正常清单、损坏哈希、缺文件及依赖/路径约束 |
| `control_stdin` / `jsonl_logger` | 39 项终端检查通过，日志测试输出 `LOGGER_TEST_OK` |
| `mock_audio` | 输出 `MOCK_AUDIO_OK`；格式、代次、取消、有限播放队列检查通过 |
| `build_boundaries` | 输出 `BUILD_BOUNDARIES_OK`；引擎独立构建不查询原生音频库，音频独立构建不查找引擎依赖；各自测试通过 |

`build/bin/env_check` 输出 `ENV_CHECK_OK`。本机 `config/local.yaml` 无修改即可通过检查，输出 `CONFIG_OK` 和 `ASSETS_OK models=0 dependencies=0 files=0`；所有模型默认禁用，因此没有验证真实模型资产。资产哈希复用现有 `/usr/bin/sha256sum`，未安装新依赖、SDK 或模型。

使用本机配置执行 `status → start → stop → start → cancel → new-session → quit`，退出码为 0；27 条事件均为有效 JSONL、序号连续，最终 session 为 2、generation 为 4。启动记录为 `audio_backend=mock`、`asset_checks=skipped_mock`，两次 start 各生成一帧合成静音的元数据，不保存 PCM。

本机证据保存在被 Git 忽略的 `output/runs/p1-01-complete-2026-10-14/`：`regression.log`、`build-boundaries.log`、`check-config.log`、`env-check.log`、`mock-control.log`、`mock-control/events.jsonl` 和 `verification.json`。报告包含实际命令、退出码、测试结果、代码/配置 SHA-256 与工作区状态；这些本机产物不随仓库分发。

上述结果满足 P1-01 工程骨架交付条件。尚未打开真实音频设备或加载模型，未验证 A01～A14、NPU 兼容或模型推理；下一步及相关前置条件见[执行计划](../EXECUTION_PLAN.md#9-下一次实施的首批动作)。
