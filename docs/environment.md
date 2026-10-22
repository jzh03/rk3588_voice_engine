# 开发环境与已完成基线

更新日期：2026-10-19（Asia/Shanghai）。系统与设备基线来自 2026-10-16 的 Windows 本机只读检查、现有 `ssh orangepi5` 会话的只读命令，以及用户明确确认；后续工程验证按下文日期记录。重装前盘点见 [§25](#25-重装镜像与-windows-codex-交接)；当前系统恢复结果见 [§26](#26-官方-jammy-系统网络时间与构建依赖恢复)。§2～25 保留各自历史时点语义。

## 1. 记录范围与证据边界

本文件保存系统、工具库和设备快照，以及 Git 交接与工程验证的历史证据。当前任务状态与后续动作统一见 [执行计划](../EXECUTION_PLAN.md#5-工作分解依赖与交付)，构建和使用方法见 [README](../README.md#基础环境检查)，SDK 官方前提见 [兼容性依据](compatibility.md)。

后续留存说明：用户已授权删除此前列出的 10 个旧运行目录，清单与结果见[§22.1](#221-旧排查运行目录清理)。下文对这些目录的路径引用为历史记录，其原始本机文件现已不再留存；其余保留目录及 `docs/evidence/` 不受本次删除影响。

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

2026-10-17 推送排查：本机 main 为 `bd759b5`，领先远端 `1262803` 两个提交。VS Code Git 日志报告 `Invalid username or token` / `Authentication failed`；`gh auth status` 判定现存凭据无效，`gh api user` 返回 HTTP 401。匿名读取远端 main 成功，推送 dry-run 因缺少可用凭据失败，未实际推送。当前代理下 GitHub 可达；本机直接 DNS 查询失败是附加观察，未阻止上述代理请求。尚需用户重新完成浏览器授权，恢复步骤见 [README](../README.md#版本管理约定)。脱敏证据位于被忽略的本机目录 `output/runs/git-push-auth-2026-10-17/diagnosis.json`；未读取或保存凭据，未更改代理、凭据或 Git 配置。

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

## 12. P1-02 真实音频开发与待验收项

2026-10-17（客户端日期，Asia/Shanghai），完成真实音频后端、流式 DSP、`audio_probe` 及相关自动回归。产物中的 UTC 时间保留板端时钟读数。当前用法与配置契约见 [README 真实音频诊断](../README.md#真实音频诊断)，实机标准及操作见 [A01/A02](ACCEPTANCE.md#a01a02-实机执行步骤)。

沿用已安装的 libpulse 16.1、libsamplerate 0.2.2、libsndfile 1.2.2 及 PipeWire 服务，本轮没有安装依赖、更改驱动/耳机 profile 或修改系统音频路由。原有 `config/local.yaml`、默认参数和历史原始证据保留。

Release 构建成功；最终执行 `ctest --test-dir build --output-on-failure -V`，9/9 组通过，耗时 43.37 s。配置、完整配置、资产、控制分别通过 62/149/93/39 项检查；mock、DSP 和日志单元测试通过。私有音频集成检查与引擎/纯 mock/原生音频三种独立构建均通过。

音频集成测试启动完全独立的 PipeWire、Pulse 协议服务和最小连接策略，只含合成输入与空输出。已验证真实协议数据流、格式转换、正常停止保尾、WAV 保存/回放、显式设备拒绝、快速取消后新 generation、服务断开后重建且等待新触发，以及 SIGINT/SIGTERM 收尾。该测试没有访问物理音频设备。

另保存一轮 10 s 合成双向运行：输入为 48 kHz，输出到内部 16 kHz 单声道，累计 160000 个有效采样、500 个播放帧；有效采集时间 9.997947 s，时长误差约 0.0205%，采集/播放溢出、采集 hole 和播放 underrun 均为 0。正常停止后的内部采样数与原生输入换算相差不超过 1 个采样。

开发期间由私有测试发现并修复了协议处理积压：原来每次只处理一个事件便等待，导致有数据排队时仍断流和排空超时；现在每轮有界处理当前可运行的协议事件。保留的标准 `paplay` 对照在同一私有服务中正常播放并结束，修复后的后端也通过回归。

本机证据位于被 Git 忽略的 `output/runs/p1-02-dev-2026-10-17/`：

- `ctest.log` 与 `verification.json`：最终回归及源码/配置/程序 SHA-256。
- `synthetic-config.yaml`、`synthetic-duplex/{run.json,events.jsonl,summary.json}`、`synthetic-duplex.log` 和 `synthetic-server/`：可追溯的合成运行及私有服务配置/日志。
- `device-availability.json`：只读枚举设备的快照；该次未发现本机配置的耳机 source/sink。
- `debug/native-drain-before-fix/`、`debug/standard-client-control/`：修复前失败现场和标准播放器对照，均为合成数据。

**P1-02 保持进行中，A01/A02 尚未验收。** 已提醒用户连接 vivo TWS 4；用户选择稍后验收。届时先进行短试听，再按规范执行 600 s 连续录放及空闲/录音/播放三种状态的物理断线重连。声音听感、耳机路由、真实重连恢复和蓝牙尾音仍待用户配合确认；合成测试不代替这些结果。

同日用户尝试 10 s 双向诊断，run 为 `audio-1791996975422-18997`，实际在设备等待阶段以 `3/device_unavailable` 结束，采集和播放均为 0。日志明确为配置中的 source/sink 不可用；只读复核显示蓝牙服务 active、适配器 Powered=true，目标耳机 Paired/Trusted=true，但 Connected/ServicesResolved=false，音频服务没有对应 bluez 节点。对已配对耳机执行一次有界重连，12 s 后仍未连接，已提醒用户将耳机开机并准备连接。未修改本机配置、profile 或默认路由，B01 历史结论保留；本次不计为短试听或 A01/A02 通过。原始日志、报告及补充诊断保存在 `output/runs/audio-1791996975422-18997/`，恢复步骤见 [README](../README.md#真实音频诊断)。

用户随后确认可以重连，并说明 Ubuntu 蓝牙界面显示 Disconnect。再次执行 18 s、20 s 的有界连接仍未成功；BlueZ 日志包含 `Host is down` 和连接超时。枚举确认只有一个蓝牙控制器、目标地址与配置一致，目标仍为 Connected/ServicesResolved=false，没有 bluez 音频节点；8 s BR/EDR 搜索期间未发现该地址，搜索已结束。执行一次断开结束未完成连接，配对和信任记录仍保留，未运行新的音频试听。现场保存在 `output/runs/headset-reconnect-2026-10-17/{reconnect.json,discovery.json}`。已请用户让耳机进入可发现状态后继续；物理步骤参考 [vivo 客服 TWS 系列通用配对说明](https://kefu.vivo.com.cn/robot/imgmsgData/f60dfccb31c8465e8be88c037d85ac7e/index_1.html)，该页面不是 TWS 4 专属手册。

后续耳机进入可发现状态后，扫描发现目标，RSSI 约 −32～−28 dBm。经用户明确授权，仅删除并重建该耳机配对；配对成功，期间出现 ServicesResolved=true，重新设置信任成功，但随后音频连接仍超时。适配器识别为 Realtek RTL8821C（USB `0bda:c820`），当前绑定 `btusb`，固件版本 `0x75b8f098`。内核此前出现两次 `command 0x0c03 tx timeout` 及一次 `unexpected event for opcode 0xfc19`；这些记录不能单独确定硬件损坏。板端时间 2026-10-15 01:10:56 的 USB 断开、01:10:59 的重新枚举，经用户确认是手动拔插造成，不能归为自发故障；重新枚举后固件加载成功。

用户确认双耳已取出、手机蓝牙已关闭后，再次连接返回 `org.bluez.Error.Failed br-connection-page-timeout`。随后间隔约 3 s 的五次快照均为 Paired/Bonded/Trusted=true、Connected/ServicesResolved=false，无目标 card/source/sink，未启动录放。用户没有另一台蓝牙音频设备，选择先记录当前结果；目前证据不足以判断模块物理故障。后续有条件时交叉测试耳机与另一正常主机、开发板与另一正常音频设备，再决定是否进一步比较适配器或驱动；稳定连接后继续短试听与 A01/A02，不改写 B01 历史结论。

本次状态、用户确认、先前操作摘要、配置/程序哈希及服务/内核日志保存在 `output/runs/headset-isolation-2026-10-15/{diagnosis.json,bluetooth-journal.log,kernel-journal.log}`，目录日期和快照时间保留本次会话及板端读数。已结束交互式蓝牙客户端，扫描关闭、Pairable 恢复为 false、Powered 保持 true，耳机配对和信任记录保留；本机音频配置、profile、默认路由及驱动均未修改。P1-02 继续进行中，A01/A02 未验收。

按用户最新安排，本轮结束现场蓝牙排查，完成 P1-02 软件与文档交付；后续实机测试由用户自行进行。用户此前确认的 B01 结论保留，本次连接失败不改写历史结果。自测从 README 的 10 s 双向诊断开始，连接及短试听正常后，再按验收文档执行 A01/A02；未取得结果前不标为通过。

## 13. RKNPU 0.9.8 升级预检查

2026-10-18（客户端日期，Asia/Shanghai），按用户安排优先调查 RKNPU 0.9.7 → 0.9.8。用户明确要求安装前先确认；本轮只做系统只读检查、源码下载与隔离编译，没有安装依赖、替换系统文件、切换启动项或重启。**运行中的内核仍为 `6.1.0-1025-rockchip`，驱动仍为 `0.9.7`。**

| 检查 | 结果与限制 |
|---|---|
| 驱动部署形式 | `modinfo rknpu` 为 `(builtin)`，内核配置 `CONFIG_ROCKCHIP_RKNPU=y`；NPU 设备树 compatible 为 `rockchip,rk3588-rknpu` |
| 软件源 | 重新下载当前 PPA Noble arm64 索引；`gpgv` 用系统已信任的 PPA key 验签成功，Packages.gz 的 SHA-256 与签名 Release 一致；元包仍指向 1025，1026 源码驱动仍为 0.9.7 |
| 安装模拟 | `apt-get -s --no-install-recommends install linux-rockchip` 返回 0，未计划任何安装或升级；仅为模拟，没有安装操作 |
| 内核来源 | 找到原版 1025 tag；其 `rockchip_opp_select.h` 与本机已安装 headers 逐字节一致；未下载或构建完整内核 |
| 原始驱动编译 | 首次因 headers 缺少内部 `drivers/devfreq/governor.h` 失败；从固定内核提交补齐隔离 include 后，复现 RK3576 的 `rockchip_opp_set_low_length` 未声明错误 |
| 候选补丁 | 零 fuzz 应用后，7 个驱动 C 文件编译并合并为 AArch64 `rknpu.o`，返回 0；ELF `.modinfo` 含 `version=0.9.8` |
| 编译器 | 预检查用 GCC 13.3.0；当前系统内核由 GCC 13.2.0 构建，日志保留版本差异提示；不构成完整内核兼容验证 |
| 完整构建条件 | 本机尚无 bison、flex、pahole、libelf-dev、libssl-dev；根分区当次约 18 GiB 可用，原配置开启 DWARF5 / BTF，完整构建空间仍待核算 |
| 启动与恢复 | 发现通用内核包 DTB 安装位置与本机默认启动查找位置不同，需解决后再评审安装；现有镜像仅 root 可读，`sudo -n` 要求密码，本轮未完成启动备份或恢复演练 |

驱动目标文件大小为 2,234,664 B，SHA-256 `86b98348de8b055e4b76101052c5c0da9b1b05009a76b9d83b1b37bd4b1c6535`。这是隔离编译产物，不是可直接部署的 `.ko` 或内核镜像；未执行 modpost / vmlinux 最终链接、启动、模型推理或外围设备回归，不据此标记 P0-02 完成。

本机证据位于被 Git 忽略的 `output/runs/rknpu098-preflight-2026-10-18/`，不随仓库分发：

- `run.json`、`verification.json`、`before/`：初始 Git HEAD、工作区 diff 哈希、相关文件/配置哈希、命令和验证边界；原有未提交的音频工作保留。
- `sources.json`、`ppa-noble-inrelease`、`ppa-noble-packages.gz`、`ppa-signature.txt`、`apt-verification.json` 和 `apt-simulation.txt`：在线原始资料、哈希、签名核验与模拟结果。
- `compile-original.log`、`compile-with-private-header.log`、`compile-patched.log` 及对应 `*-command.json`：两次失败和补丁后的目标文件编译结果；`object-header.txt` / `object-modinfo.txt` 保存 ELF 检查。
- `compile-original/`、`compile-patched/`、`kernel-private/`、原始驱动归档及固定版本源码片段：本次隔离输入与产物；`manual-pages-12-13.txt` 为留存手册的对应页文本。
- `boot_config.txt`、`kernel_config.txt`、`original-kernel-package-status.txt` 和 `sudo-availability.txt`：当前启动配置、原内核配置/包状态及权限限制；不是启动备份。

固定来源、候选补丁和安装/恢复前置条件集中见[兼容性记录](compatibility.md#rknpu-098-升级评估)。本次仅支持“具备进一步构建评估的依据”，不表示已经可以安全安装或完成升级。

## 14. 内核重编译资源复核

2026-10-18（客户端日期，Asia/Shanghai），补做板端资源、构建依赖和发行版打包规则的只读核对。没有下载完整内核、安装软件、修改候选配置、开始完整编译或重启；当前内核和 RKNPU 版本不变。

| 项目 | 本次快照与边界 |
|---|---|
| CPU | 8 核在线，RK3588S；可以低并发试编，尚无完整内核编译耗时/负载实测 |
| 内存 | MemTotal 8,307,392,512 B（7.74 GiB），MemAvailable 6,723,878,912 B（6.26 GiB）；空闲快照不能代表编译峰值 |
| swap | 1,073,737,728 B，当前未使用；没有调整 swap |
| 磁盘 | `/dev/mmcblk1p1` 可用 19,055,251,456 B（17.75 GiB）；`/`、`/tmp`、`/boot` 共用该分区，不能分别计算三份余量 |
| 存储介质 | `mmcblk1` 为 SD，设备容量约 31.27 GB（十进制）；本次 `lsblk` 未见外接块设备，不代表用户手边没有可用存储 |
| 温度 | `soc-thermal` 当次约 41.6 ℃；未进行持续编译/散热测试 |
| 原配置 | DWARF5、BTF、BTF modules、GDB scripts 开启；这些会影响构建空间与依赖要求，尚未修改 |
| 依赖 | bc、rsync 已安装；bison、flex、libelf-dev、libssl-dev、pahole、fakeroot 等缺失，发行版打包还需另核对完整 Build-Depends |
| 基础依赖模拟 | 对 bc/bison/flex/libelf-dev/libssl-dev/pahole/fakeroot/rsync 执行 `apt-get -s --no-install-recommends install`，返回 0；模拟为 9 个新包、7 个升级，含 OpenSSL、rsync 等连带升级；未实际执行安装，不能作为最终安装清单 |
| DTB 打包规则 | 固定源码 `debian/rules.d/2-binary-arch.mk` 的 `dtbs_install` 使用 `/lib/firmware/<abi_release>-<flavour>/device-tree`，匹配本机启动布局；通用 `bindeb-pkg` 的路径不同，后续优先评估发行版规则。尚未生成完整包 |

CPU 与内存具备低并发尝试条件，当前主要限制是原调试配置的磁盘预算。量化的构建额度、并发约束和下一步选择集中维护在[执行计划 §8.1](../EXECUTION_PLAN.md#81-资源控制)，不能把语音应用的原有空间预算用于完整内核构建。

本机证据位于被 Git 忽略的 `output/runs/rknpu098-resources-2026-10-18/`，不随仓库分发：`run.json`、`resources.json`、内存/磁盘/CPU/SD/温度快照、`package-status.txt`、`dependency-simulation.txt`、原版打包规则片段及 `sources.json`。`before/` 保留本轮修改前的相关文档；在线资料仅作为通用依据，不替代本板构建峰值实测。未进行压力测试或运行无关的语音程序回归。

## 15. 免板端重编路线核对

2026-10-18（客户端日期，Asia/Shanghai），补充核对预编译产物与其他部署路线，纠正“升级内置驱动只能在板端重编”的表述。只读取本机驱动状态、公开发布说明和 API 资产列表；没有下载完整镜像或内核二进制、安装软件、写入启动介质、切换内核或重启。运行中的驱动仍为 0.9.7，`modinfo` 仍为 `(builtin)`。

| 来源 / 检查 | 核对结果与边界 |
|---|---|
| [darkice9x master 发布](https://github.com/darkice9x/ubuntu-rockchip-modified/releases/tag/master) | 发布于 2025-03-04；作者标注 Orange Pi 5 / 5 Plus、Ubuntu Noble、内核 6.1.0-1027、NPU 0.9.8。API 仅列两份对应板型 `.img.xz`，没有独立 Noble 内核 `.deb`；源码 zip/tar 链接不是内核安装包 |
| Orange Pi 5 镜像资产 | `ubuntu-24.04-preinstalled-desktop-arm64-orangepi-5.img.xz`，1,842,807,668 B；API digest 为空，当前资产列表未列独立校验文件；未下载，不掌握实际文件哈希、配置或启动结果 |
| [同仓库 OrangePi5Max 发布](https://github.com/darkice9x/ubuntu-rockchip-modified/releases/tag/OrangePi5Max) | 有 image/modules/headers 等 `.deb` 及 5 Max U-Boot 包，但发布注明 5 Max、Ubuntu 22.04 Jammy、5.10-1012；与本板现状不匹配，不作直接安装候选 |
| [PR #36 作者说明](https://github.com/Joshua-Riek/linux-rockchip/pull/36#issuecomment-3510316860) | 作者称该镜像面向 Orange Pi 5 和 5 Plus，并链接上述发布；这是第三方作者的适用声明，不是本板验证或官方 SDK 认证 |
| 独立预编译包 | 当前 PPA 及本次核对的第三方发布中，没有核实到可在当前系统原地安装的配套 Noble / 0.9.8 包；不据此推断所有来源都没有该产物 |
| 旧 SDK 分支 | 读取官方 tag 列表及固定版 CHANGELOG，确认 Qwen3.5 支持始于 1.3.0；没有核实并冻结适配 0.9.7 的旧 SDK/模型组合，不把降版本视为当前目标的等价替代 |

本次结论是存在免板端自行编译的具体镜像来源，仍需对候选产物做实际校验和部署评估。提取镜像内核/模块/DTB、在备用介质启动或外部构建均尚未执行；不声称已解决模块 ABI、启动、外围设备或模型兼容性。路线比较与下一步要求集中见[兼容性记录](compatibility.md#免板端重编的候选路线)。

本机证据保存在被 Git 忽略的 `output/runs/rknpu098-alternatives-2026-10-18/`：`run.json`、`alternatives.json`、`darkice-releases.json`、`pr36-comments.json`、`issue1093-comments.json`、固定 SDK tag/CHANGELOG 记录、`sources.json` 和文档修改前副本。记录包含来源 URL、响应哈希及未验证项；未新增模型/依赖 manifest，因为没有取得或部署真实推理资产。

## 16. 预编译产物实取与离线评估

2026-10-18（客户端日期，Asia/Shanghai），按用户要求实际取得并评估独立内核包及镜像提取路线。所有下载、解包、`depmod` 索引生成和校验均在 `output/runs/rknpu098-prebuilt-eval-2026-10-18/` 内完成；`debugfs` 使用默认只读模式，没有挂载镜像或执行其中程序。没有安装软件、增加系统软件源/密钥、修改启动配置、写卡或重启；运行中的内核仍为 6.1.0-1025-rockchip，驱动仍为 0.9.7。

### 本板预编译镜像

原始来源及内容锁集中见[兼容性记录](compatibility.md#预编译产物与镜像提取评估)。通过 HTTP Range 读取头尾，核对 GPT 和 XZ 索引后下载完整镜像：压缩输入 1,842,807,668 B，展开逻辑长度 7,352,614,912 B；根分区起始偏移 16,777,216 B，分区长度 7,334,789,120 B。下载耗时约 237 s；流式解压、XZ 完整性检查与根分区提取约 447 s，期间 SD I/O 是主要等待来源，未做性能基准测试。

| 检查 | 结果与限制 |
|---|---|
| 文件系统与系统版本 | 根分区 ext4，超级块标记 clean；镜像内 `/usr/lib/os-release` 为 Ubuntu 24.04.1 Noble。未执行完整文件系统扫描或启动 |
| 内核配套 | 实际提取 `vmlinuz`、config、System.map、modules 和版本目录下的 DTB；版本为 6.1.0-1027-rockchip / 6.1.0-1027.27，基础 Linux 6.1.75 |
| NPU 静态信息 | `CONFIG_ROCKCHIP_RKNPU=y`，内置模块元数据及隔离 `modinfo` 报 0.9.8；没有加载到运行中内核 |
| 设备树 | Orange Pi 5 型号/compatible 正确，NPU compatible 为本板对应值；本板 DTB 与当前 1025 版本文件逐字节一致 |
| 模块集合 | 共 2242 个 `.ko`，逐个读取 ELF 架构与 vermagic，全部为 AArch64 且匹配 6.1.0-1027-rockchip；当前快照中 31 个已加载模块名称均有对应项。名称覆盖不代表设备实测通过 |
| 符号依赖 | 用配套 System.map 对隔离目录执行 `depmod -n -e -F`，返回 0、无警告；这不是启动验证或完整 CRC/ABI 验收。镜像未提供对应 arch headers 的 Module.symvers |
| 配置差异 | 与当前配置比较有 19 项差异，完整差异保存为 JSON；CFG80211、MAC80211 等从内置改为模块，需在后续启动/网络回归覆盖 |
| 包 MD5 对照 | 检查 2948 个相关载荷文件，无缺失，2947 项一致；`rtl8812au/88XXau.ko` 一项不符。独立重新 dump 得到相同实际 MD5，排除首次提取复制错误；未找到解释其变更的包记录/作者说明，保留失败项 |
| 原始安装包 | 镜像 apt 缓存只有 lock/partial，没有可直接提取的原始 `.deb`；安装记录中的 image/modules/buildinfo/common headers 为 6.1.0-1027.27，状态为 hold。不会把镜像中的 hold 状态应用到本机 |
| 候选载荷 | 最终隔离目录含 2958 个普通文件，合计 282,677,914 B，包含本地生成的模块索引；逐文件 SHA-256 清单已保存。没有生成独立安装包或本机 initramfs |

完整性检查脚本保留退出码 1，因为单项 MD5 差异未通过；后续模块及符号检查的通过结果不覆盖该失败。未执行 A01/A02、NPU 推理或其他语音程序回归。

### 独立 PPA 内核包

找到 Wang Haoyang 的 `rockchip-bsp` Noble 源，取得 `linux-image-6.1.0-1005-rockchip`、`linux-modules-6.1.0-1005-rockchip`、`linux-modules-extra-6.1.0-1005-rockchip` 三包，包版本均为 6.1.0-1005.5。压缩大小分别为 42,997,952 / 14,370,402 / 5,914 B。用 Launchpad API 公布的指纹 `860B23D6BDDFF94E8788AB42D786860960965BEC` 在隔离目录验证 InRelease，再核对索引及包 SHA-256，均一致；没有向系统添加该源或密钥。这是第三方发布验证，不是官方适配认证。

解包后核实其内核为 Linux 6.1.115、内置 RKNPU 元数据为 0.9.8。包内有 388 份 DTB，但没有 `rk3588s-orangepi-5.dtb`；与当前配置有 2234 项差异，当前已加载模块名称中 8821cu、rtw88_8821cu、rtw88_usb、joydev、uio_pdrv_genirq、uio 无对应项。尚未建立替代驱动关系，不能据名称缺失断言硬件绝不支持，但这套包不构成本板可直接安装的完整组合，本轮不采用它部署。

### 证据、空间与后续边界

本机目录 `output/runs/rknpu098-prebuilt-eval-2026-10-18/` 被 Git 忽略，不随仓库分发：

- `assessment-summary.json`：两条路线的结论、未验证项及未安装状态；`run.json`、`before/` 保存起始代码/配置标识和文档快照。
- 原始 `.img.xz`、`image-download.json`、`image-layout.json`、`partition-extraction.json`：实际输入、下载哈希、分区和流完整性结果。
- `image-candidate-root/`、`image-payload-manifest.json`、`image-assessment.json`：实际提取的候选载荷、逐文件 SHA-256、含失败项的校验报告。
- `image-config-diff.json`、`image-all-module-vermagic.json`、`image-depmod-*`、`image-package-info/`、`88XXau-mismatch-review.json`：配置、模块、符号依赖、包记录和单项差异复核。
- `ppa-debs/`、`ppa-candidate-root/`、`ppa-control/`、`ppa-package-verification.json`、`ppa-assessment.json`、`ppa-config-diff.json`、`sources.json`：第三方包、签名链、索引/包哈希、只读来源及限制。
- 本轮下载/提取/检查脚本、命令日志、`storage-cleanup.json`：可重现流程。解压中间文件 `candidate-rootfs.img` 已在检查后删除，原逻辑长度、分配空间和哈希仍留存；原始压缩输入及候选载荷保留，可重新提取。清理后根分区当次约 16.21 GiB 可用，不套用为后续安装余量保证。

**评估已完成，部署条件未满足。** 保留镜像提取路线继续调查 MD5 差异及独立打包/回退方案；未把候选写成 verified，也未建立已验证 SDK/模型组合。安装、启动切换和重启仍须先提交具体方案并经用户明确确认。

## 17. 开发板闲置文件清理

2026-10-18（客户端日期，Asia/Shanghai），按用户要求清理开发板垃圾文件。先检查目录占用及用户进程的可执行文件、参数路径、映射和打开文件，再逐项删除闲置文件；系统保护的 systemd、sd-pam、sshd 部分进程信息不可读，限制已记录。板端时钟仍显示 2026-10-15，原始观测时间与客户端日期分别保存在证据中。

| 已清理内容 | 删除前实际占用 |
|---|---:|
| 闲置 VS Code Server 1.141.1，commit `7baac39ec3ca969dab74ac12c097f6a486e46593` | 630.29 MiB |
| 两个闲置 `code-07f806…`、`code-7baac3…` 启动程序 | 55.54 MiB |
| 对应启动日志和 8 份已结束的 VS Code 会话日志 | 6.52 MiB |
| 已退出的 Chromium HTTP/代码缓存及桌面缩略图缓存 | 25.46 MiB |
| **合计** | **752,680,960 B，约 717.81 MiB** |

当前使用的 VS Code Server 1.142.0（commit `6d62b1d5dace93a33b3a6d3eca1317bd5e15bcb1`）、Codex 扩展、编辑器配置/历史与当前会话日志保留。清理后原 13 个 VS Code/Codex 可执行进程仍存在，受保护的程序及配置 SHA-256 一致；Bluetooth、PipeWire、pipewire-pulse 和 WirePlumber 前后均为 active。文档更新前，清单内 56 份项目文件及本机配置哈希均未改变。内核仍为 `6.1.0-1025-rockchip`，RKNPU 仍为 `0.9.7`。

清理后的根分区一次快照可用 19,002,748,928 B（约 17.70 GiB）。同期另一个 Python 进程正写入 `output/runs/rknpu098-prebuilt-eval-2026-10-18/` 下的候选镜像 `.img.xz.part`，已确认打开文件并保留，因此整盘可用空间差不能直接作为本轮释放量；上表按删除对象的分配块计量。项目源码、构建产物、既有运行证据、模型/依赖、凭据和系统文件均未列入删除范围。

尚未清理：`snap list --all` 中 disabled 的 `core22` revision `1624`、`lxd` revision `30134` 和 `snapd` revision `21761`，文件大小合计约 197.34 MiB。它们由 root 所有，`sudo -n true` 提示需要密码；后续需在板端终端认证后逐项执行 `sudo snap remove <名称> --revision=<上述版本>`，执行前复查仍为 disabled。APT archives 没有待清理的 `.deb`，未删除包索引或系统日志。

本机证据位于被 Git 忽略的 `output/runs/board-cleanup-2026-10-18/`：`run.json` 保存代码/配置标识与初始磁盘快照，`cleanup.py` 为本次实际检查/清理脚本，`cleanup-plan.json`、`cleanup-result.json` 保存逐项大小、操作与核验结果，`active-paths-{before,after}.json` 保存进程路径引用，`concurrent-activity.json` 记录并发下载，`df-after.txt` 保存清理后磁盘读数，`before/environment.md` 保存本轮文档修改前副本。仅更新环境记录，任务阶段未改变；未运行无关的构建或 CTest。

## 18. 官方固件与 Ubuntu 26.04 只读核对

2026-10-18（客户端日期，Asia/Shanghai），按用户要求核对瑞芯微官方固件、Orange Pi 5 板厂镜像及 Ubuntu 26.04 是否能解除当前驱动兼容阻塞。只取得公开网页、目录 API 元数据和源码文本；没有下载本节涉及的完整固件或系统镜像、安装依赖/内核、变更系统源/密钥、修改启动配置、写卡或重启。运行中仍为 `6.1.0-1025-rockchip`、RKNPU `0.9.7`，板型 compatible 仍为 `rockchip,rk3588s-orangepi-5`。

| 实际检查 | 证据与边界 |
|---|---|
| 手册官方链接 | 对仓库留存 PDF 执行 `pdfinfo -url`，取得第 12 页官方固件超链接；解析入口 HTML 内嵌的目录 JSON，不将页面未渲染的空表误判为无固件 |
| 瑞芯微固件目录 | 访问入口公开链接，按页面 JavaScript 使用的只读 metadata GET 展开 Linux6.1 / Linux6.12 → RK3588 → RK3588S-EVB1-V10；目录响应均已留存。确认是参考板固件分类，未取得二进制内容，不声称核实了归档中的驱动版本 |
| 本板官方镜像目录 | 从 Orange Pi 5 下载页的 Official Images → Ubuntu 链接读取公开目录；解析文件名、文件 ID、列示大小及时间。确有 Resolute / 6.18.49 和 Focal、Jammy / 6.1.99 项，未请求镜像文件内容，目录大小不是实取大小或哈希 |
| 板厂构建与内核源码 | 保存固定提交下的板型配置、family 配置、两套内核配置、Makefile、厂商驱动头文件及 next 的 drivers 目录 API。源码支持“26.04 next 为 Rocket、BSP 6.1.99 为厂商 RKNPU 0.9.8”的判断，但未建立已发布镜像与精确源码的可复现构建对应关系 |
| 结论与下一步 | Ubuntu 版本升级不直接等于当前 RKLLM 驱动要求达标；优先核验新增的板厂 BSP 产物。具体来源、版本锁与兼容性理由集中见[兼容性记录](compatibility.md#官方固件与-ubuntu-2604-镜像核对)，不把候选写成已部署或已验证 |

本机证据目录 `output/runs/rknpu098-official-images-ubuntu2604-2026-10-18/` 被 Git 忽略，不随仓库分发：

- `run.json`、`before/`：初始 Git HEAD、工作区 diff / 文档哈希、内核/驱动/板型与只读范围；保留原有音频代码和清理记录。
- `manual-hyperlinks.txt`、`rockchip-firmware.html`、`rockchip-firmware-catalog.json`、`rockchip-linux*-metadata.txt`：官方入口、SDK 及参考板目录原始响应；相关网盘页面和元数据获取依据同时保留。网页模板中的“链接失效”隐藏文案不作为失效结论，实际公开目录 API 可读。
- `orangepi5-downloads-http.html`、`orangepi5-ubuntu-folder.html`、`orangepi5-image-catalog.json`、`orangepi*.txt`：板卡厂商下载链、公开文件元数据及固定源码。官方站 HTTPS 在本次环境握手失败，HTTP 页面读取成功；来源链和限制均记录，未据此声称完成发布者签名验证。
- `sources.json`：逐次来源 URL、状态、留存响应字节数、SHA-256 和失败尝试；包含最初路径猜测的 404，不把单个猜测路径不存在作为整个驱动不存在的证据。
- `assessment.json`、`verification.json`：检查方法、源码与二进制的区别、结论、未验证项、文档检查及本轮完成时的系统状态。未进行模型、外围设备或启动验收，也未运行无关的构建 / CTest。

P0-02 保持进行中。此前第三方产物的单项 MD5 差异仍未解决；新增官方来源不会覆盖历史失败记录。本轮同步入口、兼容性及计划，并将原重复的清理章节编号顺延为 §17，清理原文保持不变。

## 19. 方案 2 准备条件与驱动资料空间盘点

2026-10-18（客户端日期，Asia/Shanghai），按用户要求说明镜像提取路线，并只读盘点此前驱动排查的下载及衍生产物。统计范围为既有五个 `output/runs/rknpu098-*` 目录及留存的 RKLLM 官方 PDF；不代表整个项目或系统的全部占用，不含本轮新盘点目录的小额开销。没有新下载镜像、安装软件、解包/挂载镜像、删除文件、修改启动项或重启；运行中仍为 `6.1.0-1025-rockchip` / RKNPU `0.9.7`。

按 `lstat.st_blocks × 512` 统计实际分配空间，同一 `(st_dev, st_ino)` 只计一次，包含目录块，不跟随符号链接；使用 `du -B1 -s` 逐目录交叉核对一致。文件逻辑长度另存 JSON，不将压缩大小、展开逻辑长度和实际占盘混为一项，也不根据两次整盘空闲差推算下载量。

| 现存内容 | 性质 / 来源 | 实际占盘 |
|---|---|---:|
| `ubuntu-24.04-preinstalled-desktop-arm64-orangepi-5.img.xz` | 已下载的 darkice9x 第三方本板镜像；文件逻辑长度、来源与哈希见兼容性记录 | 1,842,814,976 B，约 **1.72 GiB** |
| `image-candidate-root/` | 从上述镜像提取的内核、模块、DTB 等，含本地模块索引，属于衍生产物 | 290,811,904 B，约 **277.34 MiB** |
| `ppa-debs/` 三个内核包 | 已下载的 haoyangw/rockchip-bsp image、modules、modules-extra；本次已评估为不直接用于本板 | 57,384,960 B，约 **54.73 MiB** |
| `ppa-candidate-root/` | 上述三个包的解包内容，属于衍生产物 | 186,032,128 B，约 **177.41 MiB** |
| 官方驱动源码归档 | `rknpu_driver_0.9.8_20241009.tar.bz2`，原文件 36,877 B；只是驱动源码，没有下载完整内核源码 | 40,960 B |
| 官方 RKLLM PDF | `docs/evidence/sdk/Rockchip_RKLLM_SDK_CN_1.3.1.pdf`，原文件 3,902,489 B | 3,903,488 B，约 **3.72 MiB** |
| 源码预检查产物及证据 | preflight 中的源码片段、目标文件、编译日志及文档副本等，不含上列驱动归档 | 9,854,976 B，约 **9.40 MiB** |
| 其他只读资料与检查记录 | API/网页/源码文本、目录快照、配置与模块检查报告、局部探测片段及文档副本 | 15,011,840 B，约 **14.32 MiB** |
| **合计** | 既有驱动资料及手册，包含原始下载和提取/解包结果 | **2,405,855,232 B，约 2.24 GiB** |

根分区当次用户可用 17,404,813,312 B（约 **16.21 GiB**）；`/`、`/boot`、`/tmp` 仍共用同一分区。此前删除的 `candidate-rootfs.img` 当前不存在，历史上分配约 5.20 GiB，未计入本次现存占用。保留的 `image-head.xz.part` / `image-tail.xz.part` 各为 65,536 B 的 Range 探测片段，并非未完成的整镜像下载。

用户打开的 `rknpu098-resources-2026-10-18/kernel-config.txt` 是配置快照，逻辑长度 243,090 B、实际分配 245,760 B；整个 resources 证据目录实际约 592 KiB，不是完整内核源码或系统镜像。香橙派官方 BSP 6.1.99 镜像与 Ubuntu 26.04 整镜像均尚未下载；先前只有其目录/源码资料。官方下一候选的来源及列示大小统一见[实施顺序](compatibility.md#镜像提取路线的实施顺序)，其展开大小尚未核实。

解包工具检查：本板有 `xz`、`debugfs`、`dpkg-deb`、`mkinitramfs`、`depmod`、`modinfo`，没有 `7z`、`7zz`、`bsdtar` 或 Python `py7zr`。用现有 APT 索引执行 `apt-get -s --no-install-recommends install 7zip`，返回 0，模拟为新增 `7zip 23.01+dfsg-11` 一个包、无升级或删除；没有刷新索引、下载或安装。模拟清单只说明当次依赖解析结果，实际安装前须复核并取得用户确认。

证据位于被 Git 忽略的 `output/runs/rknpu098-route2-storage-2026-10-18/`：`run.json`、`before/` 保存初始代码/配置标识与文档；`inventory.json` 保存分类/逐目录汇总、统计范围和文件系统快照；`file-inventory.json` 保存逐项逻辑长度、分配块和 inode；`du-crosscheck.txt`、`df.txt` 保存交叉核对原文；`inspection-tools.json`、`7zip-install-simulation*.json` / `.txt` 保存工具与模拟依据；`verification.json` 保存文档检查及状态边界。P0-02 阶段不变，安装包、initramfs、启动和模型验证仍未完成；本轮未运行无关构建或 CTest。

## 20. 方案 2 启动与旧产物清理

2026-10-18（客户端日期，Asia/Shanghai；原始主机时钟读数保留）。用户选择方案 2，并明确要求删除截图中的四类旧产物。本阶段清理指定内容、准备隔离工具和尝试公开下载，没有安装系统依赖或内核、写入启动介质、切换启动项或重启。运行中仍为 `6.1.0-1025-rockchip` / RKNPU `0.9.7`。

下表路径均相对于 `output/runs/rknpu098-prebuilt-eval-2026-10-18/`，四项均已删除：

| 已清理内容 | 实际释放的分配空间 |
|---|---|
| `ubuntu-24.04-preinstalled-desktop-arm64-orangepi-5.img.xz` | 1,842,814,976 B |
| `image-candidate-root/` | 290,811,904 B |
| `ppa-debs/` | 57,384,960 B |
| `ppa-candidate-root/` | 186,032,128 B |
| **合计** | **2,377,043,968 B，约 2.21 GiB** |

清理按四条固定路径执行，核对 inode、文件系统、符号链接及当前用户可读的进程引用；没有发现可读进程占用。部分受保护的 systemd / sd-pam / sshd 进程无法完整读取，限制已记入证据，不声称完成系统所有进程检查。旧目录其余 **132 份原有证据文件**按 SHA-256 核对未变，另新增 `artifact-availability.json` 说明大产物已清理；历史单项 MD5 失败仍保留。§16、§19 的旧占用及“当时保留”描述属于历史快照；此前约 5.20 GiB 中间文件的释放不计入本次合计。

隔离工具：用现有 APT 索引下载 `7zip 23.01+dfsg-11` arm64 包，文件为 1,803,596 B，SHA-256 `b85ce28f0c61e9fb5655e2426b72107b83385f62f0c79c91688509e01dec1159`，与索引一致。通过 `dpkg-deb --extract` 解包到本次运行目录，实际可执行文件为 `tool-root/usr/lib/7zip/7za`，`7za i` 成功、动态依赖可解析。没有执行安装或维护脚本，没有刷新/修改软件源、信任密钥或全局工具路径。

下载状态：官方 Google Drive 的四份 Ubuntu 6.1.99 镜像及一份 Debian Bookworm server 6.1.99 镜像均在取得正文前返回下载限额页，没有下载完整镜像。板厂[中文官方下载页](http://www.orangepi.cn/html/hardWare/computerAndMicrocontrollers/service-and-support/Orange-pi-5.html)提供的百度目录可读取公开文件名和大小；正常公开下载接口返回 `errno=0`，但载荷为不透明字符串，未取得可用文件流。不据此推断必须登录，没有使用私人账号或绕过下载限制。

用户随后决定自行从百度下载。交接前建立空 `input/` 目录和 `handoff.json` 记录预期输入，当时归档哈希为空、等待文件。用户实际放入此前的 storage 运行目录后，继续完成的离线检查、候选包与当前交接状态见下一节；`handoff.json` 已随之更新。安装前仍须提交具体方案并取得用户确认。

镜像交接前，准备工具及保存下载元数据后的根分区用户可用快照为 19,768,172,544 B（约 **18.41 GiB**）；本次新增工作目录当次实际分配约 **9.55 MiB**，主要为工具包、隔离工具及文本证据，无整镜像。这些是当时快照，交接及处理后的占用见下一节。

本机证据位于被 Git 忽略的 `output/runs/rknpu098-official-extract-2026-10-18/`：`run.json`、`before/` 保存初始代码/配置标识与文档；`cleanup_selected.py`、`cleanup-plan.json`、`cleanup-result.json` 保存授权范围、执行与保留文件哈希；`isolated-tool.json`、`isolated-tool-info.txt`、工具包和 `tool-root/` 保存隔离工具依据；`google-download-attempts.json`、公开响应、百度目录及 `baidu-public-download-result.json` 保存下载结果与限制；`assessment.json`、`handoff.json` 保存当前状态，`verification.json` 保存最终检查。原有音频实现、配置和构建文件未改动，P0-02 保持进行中；本轮未运行无关构建或 CTest。

## 21. 官方 BSP 镜像实取与离线候选包

2026-10-18（客户端日期，Asia/Shanghai），用户将官方 Jammy server / 6.1.99 归档放到 `output/runs/rknpu098-route2-storage-2026-10-18/Orangepi5_1.2.4_ubuntu_jammy_server_linux6.1.99.7z`，随后要求继续。最初观察文件约 320 MiB，复查时已传完整，大小为 621,852,720 B；哈希期间大小和修改时间稳定。没有移动、覆盖或删除用户提供的归档。

已用前节的隔离工具列目录并解包，7z CRC 通过；展开镜像为 3,091,202,048 B，与归档内 SHA-256 记录一致。MBR 只有一个 Linux 分区，偏移 31,457,280 B、长度 3,059,744,768 B；以稀疏副本和只读 `debugfs` 提取，未挂载、未执行镜像内程序。实际文件内容锁、驱动和包版本、完整性/模块/设备树结果及兼容差异集中见[官方候选核验](compatibility.md#官方-bsp-6199-离线核验与候选包)，没有据此宣称已启动或推理通过。

候选内核、778 个模块、设备树和两份未压缩 RTL8821C 固件已放入本次运行目录的 `candidate-root/`；固件来自同一官方镜像，分别为 51,700 B 和 10 B，源包 MD5 核验通过。准备过程中曾尝试使用现有压缩固件：固件主体可解压，配置的符号链接目标未能用 zstd 解压，未修改系统；暂存主体最终改为官方镜像的配套文件。原始候选索引与本地 `depmod` 生成索引分别留证，源包校验在原始索引状态下完成。

已生成 `packages/voice-engine-kernel-6.1.99-rockchip-rk3588_1.2.4+voice1_arm64.deb`，实际 34,902,652 B。包内 919 个普通文件的内容哈希、root 归属及当前目标文件冲突检查通过，不含维护脚本。`install-review.json` 列出备份、第一阶段并存安装/本机 initramfs、第二阶段恢复确认/试启动的具体边界；`extlinux-trial.conf.preview` 仅为预览，保留现有根 UUID 和旧默认入口，未写入系统。当前 root 权限下的完整备份尚未执行，本机 initramfs 尚未生成，恢复入口尚未确认或演练。

离线处理后，删除了本轮生成的完整 `.img`、`rootfs.ext4` 和 `package-root/` 三项临时内容，实际分配合计 5,739,016,192 B（约 5.34 GiB）；保留用户原始 `.7z`、归档内校验文本、候选组件、独立包和证据。此临时释放量与前节用户指定四类旧产物的 2.21 GiB 分别记录。清理后本次运行目录实际占用快照约 **163.36 MiB**，另有用户归档约 **593.04 MiB**；根分区用户可用为 18,963,509,248 B（约 **17.66 GiB**）。后续备份、安装及 initramfs 仍须复核峰值和余量。

同日按用户追问只读核算两套内核并存占用，记录于同目录 `coexisting-kernel-storage.json`：现有版本的内核、配置、System.map、initramfs、模块和版本专用固件/设备树合计实际分配 **309,280,768 B（294.95 MiB）**；候选组件暂存树实际分配 **121,647,104 B（116.01 MiB）**，安装后占用以此估算，另计包记录等小额开销。新 initramfs 尚未生成，暂按 **128 MiB** 预算，则两套运行组件合计约 **539 MiB（0.53 GiB）**。第一阶段备份归档另按 **320 MiB** 预算，相对当前新增持久文件约 **564 MiB（0.55 GiB）**；这两个额度均非实测大小或硬上限，未包含生成 initramfs 时的临时工作空间。现有归档与离线目录已占用的空间不重复计入新增量；本次根分区用户可用快照 **18,943,164,416 B（17.64 GiB）**。统计不含共享固件、headers/编译工具或系统应用；安装前仍须复核峰值及 3 GiB 余量，未据此授权安装。

本机证据仍位于被 Git 忽略的 `output/runs/rknpu098-official-extract-2026-10-18/`，本阶段新增：`continued-baseline.json` 与主机启动配置/设备绑定快照；`input-archive.json`、`archive-list.txt`、`extraction-space-plan.json`、解包与分区脚本/日志、`image-verification.json`；组件提取脚本/命令、原包元数据、`candidate-assessment.json`、配置差异与载荷清单；`original-module-indexes/`、索引差异/恢复记录、`firmware-preparation.json`；`prepare_candidate.py`、包内容与 `package-assessment.json`；`install-review.json`、启动项预览、`intermediate-cleanup.json`。重现被清理的中间步骤须从保留的用户归档重新开始，不把已清理文件写成现存证据。

`verification.json` 记录文档链接/差异、用户原有代码及配置哈希、旧证据保留、包及系统状态等最终检查。当前仍为 `6.1.0-1025-rockchip` / RKNPU `0.9.7`，原默认启动配置和设备树未改动；没有安装依赖或内核、切换启动项、刷写或重启。P0-02 保持进行中，GPU、无线、蓝牙音频和 SDK/模型待试启动后验收；本轮未运行无关应用构建或 CTest。

## 22. 可重建中间产物清理与整目录取舍

2026-10-18（客户端日期，Asia/Shanghai；原始主机时钟读数单独保留），用户明确要求先删除前次列出的约 128 MiB 中间产物，并询问哪些运行目录可以整个删除。本轮只删除下表四处授权内容，未删除任何完整运行目录。

| 已删除路径（相对于 `output/runs/`） | 删除前实际分配 |
|---|---:|
| `rknpu098-official-extract-2026-10-18/candidate-root/` | 121,647,104 B，约 116.01 MiB |
| `rknpu098-preflight-2026-10-18/compile-original/` | 2,682,880 B，约 2.56 MiB |
| `rknpu098-preflight-2026-10-18/compile-patched/` | 5,287,936 B，约 5.04 MiB |
| `rknpu098-prebuilt-eval-2026-10-18/image-88XXau-redump.ko` | 4,304,896 B，约 4.11 MiB |
| **合计** | **133,922,816 B，约 127.72 MiB** |

删除前复核独立包和用户原始 `.7z` 的 SHA-256，与 §21 的内容锁一致；候选副本全部 916 个普通文件与保留 `.deb` 的对应内容一致。检查 inode、属主、文件系统、链接及当前用户可读的进程引用，未发现选中路径被占用；systemd / sd-pam / sshd 的受保护字段不可完整读取，限制已留证。删除后四处路径均不存在，其他 637 份既有产物文件哈希未变，源码、本机配置、安装包、原始归档和源码补丁均保留。清理后根分区用户可用快照为 19,076,722,688 B（约 17.77 GiB）；实际磁盘差另受新增证据和后台写入影响。

后续需要恢复 `candidate-root/` 时，先按 §21 或 `install-review.json` 校验包哈希，再在仓库根目录对不存在的新目标执行：

```bash
dpkg-deb --extract \
  output/runs/rknpu098-official-extract-2026-10-18/packages/voice-engine-kernel-6.1.99-rockchip-rk3588_1.2.4+voice1_arm64.deb \
  output/runs/rknpu098-official-extract-2026-10-18/candidate-root
```

该操作仅恢复隔离副本，不安装系统文件。`verify_components.py`、`prepare_candidate.py` 等使用副本的历史处理脚本应先恢复对应输入，再按原流程运行；完整镜像提取仍需保留的 `.7z`。源码路线的目标文件可用保留的固定源码、驱动归档和补丁重新编译。旧第三方模块的原镜像此前已清理，重现其提取需重新取得历史输入；MD5 失败的原始报告仍保留。§13、§16、§21 中产物“当时存在”的记录不改写，当前可用性以本节及新增清理记录为准。

整目录盘点结果（约 128 MiB 清理轮次时的快照）：以下 10 个目录当时尚未删除，合计约 9.93 MiB。用户随后已确认删除，执行结果及证据可用性见[§22.1](#221-旧排查运行目录清理)。

| 类型 | 可整目录删除的候选 |
|---|---|
| 旧配置检查、控制和现场诊断 | `config-check-2026-10-16`、`config-check-2026-10-17`、`p1-01-control-2026-10-17`、`git-push-auth-2026-10-17`、`audio-1791996975422-18997`、`headset-reconnect-2026-10-17`、`board-cleanup-2026-10-18` |
| 旧驱动候选路线记录 | `rknpu098-alternatives-2026-10-18`、`rknpu098-official-images-ubuntu2604-2026-10-18`、`rknpu098-prebuilt-eval-2026-10-18` |

建议继续保留其余 7 个目录：`rknpu098-official-extract-2026-10-18` 保存当前独立包、安装评审与复现工具；`rknpu098-route2-storage-2026-10-18` 保存用户原始 `.7z`，尚未确认板外存在校验一致的备份；`rknpu098-preflight-2026-10-18` 与 `rknpu098-resources-2026-10-18` 保存源码备选路线的输入及资源依据；`p1-01-complete-2026-10-14`、`p1-02-dev-2026-10-17` 保存当前工程/音频回归证据；`headset-isolation-2026-10-15` 保存尚未解决的蓝牙排查结果。

本轮证据复用 `output/runs/rknpu098-official-extract-2026-10-18/`：`cleanup_intermediates.py` 为实际清理脚本，`cleanup-128mib-plan.json` 保存代码/配置标识、逐项占用、进程检查和保留文件哈希，`cleanup-128mib-result.json` 保存操作结果与恢复条件，`folder-cleanup-options.json` 保存整目录分类及文档引用，`before/cleanup-128mib/` 保存相关文档修改前副本，`cleanup-128mib-verification.json` 保存交付检查。旧 preflight 和 prebuilt-eval 目录另新增 `cleanup-128mib-availability.json` 标明本次删除项；既有报告未修改。当前仍运行 `6.1.0-1025-rockchip` / RKNPU `0.9.7`，未安装、改启动项或重启，任务阶段不变；本轮未运行无关构建或 CTest。

### 22.1 旧排查运行目录清理

2026-10-18（客户端日期，Asia/Shanghai），用户确认删除上文列出的 10 个旧运行目录。十个目录均已整目录删除，删除前实际分配合计 **10,416,128 B（约 9.93 MiB）**，涉及 240 个普通文件；不重复计算此前已清理的中间产物、旧镜像或 VS Code Server。

删除前逐项检查路径、inode、属主、文件系统、链接及当前用户可读的进程引用，未发现选中目录被占用。systemd / sd-pam / sshd 的部分受保护进程字段不可完整读取，限制留证。删除完成时项目源码与本机配置未变，保留目录内 439 份既有文件哈希均一致；原始 `.7z`、独立 `.deb`、源码补丁、最新回归和蓝牙排查记录保留。本轮没有移动、重新命名或删除其余运行目录。

原先建议保留的 7 个目录及随后新增的 `rknpu098-phase1-2026-10-18/` 第一阶段安装记录均保留，清理完成时共剩 8 个运行目录。第三方镜像/PPA 的详细命令、逐文件清单和原始失败报告已随旧目录删除；文档中的历史结论与 MD5 差异摘要保留，不能再将原目录写成可取得的本机证据。此前的 `folder-cleanup-options.json` 和旧清理报告保存其生成时的状态，当前可用性以本节与本轮结果为准。

本轮继续复用 `output/runs/rknpu098-official-extract-2026-10-18/`：`cleanup_old_runs.py` 为实际清理脚本，`old-run-cleanup-plan.json` 保存授权路径、逐项大小、进程检查、代码/配置标识和保留文件哈希，`old-run-cleanup-result.json` 保存逐项删除及验证结果，`old-run-cleanup-verification.json` 保存交付检查，`before/old-run-cleanup/` 保存清理前文档，`before/old-run-cleanup-documentation-current/` 保存同步说明前的最新文档快照。本次清理不改变项目阶段，不执行系统安装、启动切换或重启；未运行无关的程序构建或 CTest。

## 23. 第一阶段授权与安装执行

2026-10-18（客户端日期，Asia/Shanghai；主机时钟仍另行记录），用户明确要求“开始进行第一阶段吧，有问题随时问我”。本阶段范围为旧内核备份、独立候选包并存安装及本机 initramfs 生成/检查；保持原默认启动项，不重启。初次 `sudo -n true` 返回需要密码；完成可读预检查与固定执行脚本后，用户在开发板终端通过 sudo 执行，未在聊天中提供密码。

**第一阶段已完成并核验。** 原内核及启动配置备份通过安装前后两次 `tar --compare`，候选包安装成功，919 个包内文件与安装后文件的 SHA-256 一致。`depmod -e -F` 返回 0、无符号警告；本机 `mkinitramfs -c gzip` 返回 0、日志为空。initramfs 包含 `init`、本地根文件系统启动脚本、busybox、udev 和新版本模块索引，模块版本集合仅为 `6.1.99-rockchip-rk3588`。已独立复核安装包状态、六项关键内核/设备树/固件哈希和 initramfs 哈希。

| 已完成产物或检查 | 结果 |
|---|---|
| 旧内核完整备份 | `/var/backups/voice-engine/rknpu098-20261018/original-kernel-and-boot.tar`，307,159,040 B（292.93 MiB）；SHA-256 `47ee64ed60280d8e068b6ef0adc9674558dfdcdb17d923ec37cd536acea25c4a`，同目录保存 `SHA256SUMS` |
| 已安装包 | `voice-engine-kernel-6.1.99-rockchip-rk3588` / `1.2.4+voice1`，dpkg 状态 `install ok installed`；备份目录另保留一份 root 所有、哈希复核通过的 34,902,652 B 安装包副本 |
| 本机 initramfs | `/boot/initrd.img-6.1.99-rockchip-rk3588`，16,022,312 B（15.28 MiB）；SHA-256 `aa77add0fbdea493dc890937de121b263bfcd3fd4f4d53ce02440bc5b84287d4` |
| 新版本驱动元数据 | `modinfo -k 6.1.99-rockchip-rk3588 -F version rknpu` 为 `0.9.8`；表示已安装候选的内置元数据，不是运行中新驱动验证 |
| 当前运行版本 | `uname -r` 仍为 `6.1.0-1025-rockchip`，默认 `modinfo` 的 RKNPU 为 `0.9.7` |
| 启动配置与旧文件 | extlinux、`/etc/default/u-boot` 哈希与基线一致；`vmlinuz`、`initrd.img`、`initrd.img.old` 链接仍指向旧内核；原内核、模块及版本固件与备份逐项比较通过 |
| 包管理状态 | 安装前后 `dpkg --audit` 均为空；安装前无 pending/awaited triggers，候选路径不触发已注册的文件 triggers；没有运行 `update-initramfs`、`u-boot-update` 或镜像维护脚本 |

initramfs 是前置未压缩 CPIO 加 gzip 主 CPIO 的组合。直接对整个文件执行 `gzip -t` 曾返回“not in gzip format”，该命令不适用于其前置 CPIO 格式；随后解析前置归档，定位偏移 49,664 B 的 gzip 主流，其 CRC、流结束及解压后的 CPIO 格式均通过。主 CPIO 解压长度为 44,524,544 B。没有因此修改或重新生成 initramfs。

实际分配：新版本内核/模块/设备树/固件、包内文档及 initramfs 合计 **137,682,944 B（131.30 MiB）**；旧版本合计 **309,280,768 B（294.95 MiB）**，两套运行组件合计约 **426.26 MiB**，备份归档与安装包副本另计。执行步骤按约 250 ms 采样的最低用户可用空间约 **17.27 GiB**，不等同连续峰值测量；安装后复核快照为 **18,594,144,256 B（17.32 GiB）**。实际 initramfs 大小低于 §21 的预留额度，当前空间满足计划的 3 GiB 余量要求。

本机证据位于被 Git 忽略的 `output/runs/rknpu098-phase1-2026-10-18/`：`baseline.json`、`before/` 保存授权、代码标识与文档快照；`phase1_install.py` 为实际执行脚本，`script-validation.json` 保存语法、错误包哈希/启动配置拒绝检查；`phase1-check-result.json` 为非 root 预检查；`phase1-execute-result.json` 和 `execute-*.log` 保存实际命令、返回码、备份及 initramfs 哈希、剩余空间采样；`post-install-verification.json` 保存独立复核。脚本已执行，已有备份与新版本路径，**不要再次执行安装命令**；需要修复时先看结果与日志。原 official-extract 目录的 `install-review.json`、`assessment.json`、`handoff.json` 已更新为第一阶段完成并关联本阶段证据，离线历史报告保持原时点语义。

39 份原有源码、头文件、测试、构建及受 Git 管理/列示的配置文件哈希未变。没有修改启动菜单、试启动、重启、刷写 U-Boot/SPI 或迁移发行版；恢复入口仍未确认或演练。下一步先确认可操作的启动菜单、串口或恢复介质，再单独确认第二阶段试启动。GPU、无线、蓝牙音频及 SDK/runtime ABI 和模型推理仍待验收，P0-02 保持进行中。本阶段未运行无关应用构建或 CTest。

## 24. 第二阶段显示与恢复方式准备

2026-10-18（客户端日期，Asia/Shanghai），用户确认具有显示器、键盘、USB 转 TTL 串口和 SD 读卡器；后续说明电脑是 Windows，串口型号、电平与速率支持不清楚，并要求尽量不用串口。按该偏好准备 HDMI/键盘菜单路线。本阶段尚未获启动配置写入或重启授权。

只读复核：当前根分区为 `/dev/mmcblk1p1`、ext4，内核报告介质类型为 SD；根 UUID 仍与 extlinux 一致。运行内核/RKNPU 仍为 `6.1.0-1025-rockchip` / `0.9.7`，第一阶段原启动配置 SHA-256 保持不变。实际启用控制台为 `ttyFIQ0 tty1`，两套设备树的 FIQ debugger 均使用 UART2 / 1500000，旧内核的 `serial-getty@ttyFIQ0.service` active；命令行虽有 `ttyS2`，不能据此认定 `serial-getty@ttyS2` 可用。本阶段没有连接或测试串口。

最初 HDMI 报 disconnected；用户接线后短暂报告无画面，此时系统已读到 256 B EDID，HDMI 为 connected/enabled、DPMS On，日志显示启用 2560×1440@60，GDM 与用户桌面会话 active。热插拔时存在 VOP power-domain timeout 等日志，但尚未建立根因；随后用户明确回复“有画面”“一切正常”。没有调整分辨率、重启桌面服务或修改显示配置，当前桌面与键盘以用户确认记录。此结果不代表 U-Boot 阶段能显示菜单或接受键盘输入。

已准备两份未应用的配置：`extlinux-probe.conf.preview` 仅把 `timeout 20` 改为 `timeout 200`，保留 `default l0` 以及原来的旧内核普通/恢复入口；此次菜单测试不包含新内核入口。`extlinux-trial.conf.preview` 则另含后续新内核试启动项，须在恢复入口可用并获得单独授权后使用。U-Boot 文档将 timeout 单位定义为十分之一秒，因此对应 2 秒改为 20 秒；实际显示及输入仍须现场测试。[U-Boot 文档](https://docs.u-boot-project.org/en/latest/usage/pxe.html)

具体待授权步骤：使用已准备的 `apply_menu_probe.py --apply` 先保存 `/boot/extlinux/extlinux.conf.pre-npu098-probe`，核验原配置哈希，再原子写入仅变更等待时间的配置；脚本自身不重启。之后仅在用户明确确认的范围内进行一次现场重启，验证 HDMI 上能看到菜单、键盘能够选择旧内核并回到原系统。若没有菜单或键盘无效，允许旧默认项自动启动，继续准备其他恢复方法，不改为新内核默认项。脚本的只读预检查、语法检查、拒绝原配置变更/默认项变更/额外入口的验证均已通过，实际写入未执行。

Windows 原生不能直接挂载 ext4，Microsoft 文档列出的 `wsl --mount` 流程也不直接支持 USB/SD 读卡器，不能把现有读卡器视为已验证的系统恢复环境。如 HDMI 菜单不可用，再准备可操作的 Linux 恢复环境或其他实际验证过的方案；本阶段没有下载/制作恢复系统或刷写存储。[Microsoft 文档](https://learn.microsoft.com/windows/wsl/wsl2-mount-disk)

证据位于被 Git 忽略的 `output/runs/rknpu098-phase2-2026-10-18/`：`baseline.json` 与 `before/` 为工作区/启动/设备基线，`display-readiness.json` 与 `hdmi-kernel-log.txt` 为显示观察及用户确认，`extlinux.original.conf` 为原始配置副本，两份 `.preview` 与对应 `.diff` 为未应用方案，`phase2-review.json` 为权限及恢复边界，`apply_menu_probe.py` 和 `script-validation.json` 为待授权执行脚本与验证。原 `install-review.json`、`assessment.json`、`handoff.json` 已关联此准备记录。系统启动文件未改动，没有重启；P0-02 继续进行中，真实新内核、外围及推理验收仍待后续完成。

### 24.1 旧内核菜单测试授权与执行

2026-10-18（客户端日期，Asia/Shanghai），用户对上节提出的“延长菜单等待时间＋旧内核重启测试”明确回复“确认执行”。授权范围为写入仅延长等待时间的配置，随后一次现场观察的重启并用键盘选择原普通启动项；不包含新增候选入口、新内核试启动或变更默认内核。授权记录保存为 `rknpu098-phase2-2026-10-18/probe-authorization.json`。

执行前复核通过：当前仍为 `6.1.0-1025-rockchip` / RKNPU `0.9.7`，HDMI connected；原 extlinux 和 `/etc/default/u-boot` 哈希未变，待建备份和临时文件均不存在。已复核原脚本与预览哈希，并再次通过只读预检查。重启前 boot ID 为 `229cd3cb-7807-4980-a38b-625f06068f6c`，后续据此区分是否确实发生重启；不会仅凭旧内核版本相同宣称完成重启测试。

本次 `sudo -n true` 仍返回需要密码，用户已在开发板终端执行 `apply_menu_probe.py --apply` 并返回 `MENU_PROBE_APPLIED`。实际写入及备份已独立核验：`/boot/extlinux/extlinux.conf` SHA-256 为 `60a69ab93da2891ee3ee6fce6a253510036d46ef5f65176e48849491be36fc20`，与已审预览逐字节一致；`/boot/extlinux/extlinux.conf.pre-npu098-probe` SHA-256 为原配置的 `2f1faee463e1c3dd756d045f11e529a20ba8d6c4fd75eea2270f368c7d7dc120`。差异仅为 `timeout 20` → `timeout 200`，`default l0`、普通旧内核和旧恢复入口均保留，未加入候选入口；`/etc/default/u-boot` 未变，原子写入临时文件已不存在。

写入后 boot ID 仍与上述重启前标识相同，尚未记录重启完成。下一步由用户按已获授权范围在显示器旁执行 `sudo reboot`，按实际菜单提示用键盘选择普通 `Ubuntu 24.04.1 LTS 6.1.0-1025-rockchip` 项（不选 rescue），观察是否正常回到旧桌面。若看不到菜单或键盘无效，让旧默认项自动启动并报告现象；不将自动回到旧系统写成菜单恢复能力已通过。重连后同时核对 boot ID 已变化、旧内核/驱动和配置哈希，并记录用户观察；候选入口及新内核启动仍未授权。

证据仍统一在 phase2 运行目录：`before/probe-execution/` 为本次修改前文档与评审，`probe-authorization.json` 为授权/重启前基线，root 生成的 `menu-probe-apply-result.json` 为执行结果，`menu-probe-write-verification.json` 为独立差异与备份核验，`reboot-handoff.json` 为现场操作及重连核验要求。项目入口、计划、兼容性及现有交接 JSON 已同步；P0-02 保持进行中。

### 24.2 旧内核重启复核与菜单观察

本次按当前会话日期 **2026-10-15（Asia/Shanghai）** 记录，主机原始时钟也为 2026-10-15；沿用既有 `rknpu098-phase2-2026-10-18/` 证据目录，不改写先前记录的日期或当时执行状态。用户重连后报告“打开直接就进桌面了”，经询问补充“看到过启动画面，但没注意是否有菜单”。因此本次菜单观察结果为未确认，不能记成菜单未显示，也不能记成恢复入口验证通过。

**一次旧内核重启已完成并返回桌面；新内核未启动。** boot ID 从 §24.1 的 `229cd3cb-7807-4980-a38b-625f06068f6c` 变为 `6c1b6a00-3144-4868-8a92-65574da090c6`。当前 `uname -r` 为 `6.1.0-1025-rockchip`，RKNPU 元数据与本次内核日志均为 `0.9.7`；根分区仍为原 SD 的 ext4，显示管理器 active、HDMI connected，SSH 已可用。

extlinux 与已批准的菜单测试预览逐字节一致，SHA-256 仍为 §24.1 的 `60a69ab9…36fc20`，原配置备份和 `/etc/default/u-boot` 哈希也未变。配置仍为 `default l0`、`prompt 1`、`timeout 200`，仅有旧普通和旧恢复两个入口，未添加候选项。`timeout 200` 表示配置等待 20 秒；本次没有现场计时或 U-Boot 输出记录，不能据此宣称实际停留时间已验证。[U-Boot extlinux 文档](https://docs.u-boot-project.org/en/latest/usage/pxe.html)

只读检查未发现当前根文件系统中的 `/extlinux/extlinux.conf`、`/boot.scr` 或 `/boot/boot.scr`。已安装的 `u-boot-orangepi-5` 版本为 `2017.09+20240806.gitf73b1eed-2`，包内 `/usr/lib/u-boot/u-boot.itb` 包含 `stdout=serial,vidconsole`、`stderr=serial,vidconsole` 及 extlinux 扫描字符串，说明不能直接断言此构建只支持串口输出。但没有读取运行固件的实时环境或核对存储中的固件字节，包内字符串不能证明这次 HDMI 菜单/键盘实际可用；Linux 控制台、启动画面与桌面也不能替代该验证。U-Boot 环境可有保存值和默认值之分，须区分包内默认内容与运行状态。[U-Boot 环境文档](https://docs.u-boot.org/en/latest/usage/environment.html)

本次只读复核，没有安装、下载内核/镜像、改动系统启动文件、刷写固件或发起重启。下一步可在用户确认后，沿用现有配置再做一次现场旧内核重启观察，从重启前一直观察到桌面，辨认 `U-Boot menu` 和旧普通启动项，并按实际提示尝试选择；未确认菜单或输入能力时，仍需准备可验证的恢复方式。一次旧内核重启授权已使用，再次重启及候选启动不自动包含在本次检查范围内。P0-02 保持进行中。

证据：同一 phase2 目录内 `post-reboot-verification.json` 保存命令、boot ID、完整哈希、用户补充和限制，`post-reboot-kernel-display-log.txt` 为本次内核显示相关日志，`post-reboot-uboot-strings.txt` 为包内静态字符串；`before/probe-reboot-check/` 保存更新前文档及当前交接。`phase2-review.json`、`reboot-handoff.json` 和 official-extract 目录的三个当前交接 JSON 已同步为旧内核重启已验证、菜单观察未确认；先前安装和写入结果保留原时点语义。39 份项目源码/构建/配置文件哈希保持不变；本轮仅同步记录，未重复运行应用构建或 CTest。

## 25. 重装镜像与 Windows Codex 交接

2026-10-19（客户端日期，Asia/Shanghai），用户决定自行重装香橙派官方 Ubuntu 26.04 LTS，要求在根目录保存当前配置、依赖与 Windows SSH 恢复说明，供 Windows Codex 接手。用户只有现用系统卡，明确不安排整卡备份，工程由用户上传 GitHub。本轮主机时钟读数仍为 2026-10-15，原始读数留在证据中，不覆盖客户端日期或先前历史。

用户已再次完整观察启动过程并报告没有内核选项；因此不能将 §24.2 的“未注意菜单”继续作为最新状态，也未取得可用的 HDMI 菜单恢复证明。本轮 boot ID 为 `c98fb265-baa3-4306-b1bb-4fc22971415f`，仍运行 `6.1.0-1025-rockchip` / aarch64，sysfs 与 modinfo 的 RKNPU 版本均为 `0.9.7`。NPU `renderD129` 绑定 `fdab0000.npu` / `RKNPU`；原默认项及等待配置保持，extlinux SHA-256 仍为 `60a69ab93da2891ee3ee6fce6a253510036d46ef5f65176e48849491be36fc20`。没有启动此前安装的候选，本轮未发起重启。

只读补充盘点确认：有线连接使用 NetworkManager 的 `Wired connection 1`、`end1`，IPv4 manual、`192.168.137.2/24`，网关/DNS `192.168.137.1`；环境代理为该 Windows 地址的 HTTP 17897 端口，配置位于 `/etc/environment` 与 `/etc/profile.d/orangepi-proxy.sh`。没有读取 Wi-Fi 密码、GitHub 令牌或私钥。SSH 日志证实客户端 `192.168.137.1` 通过 RSA 公钥登录 `jzh`，服务监听 22；用户提供的 Windows `ssh -G orangepi5` 输出确认同一目标和默认 identityfile 候选列表。实际私钥文件路径未在 Windows 实机核验；完整恢复参数、路径和指纹区别集中维护在[迁移文档 §4](../MIGRATION_GUIDE.md#4-恢复网络与远程入口)。普通用户 `sshd -T` 因没有可读主机密钥而失败，不将默认密码策略写成已验证。

当前内存总量 8,307,392,512 B，1,073,737,728 B 文件 swap；根分区当时可用 18,550,231,040 B，资源会随运行变化。库/工具版本继续为 G++ 13.3.0、CMake 3.28.3、Ninja 1.11.1、libpulse 16.1、libsamplerate 0.2.2、libsndfile 1.2.2、yaml-cpp 0.8.0；本次补齐 nlohmann-json 3.11.3、Python 3.12.3、WirePlumber 0.4.17 与 glibc 2.39。已安装包的完整精确版本保存在本轮 `baseline.json`；新系统按自身发行版补依赖，不锁定这些旧版本。

PipeWire / pipewire-pulse / WirePlumber、SSH 和蓝牙服务均 active，Linger=no；本次音频枚举没有 bluez 耳机节点，默认输入为 monitor，不作为迁移后的录音 source。用户及 `/etc` 的 PipeWire/WirePlumber 配置目录未发现自定义文件，未发现工程专用用户服务；现有 local.yaml 保留原耳机意图。`deps/`、`models/` 只有占位文件，系统动态库缓存无 RKNN/RKLLM runtime 记录。当前工程含尚未提交/跟踪的音频实现，Git 上传需连同 CMake、源码、测试及文档一起处理；没有替用户暂存、提交或推送。

已创建根目录 [MIGRATION_GUIDE.md](../MIGRATION_GUIDE.md)：记录 Windows PowerShell 与板端 Bash 的操作边界、SSH alias/公钥/主机指纹、网络/代理、依赖包、耳机配置、构建/测试入口及 SDK 接手顺序。新镜像的 Rocket/RKNPU 区别与 WirePlumber 0.5 测试迁移边界见[兼容性记录](compatibility.md#重装-2604-的交接与核验边界)。未接收最终 26.04 镜像，未验证新系统编译、音频或模型；旧 P1-01、B01 的历史完成记录保留，P0-02 与 P1-02 继续进行中。

本轮本机证据位于被 Git 忽略的 `output/runs/new-image-handoff-2026-10-19/`：`before/` 保存修改前文档及当前交接 JSON，`baseline.json` 保存代码标识、系统/包/服务命令和保护文件哈希，`configuration-details.json` 保存网络/NPU 绑定及资产目录，`ssh-inventory.json` 保存 SSH 只读结果及用户补充，`verification.json` 保存交付检查。目录不会随 Git 克隆恢复；必要摘要已进入版本管理文档，不将新机缺少历史 output 视为工程损坏。本轮仅文档/交接同步，不重复运行应用构建或 CTest；未下载镜像、安装包、改动系统配置、录音或刷卡。

## 26. 官方 Jammy 系统网络时间与构建依赖恢复

2026-10-19（Asia/Shanghai），用户决定保留 Orangepi5_1.2.4_ubuntu_jammy_desktop_xfce_linux6.1.99，明确授权依次配置网络、时间和构建依赖。当前为 Ubuntu 22.04.5 LTS / 6.1.99-rockchip-rk3588 / aarch64，厂商 RKNPU 0.9.8，renderD129 已绑定 RKNPU 且普通用户可读写。此前各节为旧环境历史。

### 26.1 网络、时间与工程位置

- 普通用户 orangepi；工程恢复到 /home/orangepi/projects/rk3588_voice_engine，初始代码提交 6c1aecb726903bfb6fd8fc0334ca020f4154bb31。Windows 参考副本未修改，未提交或推送。
- NetworkManager 的 eth0 配置已落盘：自动连接启用、优先级 100，192.168.137.2/24，网关 192.168.137.1，DNS 223.5.5.5、119.29.29.29。Windows WLAN 2 → 以太网 ICS 重新应用后恢复转发，期间额外生成的 192.168.50.1 已清理。未执行重启测试。
- 复用 Windows 已有 RSA 公钥恢复 authorized_keys；Windows orangepi5 Host 单独增加 BindAddress 192.168.137.1，避免 VPN 路由干扰。主机指纹已用开发板照片核对；普通 BatchMode SSH 登录通过。
- 下载代理 http://192.168.137.1:17897 最小写入 /etc/environment，保留其他环境项；新 SSH 非交互会话访问 GitHub HTTPS 返回 200。本轮临时 profile.d 代理文件已撤下，避免重复配置。代理依赖 Windows 开机、代理运行并允许局域网访问。
- apt 使用 /etc/apt/apt.conf.d/80rk3588-proxy；华为 Ubuntu 源与 Docker 官方源使用 DIRECT。镜像原有阿里 Docker 源返回 403，改为同一仓库的官方地址，并使用 /etc/apt/keyrings/docker.asc 限定 signed-by；未安装或升级 Docker。公钥指纹为 9DC858229FC7DD38854AE2D88D81803C0EBFCD88。严格 apt 索引更新返回 0，TLS 和包签名校验保留。
- 时区 Asia/Shanghai，沿用 Chrony：先通过电脑时钟纠正日期，再由网络 NTP 同步，NTPSynchronized=yes、Leap status Normal。已保存 fake-hwclock，并写入 UTC RTC。
- 板端配置备份位于 /var/backups/rk3588-setup-20261019/；Windows SSH config 和 known_hosts 保留本次操作前备份。

### 26.2 构建依赖与实际验证

新增 CMake、Ninja、libpulse/libsamplerate/libsndfile/yaml-cpp/nlohmann-json 开发包及必要依赖，共 15 包，0 升级、0 删除。版本为 G++ 11.4.0、CMake 3.22.1、Ninja 1.10.1、Python 3.10.12、libpulse 包 15.99.1、libsamplerate 0.2.2、libsndfile 1.0.31、yaml-cpp 0.7.0、nlohmann-json 3.10.5。dpkg --audit 无问题，apt-get check 返回 0。

普通用户 Release 配置、cmake --build build -j2 均通过；build/bin/env_check 输出 ENV_CHECK_OK。ctest --test-dir build --output-on-failure -E '^audio_probe$' 为 8/8 通过，包括配置、资产、控制、JSONL、DSP 和独立构建边界验证。完整构建另注册的 audio_probe 集成测试需要 PipeWire/WirePlumber，本轮未安装其服务组件，因此未执行；不能把本轮结果写成完整 9/9。

### 26.3 后续范围与证据

当前仍为镜像自带 PulseAudio，蓝牙适配器已识别但无耳机节点；音频服务恢复、耳机双向录放及 A01/A02 待下一阶段。内核和 NPU 驱动未修改；系统 RKLLM 1.1.4、RKNN 2.3.0 未替换，尚未部署模型或运行推理。

官方 RKLLM 1.3.1（commit f7390530443bf84f0394255a449d7cbe81e69d1c）aarch64 runtime SHA-256 为 f25e9b099db08aaacd0a3ac62b4697d3951d6ae61ae41ea09f6702cfa89eb32c；其所需 GLIBC/GLIBCXX/CXXABI/GOMP/GCC 版本节点与板端 7 个依赖库逐项核对通过（最高 GLIBC 2.29、GLIBCXX 3.4.26，板端 glibc 2.35、GLIBCXX 3.4.30）。这只是 ELF 与符号版本前置检查，不是动态加载或推理验证；本轮未安装该 runtime。

证据在 Git 忽略的 output/runs/20261019-network-time-build/：setup-baseline.json、build-validation.json、配置/构建/env_check/CTest 日志、apt 更新/安装/包一致性日志及修改前文档。该目录不会随 Git 克隆恢复，文档保留必要摘要。未修改应用代码、默认模型目标或验收标准。


## 27. VS Code Remote-SSH 恢复（2026-10-20）

- 普通 SSH 公钥登录正常；失败发生在 VS Code Server 首次下载，远程 wget 失败后本地备用下载报 LocalDownloadFailed / Failed to fetch。原日志没有给出具体 wget 错误，不能把下载故障归因于账户、密钥或系统不兼容。
- 通过 VS Code CLI 重新打开 orangepi5 上的 /home/orangepi/projects/rk3588_voice_engine，匹配 VS Code 1.142.0 的服务安装成功。code --status 已确认远程主服务、扩展主机、文件监视服务和项目文件夹；不是仅验证 SSH 端口可达。
- 远程扩展日志另发现继承的 127.0.0.1:7897 代理连接失败；在板端 ~/.vscode-server/data/Machine/settings.json 将 http.proxy 设置为 http://192.168.137.1:17897。该地址依赖电脑和代理运行，Windows 本地 VS Code 设置未修改。未关闭 TLS 校验。
- 证据：output/runs/20261020-vscode-remote-ssh/result.json。官方启动程序下载 SHA256 已与官方响应头匹配；并未替换系统或修改 NPU 驱动。
- 附带发现：当时板端时钟落后于客户端日期，NTPSynchronized=no；本次 SSH 排查未调整时间。随后已按用户要求修复并验证重启自动同步，见第 28 节。所有扩展联网能力未做全面验收。


## 28. 重启自动校时修复（2026-10-20）

- 根因：Windows SharedAccess 服务为 Manual 且已停止。板端 HTTP 代理仍可用，但直连 DNS/NTP 不通；chrony 虽已启用，实际显示 16 个未知地址时间源。启动时 RTC 曾显示 2021 年，fake-hwclock 只能恢复上次保存值。
- 修复：Windows SharedAccess 改为 Automatic 并启动；HKLM\Software\Microsoft\Windows\CurrentVersion\SharedAccess 下 DWORD EnableRebootPersistConnection=1。沿用 WLAN 2 对开发板以太网的共享及 192.168.137.1 地址。DNS、ICMP 和 NTP 随之恢复。配置做法参考 [Microsoft ICS 排障说明](https://learn.microsoft.com/en-us/troubleshoot/windows-client/networking/ics-not-work-after-computer-or-service-restart)；该说明原针对 Windows 10 1709，本机效果以实际验证为准。
- 板端继续使用已启用的 chrony、原有 NTP 源、makestep/rtcsync 和 fake-hwclock；同步后保存软件及硬件时钟。遵照用户要求，没有新增检查脚本、校时定时任务或后台监控。
- 实际重启验证：boot ID 从 28568e40-9e4a-4361-87dd-95f596741b33 变为 79f65dc5-217b-486c-afa4-da6040a33a23。开机时间 14:26:16，chrony 于 14:26:40 自行选中 NTP 源；14:26:45 已显示 NTPSynchronized=yes、Leap status Normal、Asia/Shanghai。重启后没有手动设时或重启 chrony。
- 范围：已验证开发板重启后自动同步；没有重启 Windows，也未做断电冷启动或 RTC 电池保持测试。当前直连拓扑仍需要电脑开机联网且网络共享可用。
- 证据及配置前备份：output/runs/20261020-time-autosync/。没有修改模型、驱动、应用源码或提交推送 Git。

## 29. Windows Codex SSH 开发就绪复核

2026-10-20（Asia/Shanghai），从 Windows Codex 经现有 `orangepi5` SSH 公钥连接完成复核。当前普通用户为 `orangepi`，实际且可写的工程为 `/home/orangepi/projects/rk3588_voice_engine`；旧 `/home/jzh/projects/rk3588_voice_engine` 在本系统不存在。README 日常工作路径已修正，前文旧系统历史路径保留原意。

- **基础开发可直接开始。** 当前 Ubuntu 22.04.5 / aarch64 / `6.1.99-rockchip-rk3588`；C++17、G++ 11.4.0、CMake 3.22.1、Ninja 1.10.1、Python 3.10.12 及当前 CMake 所需开发库可用，具体依赖版本见 §26.2 和本轮 `dependencies.log`。SSH 用户对源码、配置、文档、构建与运行目录有写权限。
- **本轮实际构建与测试。** 在独立目录 `build/windows-codex-devcheck-20261020-145302/` 重新进行 Release 配置和完整编译，均返回 0；新生成的 `env_check` 输出 `ENV_CHECK_OK`，CLI `--help` 返回 0。CTest 注册 9 项，执行 `ctest --test-dir build/windows-codex-devcheck-20261020-145302 --output-on-failure -E '^audio_probe$'`，8/8 通过，包括配置、资产、控制、JSONL、音频 mock、DSP 和独立构建边界。`audio_probe` 集成测试依赖的 PipeWire / pipewire-pulse / WirePlumber 未安装，本次未运行，不能记为完整 9/9 或 A01/A02 验收通过。
- **真实设备与模型的剩余条件。** 当前 PulseAudio 正常，可枚举 ES8388 输入输出与 HDMI 输出；蓝牙服务和适配器已识别，但没有耳机音频节点。`config/local.yaml` 尚未恢复，默认模板的 source/sink/profile 为空，执行 README 中本机配置命令前须按实际设备恢复。`models/`、`deps/` 仍只有占位文件；系统 RKNN/RKLLM 库文件及 `rkllm.h` 存在，RKNPU 版本 `0.9.8`、普通用户可读写 NPU render 节点，但 SDK/模型配套、动态加载与实际推理未在本次验证。
- **资源、时间与联网。** 检查时根分区可用约 23 GiB，RAM 7.7 GiB、可用约 6.7 GiB、swap 3.9 GiB；`Timezone=Asia/Shanghai`、`NTPSynchronized=yes`。当前 SSH 会话访问 GitHub HTTPS 返回 200；本次没有验证 Git 推送认证。
- **工作区与范围。** 分支 `main`，HEAD 为 `6c1aecb726903bfb6fd8fc0334ca020f4154bb31`。开始时已有 `EXECUTION_PLAN.md`、`MIGRATION_GUIDE.md`、`README.md`、`docs/compatibility.md`、`docs/environment.md` 五份文档修改；本轮保留它们，仅补充环境记录及 README 当前路径/本机配置状态。47 份受保护的非 Markdown 跟踪文件哈希保持不变。未安装依赖、配置音频服务、录音、加载模型、提交或推送；Windows 参考副本未修改。

证据位于 Git 忽略的 `output/runs/windows-codex-devcheck-20261020-145302/`：`baseline.json` / `result.json` 保存代码标识、命令、退出码、构建目录、测试排除原因与文件哈希；独立日志保存系统/依赖/音频/NPU 枚举、CMake 配置、编译、环境检查及 CTest 输出；`before/` 保留本次修改前的 README 与环境记录。被忽略的本机证据不会随 Git 克隆恢复。

## 30. NPU runtime 指定版本更新（2026-10-20）

按用户明确要求，已落实 RKNPU **0.9.8**、RKNN runtime **2.3.2**、RKLLM runtime **1.3.0**。Windows Codex 经 SSH 在当前板端项目准备并验证；用户在板端终端执行 sudo 安装，返回 `INSTALL_OK`。原有六份文档修改保留，未操作 Windows 参考源码或提交/推送 Git。

- **驱动已达标。** `/sys/module/rknpu/version` 为 0.9.8，运行内核为 `6.1.99-rockchip-rk3588`；本轮无需安装驱动、切换内核或重启。
- **runtime 和头文件已更新。** 从官方固定提交下载两库及四个头文件，库版本字符串分别为 `2.3.2 (429f97ae6b@2025-04-09T09:09:27)` 和 `1.3.0 / 82bf4aa8e 2026-06-16 14:06:08`。来源、提交和库哈希集中见[兼容性版本锁](compatibility.md#指定-runtime-版本锁2026-10-20)，逐文件清单见 `deps/manifest.json`。
- **安装与备份。** 普通 SSH 无免密 sudo，由用户运行 `sudo python3 scripts/install_npu_runtimes.py --install` 完成系统安装。脚本校验原文件与下载哈希、运行内核/驱动和 NPU 空闲，先备份，再逐文件原子替换并刷新 ldconfig；异常自动恢复。更新 `/usr/lib/librknnrt.so`、`/usr/lib/librkllmrt.so` 以及 `/usr/include/` 下 `rknn_api.h`、`rknn_custom_op.h`、`rknn_matmul_api.h`、`rkllm.h`，均为 root:root / 0644。原有文件不属于 dpkg 包，已备份在 `/var/backups/rk3588-npu-runtimes-20261020/`；`backup.json` 保存原哈希/权限，`install-result.json` 记录 installed。
- **安装后验证。** 六个目标文件哈希与官方固定下载一致；`/lib` 和 `/usr/lib` 实路径一致。两库 ELF 为 aarch64，`ldd -v` 无缺失依赖；普通用户用同版头文件 C++17 编译检查程序，安装前私有候选、安装后系统绝对路径及默认 soname 的同进程加载、主要入口符号和 RKLLM 默认参数调用通过。默认解析路径和映射路径已记录。没有模型推理证据，P0-02 与 P0-04 不据此标完成。

安装脚本的 `--check` 为安装前检查（安装后会因原系统哈希已改变而拒绝，避免重复覆盖）。若需回退，在板端工程根目录执行：

```bash
sudo python3 scripts/install_npu_runtimes.py --rollback
```

回退验证备份和当前目标哈希，恢复原文件与权限，移除本轮新增且未被其他操作修改的头文件，再刷新 ldconfig。备份存在时脚本拒绝重复安装；发生中断时先检查备份中的结果文件，必要时回退，不删除备份强行重试。本轮没有实际执行回退，以保留用户指定的新版本。

本机证据（Git 忽略）：`output/runs/npu-runtimes-20261020/`，包括 `baseline.json`、`before/`、官方 tree/source/download 记录、`install-plan.json`、两库 ELF/依赖日志、`runtime_smoke.cpp`、编译/加载日志和 `verification.json`。依赖文件同样被忽略，`deps/manifest.json` 和安装脚本纳入可审阅工作区；恢复时需重新取得匹配哈希文件。此次未修改应用实现，未重复运行无关应用构建或音频测试。

## 31. 音频链路恢复

2026-10-20（Asia/Shanghai）。

### 31.1 恢复前检查

用户要求先恢复音频链路，并确认继续使用 vivo TWS 4、耳机已进入配对模式。预检查时系统仍运行镜像自带 PulseAudio；RTL8821C USB 适配器 `0bda:c820` 已识别，蓝牙 powered、无 rfkill 阻断，但无已登记的耳机设备。PipeWire / pipewire-pulse / WirePlumber / 蓝牙 SPA 插件尚未安装。

- 镜像 `/etc/pulse/client.conf` 将客户端固定到 `unix:/tmp/pulse-socket`，`/etc/pulse/default.pa` 也显式创建该 socket；后续切换必须同时处理客户端连接目标，不能只启动新服务。拟使用普通用户配置覆盖连接目标，保留系统原配置和可回退的旧 PulseAudio。
- 仅对缺少的 `pipewire pipewire-bin pipewire-pulse wireplumber libspa-0.2-bluetooth` 做 apt 模拟，结果为新增 9 包、升级 2 个 PipeWire 依赖库、删除 0 包；不主动升级已安装的 BlueZ。候选 PipeWire 为 0.3.48-1ubuntu3.2，WirePlumber 为 0.4.8-4，具体以实际安装日志为准。[Ubuntu Jammy WirePlumber 包](https://packages.ubuntu.com/jammy/wireplumber)
- `sudo -n true` 表明需要交互认证，已指导用户在自己的终端执行 apt 更新和上述安装命令；密码在终端输入。本节只记录预检查，不代表安装、服务切换、耳机配对、真实录放或 A01/A02 已通过。
- 预检查和备份保存在 Git 忽略的 `output/runs/audio-link-restore-20261020-150614/`：`preflight.json`、包安装模拟、原用户服务状态、蓝牙与 PulseAudio 枚举；`before/` 保存本轮前文档及不含凭据的 PulseAudio 配置。后续在同一目录追加实际安装、切换与验证结果。P1-02 保持进行中。

### 31.2 安装、服务切换与当前状态

用户在板端终端完成 apt 更新和安装，apt 记录为 15:07:05 至 15:07:16：新增 9 包、升级 2 个 PipeWire 库、无删除。实际 PipeWire / pipewire-pulse / SPA 为 `0.3.48-1ubuntu3.2`，WirePlumber 为 `0.4.8-4`；BlueZ 保持 `5.64-0ubuntu1.4`。

曾以普通用户 mask 原 PulseAudio，启用 PipeWire / pipewire-pulse / WirePlumber；三项 active，`pactl info` 实测为 `PulseAudio (on PipeWire 0.3.48)`。初次仅添加用户 client.conf.d 覆盖未被采用，客户端仍读镜像系统配置；改为显式 `~/.config/pulse/client.conf` 后，连接到用户 native socket 成功。首次失败及回退保存在 `audio-service-switch.json`，成功切换保存在 `audio-service-switch-final.json`。中间一次客户端配置检查曾在原服务的 native socket 上等待超时，后续已恢复。

用户询问新服务是否造成连接失败后，实际切回原 PulseAudio 对照：停止并 mask 新增的五个服务/socket，撤回本轮创建的用户 client.conf，unmask 并启动原 pulseaudio.service / pulseaudio.socket。`pactl info` 已恢复 `pulseaudio 15.99.1` / `unix:/tmp/pulse-socket`。**该阶段临时保留原 PulseAudio 诊断状态**；当时新包仍已安装，新服务为用户级 masked，并未卸载。新服务客户端配置保留在证据目录 `pipewire-client.conf.saved`，三份系统 PulseAudio 配置与本轮前备份哈希一致。以后切回 PipeWire 须先核对状态、撤销新服务 mask 并恢复所需用户客户端配置；本次临时对照不等于永久更改技术路线。同日 16:05 已恢复 PipeWire，后续状态见 §31.6 与 §32。

### 31.3 耳机与蓝牙对照结果

- 用户在桌面完成 vivo TWS 4 配对；目标 `1C:7A:43:7F:83:8F` 实测 Paired=yes、Trusted=yes、Blocked=no。曾短暂显示 Connected=yes，但未形成蓝牙音频节点，随后 Connected=no、ServicesResolved=false。未生成虚假的 local.yaml，也未开始耳机录放。
- 用户关闭手机及 Windows 蓝牙、重新入盒取出佩戴，并确认有电且距板端约 1 m；还重新进入配对模式、将 Realtek USB 适配器拔下约 5 s 再插回。上述操作后仍未稳定连接。用户另测手机连接与播放正常，测试后关闭手机蓝牙；没有备用耳机/音箱或适配器可做交叉测试。手机结果为用户报告，不是板端验收。
- PipeWire 与原 PulseAudio 下，对同一目标的 BlueZ Connect / A2DP ConnectProfile 均复现 `br-connection-page-timeout`；日志另有 HFP gateway 超时与 Host is down。临时启用控制器 discoverable 后可见 connectable 标志，仍未解决，已恢复 discoverable 原状态。用户重启了 bluetooth.service，进程由 916 变为 24101；重启后一次 Connect 在 15 s 内未收到 D-Bus 回复，仍无耳机 card/source/sink。不能把通用 NoReply 消息列举的若干可能原因当成已确认根因。
- 现有对照不足以把蓝牙超时归因于新音频栈，具体控制器/固件/射频或设备组合原因仍待定位。现有内核加载 `rtl8821c_fw.bin` / `rtl8821c_config.bin`，日志固件版本 `0x829a7644`；本轮音频操作未更换驱动/固件或重启系统。下一步需有针对性地采集 HCI 连接状态，或在具备设备时交叉验证，不据超时要求购置设备。

### 31.4 自动测试与未完成项

安装后构建和 ENV_CHECK_OK 通过；完整 CTest 为 **8 项通过、audio_probe 1 项失败**，原测试等待私有 source 超时。对旧版 PipeWire 的配置/权限差异做了独立试验：候选配置可推进私有服务启动，但后续虚拟流格式协商仍触发 DEVICE_WAIT，未通过全部原断言。候选未降低断言，未作为正式修复部署；`tests/integration/audio_probe.py` 已逐字节恢复到本轮开始时的内容，候选 diff 及失败日志保留在证据目录。

当前不能记成 9/9、耳机已可录放或 A01/A02 已通过，P1-02 继续进行中。音频操作保持生产源码、构建、配置模板及其余受保护非 Markdown 跟踪文件哈希不变。本节不改写同日在其他工作中完成的 NPU runtime 更新记录。

证据统一位于 Git 忽略的 `output/runs/audio-link-restore-20261020-150614/`：包/apt 日志、切换及原服务对照、连接与控制器复核、USB 拔插和 BlueZ 重启后的结果、原 CTest 及私有测试试验。`test-compatibility-experiment-status.json` 明确候选未部署；`before/` 保存本轮前文档、原测试及非凭据系统音频配置。没有采集配对密钥、PulseAudio cookie 或原始私有录音；未提交、推送或改动 Windows 参考副本。

### 31.5 HCI 结果与重新发现检查

用户完成 25 s 有界 btmon 采集并提供终端输出，板端 `hci-status.log` 已核对为 345 B。该次 `Create Connection` 的 Command Status 为 `Success (0x00)`，随后 Connect Complete 为 `Page Timeout (0x04)`。这说明控制器接受了连接命令，但此次基础蓝牙连接未建立，尚未进入音频协商；结合 §31.3 原 PulseAudio 下的复现结果，不能将故障直接归因于 PipeWire/WirePlumber。该状态码不能单独确定耳机状态、无线环境、控制器或固件中的具体原因。`hci-capture-status.json` 已更新为采集完成，原先检查时的空日志不再代表当前结果。

用户按指导在板端移除耳机配对后，报告重新搜索不到 vivo TWS 4。独立核验确认控制器 Powered=true、rfkill 无阻断、设备列表为空；普通用户启动实际发现扫描成功，18 s 内没有 Device1 新增或 RSSI 事件，扫描后设备列表仍为空。证据为 `rediscovery-after-remove.json`。本次扫描未发现设备，不等于已证明适配器硬件损坏。

后续已取得手机发现对照并恢复 PipeWire，见 §31.6；本节 HCI 记录保留此前那次失败的含义，耳机录放及 A01/A02 仍未通过。

### 31.6 暂停前补充结果与后续硬件更换

用户报告手机可在可用设备列表看到 orangepi5；随后板端经典蓝牙扫描实测发现 `Xiaomi 17 Max`，首个新设备事件约 0.54 s、RSSI −36 dBm。独立 LE 扫描还发现 vivo TWS 4，RSSI 约 −38 dBm。手机没有在本轮配对或传输音频。这些结果说明当时设备发现并非持续完全不可用，不将此前一次空扫描写成硬件损坏结论。

用户按两只耳机入盒、开盖、白灯持续闪烁的步骤操作后，板端重新配对成功并短暂 ServicesResolved=true；随后断开，原 PulseAudio 下 A2DP 连接仍返回 `br-connection-page-timeout`，取出佩戴和在配对窗口内连接也没有生成蓝牙音频节点。未取得成功配对瞬间的内核连接类型证据，不将该次配对擅自归类为 LE 或经典蓝牙。另一次有界 LE ATT 检查只观察到 pending（handle 65535 / state 5），未确认 LE 连接建立，也没有读取或写入特征值。

16:05 已完成原 PulseAudio 对照并恢复 PipeWire / pipewire-pulse / WirePlumber，三服务 active，客户端为 `PulseAudio (on PipeWire 0.3.48)`；旧 PulseAudio 服务/socket masked，用户 client.conf 恢复指向 native socket。之后一次重配前已移除目标旧记录，但重新发现未完成，因此不能继续声称此前 Paired=true 仍是当前状态。Wi-Fi rescan 对照和临时关闭 Wi-Fi 的 LE/经典发现对照均未重新发现设备；Wi-Fi 原 enabled 状态已恢复，SSH 路由仍为 eth0。不能据这些阴性结果确认 Wi-Fi 共存或省电为根因。

用户明确补充 USB 模块型号为 RTL8821CU。仅准备了有回退的 Wi-Fi 驱动隔离脚本，执行了 py_compile 和普通用户只读预检查；**未执行 --run、未卸载驱动**。随后用户要求暂停，已停止排查。该脚本针对旧 USB 设备，不适用于下一节的新模块。

以上证据仍在 `output/runs/audio-link-restore-20261020-150614/`：`phone-discovery-two-transports.json`、`headset-repair-open-case.json`、A2DP 连接检查、`headset-le-transport-check.json`、`pipewire-restored-after-comparison.json`、`rtl8821cu-wifi-radio-off-comparison.json` 等；其他阶段原始日志保留各自时点含义。

## 32. AP6275P M.2 模块安装后检查

2026-10-20（Asia/Shanghai），19:15 起经 SSH 检查。用户已拔掉 RTL8821CU USB 二合一模块，改装官方 M.2 模块，并明确确认标签型号为 AP6275P。19:15 检查阶段范围为识别、配置及启动前离线验证，当时没有修改启动文件或执行重启；随后授权启用及重启的结果见 §32.1～32.2。

- **19:15 检查时尚未启用，不能计为正常工作。** 旧 USB `0bda:c820` 已不在枚举中；`bluetoothctl list` 为空、`show` 返回 `No default controller available`，btmgmt 为 0 个控制器，sysfs 无蓝牙设备。网络只有 eth0 与 lo，PCI/SDIO 枚举为空。当前内核为 `6.1.99-rockchip-rk3588`、板型 Orange Pi 5，boot ID 为 `d73c6033-1cbf-40bf-a93f-1b311ae271ce`。旧 USB 在本次启动较早时段的识别日志不代表当前仍接着该设备。
- **服务与硬件识别分别核验。** bluetooth.service active；PipeWire / pipewire-pulse / WirePlumber 三项 active，原 PulseAudio inactive。该检查阶段没有控制器，因而没有执行蓝牙扫描、耳机连接或录放，A01/A02 继续未验收。
- **已确认缺少启用配置。** `/boot/orangepiEnv.txt` 没有 overlays 项；运行中 `/proc/device-tree/wireless-wlan` 与 `wireless-bluetooth` 的 status 均为 `disabled`，前者 wifi_chip_type 为 `ap6275p`。启动日志有 PCIe link fail；在节点未启用的情况下，不能单凭该日志判定模块本体故障或插接问题。
- **所需镜像文件已存在。** 当前内核配套 bcmdhd 模块、`rk3588-wifi-ap6275p.dtbo`，以及 AP6275P 的 Wi-Fi firmware/NVRAM 和 `BCM4362A2.hcd` 文件均可读取；这表示文件准备条件具备，不代表驱动已识别模块或固件已实际加载。
- **配置副本验证通过。** 已将镜像自带 overlay 用 fdtoverlay 应用到同版 Orange Pi 5 基础 DTB 的输出副本，返回 0；副本中无线和蓝牙节点均为 `okay`。启动环境预览仅追加 `overlays=wifi-ap6275p`，原 `/boot/orangepiEnv.txt` SHA-256 仍为 `7bc6713dbe577c5c908c4f1b6aa893223c47fc23220485a01e34c2fd2e1c745e`。预览不是实际启动结果。

19:15 检查后提出启用 `wifi-ap6275p`、重启并做真实扫描；该检查阶段未执行启用或重启，也未运行旧 RTL8821CU 隔离脚本。用户随后明确授权启用、重启和复核，结果如下；耳机连接及录放仍待验证。

本机证据（Git 忽略）：`output/runs/m2-wireless-check-20261020-191524/`，包括 `baseline.json`、`kernel-wireless.log`、`ap6275p-configuration-check.json`、`overlay-preview-validation.json`、反编译 overlay、`ap6275p-preview.dtb`、`orangepiEnv.preview.txt`、原启动文件和修改前文档备份。没有修改应用实现、系统驱动、固件、NPU runtime，未提交或推送。

### 32.1 启用与重启确认

2026-10-20（Asia/Shanghai），用户明确要求启用 AP6275P、重启后检查 Wi-Fi 和蓝牙。普通 SSH 用户不能免密 sudo，因此先生成并完成只读预检的 `enable_ap6275p.py`，由用户在板端终端执行 `sudo python3 .../enable_ap6275p.py --apply-reboot`。19:23:45 完成写入并发起重启；随后 SSH 重新连接，独立确认新的启动结果。

- `/boot/orangepiEnv.txt` 仅追加 `overlays=wifi-ap6275p`，其余启动设置保留。原文件备份为 `/boot/orangepiEnv.txt.before-ap6275p-20261020-191524`；原 SHA-256 为 `7bc6713dbe577c5c908c4f1b6aa893223c47fc23220485a01e34c2fd2e1c745e`，启用后为 `aa24fb7ca06a0e85448ba35dd123ebc718b0d3535e323da92e710534d93be7f5`。新文件及备份均保持 root:root、0664。
- 启动 ID 从 `d73c6033-1cbf-40bf-a93f-1b311ae271ce` 变为 `23c5113b-8204-4ac7-801a-9d91d36d079c`，已确认真正重启。内核仍为 `6.1.99-rockchip-rk3588`；运行中 wireless-wlan 和 wireless-bluetooth 节点均为 `okay`，wifi_chip_type 为 `ap6275p`。
- Wi-Fi 在 PCIe `0004:41:00.0` 枚举为 Broadcom `[14e4:449d]`，使用镜像自带 `bcmdhd`（PCI 绑定 `pcieh`；NetworkManager 报告接口驱动 `wl`）。固件和 NVRAM 加载成功，出现 `Firmware up` 及 `wlan0`，MAC 为 `40:D9:5A:63:E2:86`。
- 蓝牙出现 UART `/dev/ttyS9` 对应的 `hci0`，本次启动地址为 `7C:6D:24:C5:C8:77`，Broadcom manufacturer 15，控制器 Powered=yes。镜像自带 `brcm_patchram_plus` 正在使用 AP6275P 固件；启动参数包含 `--bd_addr_rand`，本次控制器地址不作为跨重启固定设备标识。
- 未更换内核、驱动、固件或 NPU runtime。原启动配置可由上述备份恢复；如需回退，应先核对后续是否有其他启动设置修改，再恢复原内容并重启。

### 32.2 Wi-Fi 与蓝牙实际扫描

19:26 起进行扫描。重启后 Wi-Fi 硬件已可用，但 NetworkManager 保留的软件无线开关为 disabled，导致 wlan0 暂为 unavailable。执行 `nmcli radio wifi on` 后恢复为 disconnected（可用、尚未连接热点），再执行 `nmcli -w 20 device wifi rescan ifname wlan0`，返回 0。

- **Wi-Fi 扫描通过。** 首次读取 6 个热点，后续缓存复核为 6 个；实际频率包含 2.4 GHz 与 5 GHz。NetworkManager 的 `WIFI-PROPERTIES.5GHZ` 字段仍报告 no，与已返回的 5 GHz 扫描频率不一致；这里仅记录扫描成功，不据此认定已验证 5 GHz 连接。未连接任何 Wi-Fi 热点，未测无线 DHCP、联网或吞吐。
- **蓝牙扫描通过。** `bluetoothctl --timeout 20 scan on` 返回 0，报告 `Discovery started`，本轮收到 6 个新设备和实时 RSSI 事件，包括 FlyDigi BS2、Mijia Scale S400。扫描结束后 Discovering=no、Powered=yes；rfkill 无软/硬阻断。该扫描证明附近设备发现可用，未进行目标耳机配对、经典蓝牙音频连接或录放验收。
- 当前 eth0 仍 connected，Windows SSH 路由仍走 eth0；Wi-Fi 软件开关已开启。bluetooth.service 与 PipeWire / pipewire-pulse / WirePlumber 均 active。
- **19:28 扫描阶段结论：AP6275P 已启用，重启后硬件识别及 Wi-Fi/蓝牙实际扫描通过。** 当时下一步为 vivo TWS 4 重新配对、音频连接与短录放（随后结果见 §33）；A01/A02、音频集成适配及完整语音链路继续待验收，P1-02 保持进行中。没有修改应用实现或默认设备配置，也未提交、推送。

新增本机证据仍在同一 run 目录：`enable-reboot-preflight.json`、`enable_ap6275p.py`、`enable-apply-result.json`、`post-reboot-baseline.json`、`post-reboot-kernel-wireless.log`、`post-reboot-network-diagnosis.json`、`wireless-scan-verification.json`、`bluetooth-discovery-clean.txt`、`wireless-final-state.json`、`post-scan-kernel.log`、`activation-verification.json` 及 `before-activation-sync/`。原 `verification.json` 为 19:19 启用前检查结果，保留其历史语义；脚本自身的 reboot_completed=false 表示脚本不能观察重启完成，实际完成由新启动 ID 的独立记录确认。

## 33. AP6275P 与 vivo TWS 4 播放录音短测

2026-10-20（Asia/Shanghai），19:30 起经 Windows Codex / SSH 执行。用户已在板端自行配对并连接 vivo TWS 4，明确要求测试播放和录音。结果：**系统播放及 10 秒耳机麦克风录音回放均经用户确认清楚正常，当前音频链路的短录放已恢复。** 本次不将短测计为 A01/A02 完整通过。

### 33.1 设备、profile 与系统录放

- 蓝牙地址仍为 `1C:7A:43:7F:83:8F`，Paired=yes、Connected=yes；新 AP6275P 控制器下，PipeWire / pipewire-pulse / WirePlumber 均 active，Pulse 客户端连接 `unix:/run/user/1000/pulse/native`。
- 初始为 `a2dp-sink-sbc`，显式向 `bluez_output.1C_7A_43_7F_83_8F.a2dp-sink` 播放三声低幅度提示音，paplay 返回 0；用户反馈“提示音清楚，已准备好录音”。
- 为使用耳机麦克风，执行 `pactl set-card-profile bluez_card.1C_7A_43_7F_83_8F headset-head-unit-msbc`，返回 0。出现实际耳机 source 与 sink，均为 s16le、16 kHz、单声道、未静音；profile 保持为 `headset-head-unit-msbc`。
- 当前可复用参数：source=`bluez_input.1C_7A_43_7F_83_8F.headset-head-unit`；sink=`bluez_output.1C_7A_43_7F_83_8F.headset-head-unit`；card=`bluez_card.1C_7A_43_7F_83_8F`。每次运行仍须核验在线状态及 profile，不能使用 monitor 或临时数字 ID。
- 用 parec 显式录制该耳机 source 的 PCM16 / 16 kHz / 单声道音频，取得 320000 字节、160000 样本，精确 10.000 秒；录音进程按计划停止，返回 0、无 stderr。RMS 为 -27.49 dBFS，峰值 -4.52 dBFS，削波样本 0。WAV 显式通过该耳机 sink 回放，paplay 返回 0；用户确认“能，回放清楚完整”。
- 本轮路由通过命令明确选择耳机，没有改变系统默认 source/sink，未建立 `config/local.yaml`；供项目复核的配置保存在本次 run 的 `audio-test.yaml`。录音为用户授权的本机测试材料，保留于 Git 忽略目录，WAV 权限为 0600，不纳入公开配置或版本库。

### 33.2 项目 audio_probe 双向短测与限制

使用已有 `build/bin/audio_probe`、本次显式设备配置，运行 `--duplex --duration-sec 10 --wait-device-sec 10`，未开启 `--record-wav`。程序到达 AUDIO_READY 并完成双向会话，退出 0 / duration_complete；运行期间 profile 为 mSBC，设备身份校验为实际耳机输入与输出。

- capture_overflows=0、capture_holes=0、invalid_capture_frames=0；playback_overflows=0、playback_underruns=0；device_lost=false、reconnects=0，software_counters_ok=true。
- 输出 500 帧；采集 469 帧、149971 样本，即 9.3731875 秒。active_capture_seconds=9.996048293，报告相对目标 10 秒的误差为 6.268125%，约少 0.627 秒。**短测退出 0 不等于采集时长完整性通过；差异原因该短测阶段未定位，当时提出先复核再做长时验收。用户后续要求暂不排查，直接继续下一步，见 §33.3。** 不将差异直接归因于蓝牙丢包或硬件故障。
- A01 的连续 10 min、时长误差阈值及其他完整条件尚未验收；A02 的三种状态重连未执行。此前新系统音频集成 fixture 的失败没有因本次系统短测而自动消除。P1-02 保持进行中；该阶段提出先复核采集时长，后续安排已按用户要求调整，见 §33.3。

本机证据（Git 忽略）：`output/runs/ap6275p-audio-test-20261020-193055/`。包含 `baseline.json`、`playback-and-profile.json`、`record-and-replay.json`、`user-feedback.json`、`audio-test.yaml`、`application-duplex-result.json`、`application/duplex-10s/{run.json,events.jsonl,summary.json}`、`final-state.json`、`verification.json` 与测试 WAV。配置及程序哈希见 application 记录。没有修改应用源码、默认配置、驱动、固件或 NPU 资产，未安装依赖，未提交或推送。

### 33.3 用户要求暂不排查并继续下一步

2026-10-20（Asia/Shanghai），用户明确要求“不用排查了，直接同步一下文档；开始下一步”。保留 §33.2 的实际采集量和误差记录，不将其改为已修复或已验收；本轮不为该差异修改源码或安排定位实验。同步入口与任务进度后，使用已确认的耳机节点直接开展下一步 600 s 双向稳定性和三种状态重连检查。完整验收状态仍取决于实际结果。

## 34. AP6275P 耳机长时稳定性及重连检查

2026-10-20（Asia/Shanghai），19:36 建立本机证据目录 `output/runs/audio-stability-20261020-193657/`。开始前确认 vivo TWS 4 Connected=yes，耳机输入/输出均为 s16le、16 kHz、单声道、未静音，profile 为 `headset-head-unit-msbc`，无其他 audio_probe 进程。

初始安排为 A01 非交互 600 s 双向录放及 A02 三种状态重连；随后用户明确要求“不要测十分钟，一分钟够了”，已按该指示停止原任务并使用已采集的一分钟窗口，不重复启动稳定性测试。使用本次目录的 `audio-test.yaml` 显式绑定耳机 source/sink，未启用 `--record-wav`，没有将长测录音保存到磁盘。应用源码及验收阈值未修改；本轮检查不计为 A01/A02 完整通过。

### 34.1 用户指定的一分钟检查结果

原任务于 19:37:35 启动；用户将测试改为一分钟时已运行约 66.98 s，立即向已核对命令行的本次 audio_probe 发送 SIGINT，19:38:42 正常完成收尾。实际运行约 67 s，没有继续至十分钟，也没有再启动另一轮一分钟测试。原 run 名 `a01-duplex-600s` 和原参数保留作历史证据，不表示完成了 600 s。

- 软件摘要为 exit_code=130 / reason=signal，符合用户要求的主动中止；不是完整 duration_complete 结果。active_capture_seconds=66.886212253，capture_samples=1055680；程序报告的原 600 s 目标误差不能用于评价此次一分钟检查。采集时长相关事实照常保留，未据此继续定位或修改实现。
- 从原始每秒事件提取连续 60.158376758 s 窗口，capture_native_frames 增加 957120，playback_frames 增加 3007；整个运行 profile 保持 mSBC，capture_overflows / capture_holes / playback_overflows / playback_underruns 均为 0，无设备断开或重连。SIGINT 收尾时丢弃 15 个尚排队的播放帧，未把停止后的 PCM 继续播放。
- 用户确认“正常，没有异常中断或杂音”。结论仅为本次约一分钟短时双向检查结果正常；A01 的连续十分钟及完整条件未执行，不调整原验收阈值。

### 34.2 空闲重连检查及恢复

之后仅执行一次 READY 空闲时的软件断开/重连检查，未继续录音和播放状态两项。断开后程序进入 DEVICE_WAIT；耳机一度自动重连，但 profile 为 `a2dp-sink-sbc`，仅有输出，没有配置要求的麦克风。后续音频卡和节点消失，虽然 BlueZ 仍显示 Connected=yes，程序也未恢复 READY。因此本次空闲重连检查未通过，A02 继续未验收。

为恢复用户刚才可用的耳机，将已配对的 vivo TWS 4 标记 Trusted=yes；普通 connect 及显式 Handsfree profile 连接未恢复音频节点，后者超时。用户将耳机取出充电盒后报告“我拿出来，从盒子里拿出来，它就自动重连成功了”。随后独立确认 Connected=yes、Trusted=yes、耳机音频卡重新出现，并将 profile 恢复为 `headset-head-unit-msbc`，耳机 source/sink 均重新为 s16le、16 kHz、单声道、未静音。

这是人工操作配合后的恢复结果，不记为原程序在目标设备可用后十秒内自动恢复的 A02 通过证据。未再次断开耳机，没有继续定位先前的采集时长偏差，也未为重连问题修改应用或蓝牙策略文件。

### 34.3 本机开发配置与交付状态

按本轮实际恢复的参数，从默认模板建立板端 `config/local.yaml`，仅填写耳机 source、sink 和 `headset-head-unit-msbc` profile；保留 16 kHz / 单声道 / 20 ms 及默认模型禁用状态。`voice_engine --config config/local.yaml --check-config` 返回 0，本机配置已确认被 Git 忽略，哈希见 `local-config-verification.json`。没有修改 `config/default.yaml`，也没有改系统默认输入输出路由。

交付时耳机双向节点已恢复，三个 PipeWire 服务均 active，没有残留 audio_probe 进程。本轮按用户要求完成文档同步和一分钟检查；十分钟稳定性测试不自动续跑，P1-02 的音频集成适配、重连可靠性及 A01/A02 完整验收保留未完成状态。

本机证据仍在 `output/runs/audio-stability-20261020-193657/`：`baseline.json`、`launch.json`、`a01-process.json`、`user-shortened-test.json`、`one-minute-window.json`、`one-minute-user-feedback.json`、`application/a01-duplex-600s/{run.json,events.jsonl,summary.json}`、`reconnect-result.json`、`application/a02-idle/`、音频恢复记录、`manual-reconnect-final-state.json`、`local-config-verification.json`、`verification.json` 及修改前文档。运行辅助脚本均在此 Git 忽略目录。未更换内核/驱动/固件/NPU 资产，未提交或推送。

## 35. 模型准备汇总

2026-10-20（Asia/Shanghai），按用户要求由三个 Astra / xhigh 子代理并行准备，主对话统一合并清单和进度。全部项目改动经 Windows SSH 落在板端 `/home/orangepi/projects/rk3588_voice_engine`。下载、依赖准备和编译并行；模型执行及 ASR 转换共用 `/tmp/rk3588_voice_engine_model_validation.lock`。

| 任务 | 实际结果 | 限制 |
|---|---|---|
| Qwen3.5-2B | 社区 W8A8 权重 2,942,188,412 B；SHA-256 `946daf4377bbf5913d8d5226180991b8e6b4a3068d8b5959bdc81a9492311a09` 与固定来源一致；官方 1.3.0 示例编译成功 | 用户要求仅下载校验，未初始化、问答、采集性能或验证取消；原始转换输入和完整校准资料未披露 |
| Zipformer | 三份 FP16 RKNN 已生成，官方 5.6115 s WAV 识别与参考文本完全一致；推理段 981.136 ms、全进程 1.855 s，峰值 RSS 160232 KiB，实际加载私有 RKNN 2.3.2 | 仅运行一次文件样例，未完成 A03 的五次基线、英文、增量流式、reset/取消、共驻或质量批测；原始 WAV 与补零后时长的 RTF 口径另见专属记录 |
| Silero VAD v5.0 | 静音 0 段、两段合成语音 2 段，官方 CPU 文件工具通过 | 只核验分段数量，未验收自然语音、噪声或边界精度 |
| MeloTTS FP32 | 两份 44.1 kHz / 单声道 / PCM16 WAV，时长 2.983764 / 6.060408 s，数字日期归一化日志正确 | Generate RTF 1.292/1.300，两例 RTF 大于 1，尚无 A05 预热达标证据；未试听；原包 INT8 是不可加载 LFS 指针 |

LLM 下载在 1,487,530,008 B 因 TLS EOF 中断，按用户指示续传同一文件后完成完整校验。用户随后明确要求不推理，故没有运行任何 LLM 模型或视觉编码器。详细来源、版本、命令、测量和缺项分别在 [LLM](model-prep-llm.md)、[ASR](model-prep-asr.md)、[VAD/TTS](model-prep-speech.md)，本节只作汇总。

公共模型清单现有 4 个 candidate 条目、50 条模型文件记录；依赖清单保留原 RKNN/RKLLM 并加入 sherpa/ORT，共 4 个依赖。所有文件记录大小与 SHA-256；现有 `voice_engine --check-config` 对每个模型及其直接依赖检查均通过。该检查不加载模型或打开设备，只验证资产完整性。原有本机配置中模型仍全部禁用。

本轮保留已有未提交修改，未修改公共 CMake、应用源码、`config/local.yaml` 或系统运行库/内核/驱动/音频配置；未提交或推送 Git。P0-02/03/04 继续进行中，M0、A03/A04/A05、模型共驻、流式及完整链路仍未完成。没有续做音频、蓝牙、自动重连或长稳测试。

本机证据（Git 忽略）：

- `output/runs/model-prep-llm-20261020-200730/`：来源、下载/续传、完整哈希、编译及模型片段。
- `output/runs/model-prep-asr-20261020-200730/`：版本锁、来源、转换和文件验证。
- `output/runs/model-prep-speech-20261020-201140/`：私有运行库、VAD/TTS 结果、WAV 及两份片段。
- `output/runs/model-prep-merge-20261020-200735/`：修改前备份、约束、合并输入哈希、逐模型资产检查及 `final-audit.json`；`manifest-merge-validation.json` 为完整性检查结果。

## 36. 固定测试集与文件模型探针

2026-10-20（Asia/Shanghai），按用户要求继续由三个 Astra / xhigh 子代理分别完成固定测试集、`asr_probe` 与 `tts_probe`；主对话合并公共 CMake、模型角色元数据、文件配置及文档。所有写入仍在板端唯一开发目录，保留原有未提交修改。LLM 本轮未初始化或推理；其静态问题文件只是测试资料。

### 36.1 固定工程集

[测试集说明](file-fixtures.md)及 `tests/fixtures/manifest.json` 已交付：20 条主体语音（普通中文 8、数字日期 5、短问句 4、中英混合 3）及 6 条边界，全部 16 kHz / 单声道 / PCM16。主体 WAV 为 2.000～3.308875 s，两条不足 2 s 的原始合成句只补尾部静音，原始时长与补零数量已记录。另有 10 条 TTS 文本、20 个未执行的 LLM 固定问题。

参考、关键词在首次 ASR 前冻结。全部 26 WAV 格式/哈希检查、9 项损坏反例拒绝、从保留的原生 WAV 重建后 26/26 哈希一致均通过。清单 SHA-256 为 `49cd7104e1eb635c4a428c6d52788b0694d3e4d0fd3caf513fd7288374472369`。

该集是同一 Melo 模型生成的 synthetic engineering regression，未录制用户声音、未试听；实际合成具有上游默认随机噪声，固定 WAV 哈希才是输入身份。15 s 边界由四次完整句重复及静音构造，不能称为自然连续 15 s 发言。它适用于实现回归，不替代自然人声独立质量验收，也不因改参考而消除失败。

### 36.2 VAD / ASR 文件结果

[ASR 探针](asr-probe.md)包含真实增量 fbank、RKNN encoder/decoder/joiner 缓存、reset/flush，以及 CPU Silero 512 样本聚窗。官方固定 WAV 5/5 输出精确匹配，预热 4 次 RTF P95 为 0.10975。

- 固定主体 20 条各 3 次：每批关键词全对 **13/20**；纯中文 CER **34/213 = 15.9624%**，未达到 18/20 与 CER ≤15% 的门槛。工具正确退出 6，A03 不记为通过。
- 主体后两批共 40 次预热 RTF P95 为 **0.20705**；26 条整体在 10/20/37 ms 分块下的 final、原始识别与 VAD 结果一致，重复/reset/finish 幂等检查通过。特征缓存峰值 105 帧，15 s 输入在 finish 前已执行 15 个 encoder 块，完整消费 240000 个样本。
- 空、静音、80 ms 短音频、低幅噪声均返回空；尾部语音最后词完整，但开头“请把”识别为“行吧”，严格全文参考检查失败，保留该失败。
- 关键词失败为 `zh_01/04/07/08`、`num_01`、`mix_02/03`。另用已编译的官方整 WAV 示例复核 `zh_07`、`zh_01`，输出与本次 adapter 原始结果逐字相同；这两条未观察到增量实现引入差异，不能外推其它失败原因。
- 批次最大 wait4 子进程 RSS 为 196948 KiB，采样 PSS 最大 193789 KiB，最小 MemAvailable 6470048 KiB；这些不代表全部 NPU 内存。实际 runtime 为私有 RKNN 2.3.2 / driver 0.9.8。

当前 VAD 和 ASR 同时接收原 PCM，VAD 仅决定最终空结果并记录分段；尚未接入实时 VAD 门控和音频状态机。默认 start_timeout/max_speech 的真实触发路径本批未覆盖，不宣称完整端点验收。

### 36.3 TTS 文件结果

[TTS 探针](tts-probe.md)使用固定 sherpa-onnx 1.12.14 C API / ORT 1.17.1-patched / Melo FP32，模型在整批驻留。十条固定文本覆盖普通中文、数字日期、标点和英文，先预热 1 条，再按固定种子乱序各运行 3 次。2/4 线程两组在统一锁下串行，采样时没有编译或 ASR 并行竞争。

| 指标 | 2 线程 | 4 线程 |
|---|---:|---:|
| 正式成功 / 失败；预热成功 | 30 / 0；1 | 30 / 0；1 |
| Generate RTF P50 / P95 / max | 1.299 / 1.326 / 1.350 | 0.843 / 0.883 / 0.894 |
| 完整处理 RTF P95（含分句、复制、转换、冲刷） | 1.341 | 0.899 |
| 首段可用 / 完整处理 P95 | 5.411 / 5.411 s | 3.455 / 3.455 s |
| 模型初始化（不含资产校验） | 8.982 s | 9.051 s |
| 峰值 RSS / 最小 MemAvailable | 575040 / 6091264 KiB | 570820 / 6101924 KiB |
| 退出码 / 性能门槛 | 6 / 未通过 | 0 / 通过 |

共 124 个 WAV（两组各 31 次、每次原生/转换各一份）已独立检查格式、非零音频、采样数、有效尾帧和 SHA-256，分位数亦重算通过。保留 44.1 kHz 原生单声道 PCM16，同时实际重采样为 16 kHz / 20 ms 帧，没有仅修改 WAV 头。首段时间是整段生成并转换后可用时间，不是流式首样本延迟。

后续文件测试显式使用 `--threads 4` 候选，通用线程默认值与本机音频配置未修改。`tts_07` 的全角冒号“：”被上游忽略，两组各 3 条警告已单列；词典外英文列表为空仍不证明发音正确。没有播放 WAV，数字/日期/英文发音及听感待用户确认，**A05 全项未通过**。同步 Generate 的超时只在返回后判定，硬限时使用外层 timeout；输出长度检查在上游已分配输出后发生，不能称为及时取消或底层硬内存上界。

### 36.4 统一集成与约束核验

两个目标为 OFF-by-default 的 CMake opt-in；`config/file-probes.yaml` 只供文件测试，LLM/KWS 禁用。公共模型清单只补充 ASR/TTS 文件角色，既有模型文件路径、大小、SHA-256 与 candidate 状态保留；依赖清单未变。两探针使用私有运行库，在初始化前核对真实加载路径及哈希；对 sherpa 旧 C++ 字符串 ABI 使用同版 C API 封装，ASR 官方非 PIC fbank 仅在该目标使用 `-no-pie`。

统一锁覆盖模型初始化、推理和采样的完整进程；适配器也检查自己或实际 flock 祖先进程持有指定文件的排他锁。无锁、共享锁、错误文件以及只有其它无关进程持锁均被拒绝。下载、源码编辑、编译不占模型锁，性能正式批次期间暂停编译。

统一 Release 构建 `build/model-probes` 通过，**15/15 非音频 CTest 通过**，包含原有配置/资产/控制/独立构建，新增 fixture 完整性、锁归属、VAD 窗口、ASR 指标及负例、TTS 分句/转换及 CLI。新系统已有失败的 `audio_probe` 集成项明确排除，未在本轮重跑或计为通过。最终二进制 `--help`、`ldd -r`、ENV_CHECK_OK、本机/文件配置检查通过；分别再用官方 ASR 一条、4 线程 TTS 一条做受锁短测，均退出 0，不混入正式性能分位数。

最终静态审计核对已有无关文件、Git HEAD、`config/local.yaml`、两份系统 NPU runtime 哈希不变；LLM manifest 条目和完整权重哈希不变，两个探针未链接 RKLLM。未更改系统/内核/驱动/音频，未执行录放、蓝牙、重连、长稳或 LLM 验证，未提交或推送。所有模型继续 candidate；P1-03 工程集已交付，P2-01/03 保持进行中，M0 仍受 LLM 未验证限制。

本机证据（Git 忽略；目录名仅作 run 标识，实际时钟以日志为准）：

- `output/runs/file-fixtures-20261020-210044/` 及其 `-rebuild/`：冻结前置文本、生成、9 项拒绝检查和逐文件重建校验。
- `output/runs/asr-probe-validation-20261020-212111/`：命令、源码/二进制快照、资源、跨分块汇总；对应 `asr-probe-official5-...`、`asr-probe-fixed20ms/10ms/37ms-...` 保存各批报告。
- `output/runs/asr-probe-native-compare-20261020-212111/`：两条失败的官方示例对照。
- `output/runs/tts-probe-20261020-211053/`：`thread2/`、`thread4/`、冻结二进制/源文件、`artifact-check.json`、`oov-warnings.json`；此前构建和无模型检查在 `tts-probe-20261020-211405/`。
- `output/runs/file-probes-merge-20261020-205226/`：修改前备份、基线、协调约束、默认/联合构建、15 项 CTest、最终二进制短测、`consolidation.json` 和 `final-audit.json`。

## 37. Qwen 下载来源与运行环境复核

本节保留本日先前的静态检查结论；随后用户授权的实际初始化和问答结果见[第 38 节](#38-qwen35-2b-最小正常推理验证)。

2026-10-21（Asia/Shanghai），按用户要求重新核对板端 Qwen 的实际来源及运行准备。下载记录确认当前唯一完整 2B 权重来自 Hugging Face 的 HanzoHuang 社区预转换仓库，固定 revision 与 2,942,188,412 B 文件的完整 SHA-256 一致。模型和 RKLLM/RKNN 依赖共 22 条清单记录复核通过；当前仍为 Ubuntu 22.04.5 / aarch64 / BSP 6.1.99 / RKNPU 0.9.8 / RKNN 2.3.2 / RKLLM 1.3.0。系统与私有 runtime、配套头文件一致，两个已有 ARM64 C++ 示例及 RKLLM 私有库的依赖解析无缺项，当前 SSH 用户具备 RKNPU 节点读写权限。

**官方 C++ 最小示例的已知运行前置条件已配齐，尚未实跑验证该社区模型。** 此次仅静态检查，没有启动 LLM、调用初始化/推理、占用模型执行锁、安装环境或变更系统；也未测试音频。模型内部兼容性、模板/EOS、性能/内存和取消仍未验证，来源披露限制与 candidate 状态不变；完整实时 LLM 接入仍待后续。

详细来源、检查项、资源快照及边界统一见 [LLM 静态复查](model-prep-llm.md#2026-10-21-来源与运行条件静态复查)。证据位于 `output/runs/llm-readiness-check-20261021-113001/`：`baseline.json`、`readiness.json`、两个示例及库的 linkage 日志、`before-docs/` 和 `documentation-check.json`。保留全部已有暂存和未提交内容，未提交或推送；没有任务状态变化，不重复改写 README 或执行计划。

## 38. Qwen3.5-2B 最小正常推理验证

2026-10-21（Asia/Shanghai），用户明确授权后，由 Astra / xhigh 子代理完成 Qwen3.5-2B 的初始化、三条简短中文问答和正常结束；主对话核对原始日志并统一更新公共清单、README、兼容性、验收状态与执行计划。仅执行一个实际模型进程，初始化一次，模型运行及资源采样约 7.582 s。共享排他锁 `/tmp/rk3588_voice_engine_model_validation.lock` 覆盖初始化、问答、销毁、进程回收和采样结束，结束后已释放，无模型残留进程。

最小正常流程通过：初始化返回 0，墙钟约 **2.999 s**；三条中文回答均收到一次 FINISH，run 返回 0、is_running=0、无错误回调，最终 destroy 返回 0（约 **0.882 s**），进程退出 0。首 token 延迟 **324.238～351.505 ms**，SDK generate 阶段速度 **10.727～11.532 token/s**。逐题回答、统计口径与原始命令集中在 [LLM 专属验证记录](model-prep-llm.md#2026-10-21-最小正常推理验证)。第一个问题来自新初始化进程，后两条复用模型、均不保留历史；未清文件页缓存，只有三个不同问题，不据此报告冷启动基准或 P95。

loader 实际报告 runtime **1.3.0**、driver **0.9.8**、模型内部 Toolkit **1.3.0**、目标 **RK3588 / W8A8 / 3 NPU 核**。maps 核实加载项目私有 `deps/rkllm-1.3.0/lib/librkllmrt.so`，路径与 SHA-256 已归档；原有系统及私有 SDK 版本未更换。实际请求上下文 2048、最多输出 128，未验证模型声明的 4096 上限。自定义模板关闭 SDK 的自动模板解析，本次非思考输出由显式闭合 think 后缀提供，不能仅凭 enable_thinking 参数推断自动开关生效。

getrusage 峰值 RSS **2,195,032 KiB**，/proc 采样 RSS/PSS 峰值 **2,195,576 / 2,192,761 KiB**（约 **2.094 / 2.091 GiB**）；最低 MemAvailable **5,314,940 KiB（约 5.069 GiB）**，swap 采样全程为 0。不同内存计数口径和采样时刻已分别保留，不能当作全部 NPU/驱动内存或模型共驻余量。没有新增内核错误或 OOM。

三条都观察到 token 248046 后正常 FINISH，未达到输出上限；公开 SDK 结果不提供显式停止原因或内部 EOS 配置，仍保留 eos_proven=false。模型继续 **candidate**：本次满足最小正常流程，不代表 A04/A08、M0 或正式 LLM adapter 完成。主动取消、完整固定问题集、反复初始化、长稳、共驻、实时引擎与音频均未执行；转换原始 revision 和完整校准资料仍缺。

本次新增私有探针、CMake 入口与运行器位于 `deps/model-prep/llm/minimal-20261021/`，未修改公共源码或 SDK。首次私有编译补充 `<cstddef>` 后通过；首次运行尝试因 `/usr/bin/time` 缺失在 exec 前终止，未启动模型。此前专属文档误写 GNU time 已存在，现已按静态 JSON 更正；运行器改用 /proc / getrusage，无需安装软件。上述失败与修正均保留证据，成功运行不混入失败尝试的耗时。

更新后的清单通过 `voice_engine --check-config` 资产检查（1 个模型、1 个直接依赖、15 条文件记录，退出 0）；使用本次证据目录内独立 YAML，不加载模型或打开音频。公共模型清单仅更新 Qwen 当前验证元数据，原有文件路径、大小、SHA-256、来源和 candidate 状态保留；`models/llm/qwen3.5-2b/provenance.json` 仍作为 2026-10-20 准备阶段快照，不覆盖历史。其它模型条目和依赖清单不变，本机 `config/local.yaml` 中模型仍禁用。已有暂存内容、系统运行库、内核、驱动和音频配置均保留，未提交或推送。

本机证据（Git 忽略）：

- `output/runs/llm-minimal-20261021-113633/`：私有构建及失败修正、执行命令、源码/二进制哈希、`review.json`、`delivery-checks.json` 和证据清单；成功运行的日志、锁证据、maps、资源和返回值在 `normal-attempt-2/`。
- `output/runs/llm-minimal-merge-20261021-113529/`：修改前备份、系统与 Git 基线、独立核对的 `consolidation.json`、公共清单资产检查及最终一致性审计。

## 39. GitHub CLI 安装与板端重新认证

2026-10-21（Asia/Shanghai），用户明确要求登录板端 GitHub。检查时当前系统未安装 gh，也没有当前用户的 gh 登录配置；此前文档中的认证记录保留为历史。已安装官方 GitHub CLI 2.102.1 Linux arm64 发布包，下载后核对官方 checksums 文件中的 SHA-256，通过后安装到 `/home/orangepi/.local/share/github-cli/2.102.1/bin/gh`，入口为 `/home/orangepi/.local/bin/gh`。未使用 sudo 或修改系统软件包。

- 在用户 `.bashrc` 前增加无输出的用户 bin PATH 设置，使新的 SSH 命令可直接运行 gh；原文件备份为 `/home/orangepi/.cache/rk3588-github-login/2.102.1/bashrc.before-20261021-114937`。
- 由用户在 GitHub 网页完成设备授权，板端独立调用 API 确认当前账号为 **jzh03**，gh auth status 返回 0。已执行 gh auth setup-git，HTTPS Git 凭据助手配置完成。
- API 确认仓库 `jzh03/rk3588_voice_engine` 的 push 权限为 true；本轮仅核验权限，没有执行 commit、push 或 pull，也未调整 Git 作者身份或 origin。
- SSH 环境下 CLI 报告凭据回退保存在当前用户配置文件。目录 `/home/orangepi/.config/gh` 已设为 0700，hosts.yml/config.yml 为 0600。令牌内容未输出、未复制到工程或文档。

本机证据（Git 忽略）：`output/runs/github-login-20261021-115145/auth-result.json`。记录官方发布来源、包和二进制哈希、账号与仓库权限、配置文件权限；不含设备码、密码或令牌。本轮认证操作没有修改或暂存项目代码，保留原有暂存及未暂存内容。

## 40. 提交作者身份配置修复

2026-10-21（Asia/Shanghai），用户在 VS Code 提交时遇到 `fatal: no email was given and auto-detection is disabled`。板端检查确认有效 user.name、user.email 均未设置；分别用 `git -c user.useConfigOnly=true var GIT_AUTHOR_IDENT` 和 GIT_COMMITTER_IDENT 复现相同错误。GitHub CLI 已登录只解决远程认证，不自动补齐提交作者身份。

在板端当前仓库以 local scope 补齐 user.name=`jzh03`，user.email 使用用户明确提供的邮箱（实际值保存在本机 `.git/config`，不在公共文档重复记录）。没有修改全局身份或关闭严格身份检查。修复后上述作者和提交者检查均返回 0，配置来源为本仓库 `.git/config`；GitHub 登录状态仍返回 0。

同时确认 `refs/remotes/origin/main` 存在于 packed-refs，Git rev-parse 可正常解析到 `6c1aecb726903bfb6fd8fc0334ca020f4154bb31`。VS Code 直接读取 loose ref 文件得到 ENOENT 的日志不表示该远程分支丢失，本轮没有 fetch、重建引用或修改 origin。

本机证据（Git 忽略）：`output/runs/git-identity-fix-20261021-115530/identity-verification.json`。现有暂存条目的完整摘要与 HEAD 在修复前后相同；没有执行提交、推送或修改暂存区。README 已补充提交作者配置与 GitHub 认证的区别。


## 41. P0 剩余任务核查与十问取消验证

日期：2026-10-21（Asia/Shanghai）。本轮按用户要求由三个 Astra / xhigh 子代理分别核查 SDK、模型资产及运行 LLM，主代理合并公共文件。以板端 `orangepi` 用户、`/home/orangepi/projects/rk3588_voice_engine` 为唯一工作副本；保留已有 README 和本文件中的 Git 身份修复记录。开始时 HEAD 为 `702ef17ffa9f87830168a013ef65b3d8e883f362`，没有暂存改动。

| 任务 | 本轮后状态 | 依据及具体缺项 |
|---|---|---|
| P0-01 | 已完成 | 复用当前系统有效的 C++17/CMake/pkg-config、Release 和环境检查证据；无须重跑无关构建 |
| P0-02 | 已完成 | 固定 SDK、官方例程、驱动前置、头文件、ABI 及实际加载证据闭环；见[兼容性记录](compatibility.md#p0-02-配套闭环复核2026-10-21) |
| P0-03 | 进行中 | 文件齐备；Qwen 实际转换输入/命令/校准/embedding 来源，Silero 历史 ONNX 导出工具/命令，Melo 原始权重/导出链及字典归档历史仍未齐全 |
| P0-04 | 进行中 | 官方 ASR WAV 5/5 复用；LLM 十问、一次主动取消及恢复已执行，但短答复核仍有一题能力声明失实，转换来源未闭环，未冻结 verified 组合 |

**P0/M0 未完成。**以上状态沿用原条件；完整 A04/A08、正式 adapter、同进程共驻、实时对话与长稳不是本轮完成项，所有模型继续为 candidate。ASR 固定工程集三批各 13/20、中文 CER 15.96% 以及 TTS 尚未试听的既有缺项不变，也不混作本轮新增失败。

SDK 子代理核对 54 条当前哈希记录（其中 48 条已登记于依赖清单），系统两库和四个头文件 6/6 与对应私有副本一致；40 个小型源码文件与固定来源匹配，11 组 ELF/ldd-r 检查均为 AArch64、无缺库/未解析符号。当前 GCC 11.4 / glibc 2.35 满足已记录的符号版本要求。RKLLM 1.3.0 固定英文手册提供直接的库/头文件、GCC ≥10.2 和驱动 0.9.8 依据，历史 1.3.1 手册不冒充当前 SDK 来源。证据、ABI 明细和私有 BUILD_RPATH 部署限制集中在兼容性记录。

资产子代理检查原有 4 模型 50 条文件记录和 4 依赖 110 条记录：路径/大小无漂移；120 个小文件本轮计算 SHA-256，40 个大文件复用已有完整哈希及当前元数据，不宣称全部重新哈希。主代理补入 4 个小型 TTS 来源/许可参考文件（17,012 B），清单现为模型 54 条、依赖 110 条，共 164 条文件记录；7 个模型依赖版本关系匹配。FST 固定来源内容相同、CppJieba MIT 参考已补，后者不证明历史字典归档身份。ASR 的本地 FP16 转换来源齐全；另外三类未披露的实际生成链保持未知，见[LLM 来源缺项](model-prep-llm.md#2026-10-21-p0-十问取消与短答复核)和[语音资产复核](model-prep-speech.md#2026-10-21-p0-03-资产复核)。`deps/manifest.json` 的既有版本及文件记录无需修改。

LLM 复用官方最小示例的私有副本，仅 LLM 子代理运行模型；两个短进程全程持有公共 `flock`，耗时分别 83.517 s 和 29.214 s，性能窗口无其他代理编译或大文件校验。首轮 10 问评分 6/10，存在截断和单位换算矛盾；一次通用单句短答系统提示修正后，原问题、64-token 预算及四维评分保持不变，同十问为 9/10，剩余 llm_01 错称能实时识别/朗读文字、播放音乐。复核十条均有结束标记、FINISH、run 返回和空闲确认，未见截断；保留首轮失败，不把后者宣称为独立泛化验收。

首轮执行唯一一次主动取消：请求至 run 返回 102.011 ms，至确认空闲 102.016 ms，低于模型取消 2 s 条件；请求后有一个尾 token。确认空闲并显式清 KV 后，同句柄恢复回答“恢复成功。”，无旧轮标记或跨轮回调。短答模板没有第二次取消复测；本轮未测试软件 PCM 停止或耳机尾音。逐事件时间、实际命令、逐题输出和评分均在[LLM 专属记录](model-prep-llm.md#2026-10-21-p0-十问取消与短答复核)。

两次初始化分别 31.258 s、3.124 s；首轮首 token 339.591～411.325 ms，短答复核 792.238～4953.233 ms（保留 llm_05 异常值），对应 SDK 生成速度分别 10.742～11.338、6.863～11.290 token/s。模板输入长度不同，缓存/调度未控制，不推断为严格冷/热启动或单变量性能改善，也不据十条样本关闭 P2 性能验收。采样 RSS 峰值最高 2,195,792 KiB（约 2.09 GiB），最低 MemAvailable 5,252,200 KiB（约 5.01 GiB），swap 使用 0；RSS 不包含全部 NPU/驱动分配。两次均无新增 NPU/OOM 错误，各有一条 Wi-Fi 扫描信息，退出码 0，锁释放且无残留模型进程。

板端证据目录（均为被 Git 忽略的本机材料）：

- 主代理合并：`output/runs/p0-closeout-20261021-144042/`，含开始快照、复用证据、`consolidation.json`、合并记录及 `final-audit.json`。
- SDK：`output/runs/p0-sdk-20261021-144205/`，含 `report.json`、证据索引与交付检查。
- 模型资产：`output/runs/p0-assets-20261021-144234/`，含来源/完整性报告和补充来源文件。
- LLM 原十问、取消和恢复：`output/runs/p0-llm-20261021-144257/`。
- LLM 同十问短答复核：`output/runs/p0-llm-20261021-145026-short-answer/`。

主代理逐项核对原始回调、终止/取消事件、评分、速度计算及报告一致性；公共源码、构建入口、原固定题库、`config/local.yaml`、系统 SDK 和音频环境保持不变。最终静态审计检查清单结构/文件大小/路径/依赖、补充文件哈希、文档链接、`git diff --check` 以及用户已有修改的保留情况。没有提交、暂存或推送；本轮修改和忽略产物仅保留在板端。

## 42. Qwen 私有提示、采样对照与 prefill 定位

2026-10-21。三个Astra / Max子代理按用户授权分工；唯一模型执行者在15:48:08～15:52:06持同一flock串行运行，期间无并行编译或大文件校验。保持模型及0.9.8 / 2.3.2 / 1.3.0组合，未接正式adapter。

五组各两批，正反序、每进程固定原题预热一次后执行原十问，共100正式＋10预热。协议、原题/评分和单变量先冻结。10次初始化/释放和退出均成功，110次清KV返回0且大小0，实际参数、原始token和库路径独立核查通过。

| 配置 | 两批原四维评分 | TTFT 中位 ms | 全部回答返回中位 ms | SDK 生成 tokens/s 中位 | 内容复核 |
|---|---|---:|---:|---:|---|
| L：原长提示 | 9/10；9/10 | 770.245 | 1935.878 | 11.649 | 两批均夸称音频和音乐能力 |
| S：精简能力提示 | 10/10；10/10 | 436.902 | 1785.972 | 11.462 | 两批均无依据自报知识截止2026年 |
| K：S＋top_k=20 | 6/10；10/10 | 437.198 | 1889.721 | 11.494 | 首批拒答、数量、算术及天气失败 |
| R：S＋repeat_penalty=1.0 | 10/10；10/10 | 437.489 | 1641.532 | 11.599 | 本固定集未见上述额外事实问题 |
| P：S＋presence_penalty=2.0 | 10/10；10/10 | 435.337 | 1776.863 | 11.590 | 两批均无依据自报知识截止2026年 |

R仅作私有候选；S/P共四条无依据知识截止年份、K首批四项失败完整保留。原四维94/100、语义完整99/100，无可见截断。L→S输入86～90降至56～60tokens，TTFT中位降低43.3%，同题配对差中位−335.399ms；平均输出反而更长，生成速率约11.5tokens/s。R相对S同题回答耗时差中位+1.335ms，未证明采样参数额外明显提速。

历史4.95s主要在SDK prefill段，新100条最大prefill857.257ms；旧离群未复现、根因未定。681.308ms样本有线程等待相关线索，不能直接归因或回推历史。遥测RSS/PSS峰2,195,548/2,192,909KiB，最低MemAvailable5,273,624KiB、swap0；964次采样加权CPU4.746%单核，无deadline overrun，开销不能当零。热限频未获确认。完整调度字段更正/热策略/缺少PID I/O等边界见[遥测报告](../output/runs/llm-tuning-20261021-153941/telemetry/report.md)。

[LLM记录](model-prep-llm.md#2026-10-21-私有提示采样对照与-prefill-定位)包含提示、参数、实际命令和结果。证据 `output/runs/llm-tuning-20261021-153941/`；私有源码 `deps/model-prep/llm/tuning-20261021-153941/`。主代理统一合并六个公共文件，deps清单、模型/SDK、原fixture、公共源码/CMake、local配置、参考总结与历史记录保留；无提交/推送。

**本轮私有实验完成；P0-01/02完成，P0-03/04仍进行中，P0/M0未完成。** Qwen转换输入/recipe/校准embedding及Silero/Melo历史来源仍缺，模型candidate。20题×3批、多轮/长输入/超时、完整A08、共驻、实时和长稳未验收；新候选模板未重做取消，旧配置证据不改写为新配置通过。

## 43. P0 收尾与首个已验证组合

日期：2026-10-21（Asia/Shanghai）。唯一工作副本仍是板端 `/home/orangepi/projects/rk3588_voice_engine`，通过现有 `orangepi5` SSH 操作。用户授权 Astra / Max 子代理完成选定 R 的一次主动取消及恢复、可取得来源核查和独立复核，主代理统一合并；随后用户再次明确 LLM 直接使用下载的预转换模型，无需继续追查转换步骤。

**P0-01～P0-04 已完成，M0 技术可行通过。** 该结论覆盖计划原定的 SDK 最小基线；不改变完整 A01～A14、P2、M1～M3 的输入、评分和阈值。§41/42 的当时未关闭状态作为历史保留，以本节作为当前进度。

| 任务 | 当前状态 | 完成依据 |
|---|---|---|
| P0-01 开发/构建基线 | 完成 | 复用新系统 Release、env_check、8 项通用检查的有效证据 |
| P0-02 SDK 配套 | 完成 | 固定驱动 0.9.8、RKNN 2.3.2、RKLLM 1.3.0；官方例程、头文件/ABI、实际加载闭环 |
| P0-03 模型资产 | 完成 | 四类模型、配套资源及四依赖的身份/大小/哈希/版本/契约已记录；社区预转换历史未知留档 |
| P0-04 最小推理/M0 | 完成 | ASR 官方 5/5、CPU VAD/TTS 最小证据及 R 固定十问两批 10/10 复用；R 本轮取消/恢复通过，首个组合已冻结 |

新的唯一模型执行于 16:17:46～16:17:52：选定 R 参数/模板不变，生成中取消请求至 run 返回 97.463 ms、确认空闲 97.467 ms，满足原 2 s 条件。仍收到 1 个尾 token“的”，该事实保留。清 KV 90→0 后，同句柄回答“恢复成功。”，恢复 TTFT 460.100 ms、完整耗时 751.676 ms、SDK 生成速度 10.278 tokens/s。一次 init、两次 run、一次 destroy 和进程均正常完成，无 ERROR/过期回调、NPU/OOM 新错误或残留。采样 RSS 峰值 2,195,960 KiB，最低 MemAvailable 5,279,600 KiB、swap 0；资源和单样本边界见[LLM 专项](model-prep-llm.md#2026-10-21-r-配置取消恢复与-p0-收尾)。没有追加十问、ASR/VAD/TTS 或长稳运行。

来源核查分开记录“发布产物可固定”与“完整历史转换可复现”。新增 Melo 发布者固定 HF 快照及现存 ONNX 自述 producer 信息；Qwen/Silero/Melo 的未公开历史字段仍为 unknown。用户明确接受社区预转换 LLM，不再追查其转换步骤，也不以这些未公开细节阻塞本次 P0；没有编造、换模型或声称完整历史已补齐。许可/组件边界和具体来源集中于[来源报告](../output/runs/p0-r-closeout-20261021-161053/sources/report.md)。164/164 清单文件 stat/大小匹配，160 条沿用旧全量哈希及未变 mtime/ctime，4 个后补小来源文件校验通过；不是本轮重新全量哈希权重。

`models/manifest.json` 与 `deps/manifest.json` 的四模型/四依赖改为 `verified`，`run_id=p0-r-closeout-20261021-161053`，原准备 run_id 另存；所有文件路径/大小/SHA、原来源标识和版本保持不变。`verification_scope` 明确只覆盖 P0/M0 单模型最小可行性，该扩展字段由人工审阅、程序不强制验收范围。`selected-candidate.json` 保持历史选择快照，清单新增本次验证结果。冻结记录见 [verified-combination.json](../output/runs/p0-r-closeout-20261021-161053/verified-combination.json)。

运行共用 `/tmp/rk3588_voice_engine_model_validation.lock`，覆盖完整验证进程；性能窗口内没有并行编译或大文件校验。主代理和独立复核均从原始事件重算时序/字节/资源，旧十问评分也逐项复核。本轮仅在私有例程编译，公共源码/CMake、local 配置、原固定题目/评分、参考总结及旧证据不改；本轮公共修改只涉及文档和两份清单，没有提交或推送。

剩余工作按原阶段继续：ASR 固定集质量未达标；正式 LLM adapter/20题×3批、多轮/长输入/超时未完成；TTS 仍待试听/发音确认；同进程共驻、完整 A08、实时链路、音频集成和长稳尚未验收。单次模型取消不是 PCM 停止或耳机尾音指标。历史 4.95 s prefill 未复现且根因未知，不宣称已修复。

本轮证据根目录：`output/runs/p0-r-closeout-20261021-161053/`。入口为[收尾报告](../output/runs/p0-r-closeout-20261021-161053/report.md)、[执行报告](../output/runs/p0-r-closeout-20261021-161053/runner/report.md)、[来源报告](../output/runs/p0-r-closeout-20261021-161053/sources/report.md)、[独立最终复核](../output/runs/p0-r-closeout-20261021-161053/review/final-review.md)与[最终变更审计](../output/runs/p0-r-closeout-20261021-161053/final-audit.json)。

## 44. P1 自动化回归与固定集校验加固

2026-10-21～22（Asia/Shanghai）。按用户要求由两位 Astra / Max 子代理分别处理 P1-02 隔离音频集成和 P1-03 固定集校验，主代理统一审查、构建、回归与文档同步。用户随后确认“本轮先完成代码与自动化测试，A01/A02 实机验收继续待办”。代码和子代理验证于 10 月 21 日完成；中断后于 10 月 22 日完成统一回归，沿用 `output/runs/p1-progress-20261021-164343/` 证据目录。

开始时板端 `main` 工作区干净，HEAD 为 `0d32be5d1aacf577ef61f3fb7e40a70582a0fd5b`，领先远端 1 个既有提交。本轮只修改 CMake 测试注册、三个集成测试/校验脚本及关联文档；生产 `src/`、`include/`、`tools/`、本机/默认配置、固定语料、生成器和两份资产清单保持不变，未提交或推送。

### 44.1 隔离音频测试修复

原样复现的旧测试在私有 source 等待阶段失败。PipeWire 0.3.48 下，`access.force=unrestricted` 不能代替权限代理，旧 Pulse socket 配置也未形成测试所需的连接入口。修复连接后进一步观察到 `audiotestsrc` 的初始 44.1 kHz / 双声道格式会在连接策略配置端口时变化，触发后端既有的格式变化取消保护。原失败及逐步诊断保留于 `output/runs/p1-audio-20261021-164401/before/` 与 `trial-*`，未覆盖历史音频失败。

`tests/integration/audio_probe.py` 改为正常 host access policy、Pulse 模块参数及私有绝对 socket，以 `module-remap-source` 提供稳定 48 kHz / 单声道合成麦克风；合成驱动使用 `node.pause-on-idle=false`，保持多次采集间的数据流。新增双方原生格式断言，原有 RMS、队列、显式设备拒绝、取消、恢复和信号断言均保留，未放宽生产后端的设备身份或格式检查。

测试只有临时 PipeWire/Pulse/WirePlumber 服务、合成输入与空输出，禁用硬件监控及会话/系统 DBus，不接触耳机或系统路由。最终隔离集成涵盖 duplex、capture-only、playback-only、WAV 转换/排空、取消后新 generation、私有服务重启后显式 start 和 SIGINT/SIGTERM。子代理证据为 `output/runs/p1-audio-20261021-164401/validation/test.log`，对应 `artifacts/runs/` 保留 12 份摘要/事件及合成 WAV，认证 cookie 未归档。本次只验证本板的 0.3.48 组合，不外推未测试的 PipeWire 版本或物理蓝牙恢复。

### 44.2 固定集校验加固

修复前，长语音参考追加不存在的文字，以及修改噪声 PCM 后同步更新哈希，均被旧校验器接受；根 JSON 为数组则异常返回 1。现在补充源句/关键词关系、精确固定噪声、边界构造和补零一致性、严格类型与路径检查，错误使用约定退出码 2。详细契约及追溯限制见[固定测试集](file-fixtures.md)，未改原参考、关键词、WAV 或评分门槛。

新增 `fixed_fixtures_negative` CTest，包含 2 个正例和 33 个负例，覆盖内容篡改后更新哈希、重复 ID、结构错误、路径逃逸、截断和类别覆盖。原 26 WAV 校验通过；manifest SHA-256 仍为 `49cd7104e1eb635c4a428c6d52788b0694d3e4d0fd3caf513fd7288374472369`。原样反例、校验与未改文件记录见 `output/runs/p1-fixtures-20261021-164412/{before-negative-checks.json,check-fixtures.json,negative-checks.stdout.json,delivery.json}`；最终加强的重复 ID 断言由下述联合 CTest 验证。

### 44.3 统一验证与剩余项

| 检查 | 本次结果 |
|---|---|
| 默认及 ASR/TTS 联合 Release 配置/构建 | 通过，沿用板端 C++17 / CMake / Ninja 和 `-j2` |
| 联合完整 CTest | **17/17 通过**，0 失败，CTest 总耗时 62.77 s；包含音频集成和新负例 |
| 独立构建边界 | engine-only、mock-audio-only、native-audio-only 的构建及各自测试通过 |
| 环境与本机配置 | `ENV_CHECK_OK`、`CONFIG_OK`；模型仍全部禁用，资产计数均为 0 |
| 保护范围 | 运行前后已测代码哈希一致；生产代码、固定集/生成器、配置和两份清单未改；本次恢复工作前后的用户音频服务 PID 相同 |

统一命令、退出码和时间见 `output/runs/p1-progress-20261021-164343/verification.json`，完整输出为同目录 `ctest-full.log`、各构建日志、`environment-check.log` 和 `local-config-check.log`。`tested-source-sha256.json` 与 `tested-code.diff` 标识已测未提交代码；`baseline.json`、`resume-baseline.json`、`scope-confirmation.json` 和 `preservation-audit.json` 记录授权范围、恢复基线及未改项。10 月 21 日初次统一脚本在解析阶段失败，尚未执行构建/测试；10 月 22 日修复该临时脚本的换行转义后取得本节正式结果，原脚本留存作证据。

构建、CTest 和需要独占的校验由 `/tmp/rk3588_voice_engine_model_validation.lock` 覆盖整个进程；本轮没有模型初始化/推理或性能采样，也没有并行编译干扰采样。未更换系统服务、SDK、驱动或模型。通过后只进行文档/差异检查，未因文档修改重复跑模型或程序测试。

**阶段结论：P1-01 保持完成；P1-02 的自动化兼容问题已解决，但 A01 十分钟连续录放、A02 空闲/录音/播放三种状态重连仍待用户安排；P1-03 的合成工程回归及校验已交付，独立自然人声材料、试听与人工审核转写仍未完成。P1-02/03 保持进行中。** 私有服务重建不计作耳机重连验收；既有短测时长偏差、实机重连失败、ASR 质量未达标和 TTS 听感待确认均不改写。本轮未跨入 P2 正式适配或实时链路。

## 45. P1 耳机一分钟录放与三种状态重连

2026-10-22（Asia/Shanghai），实机运行时间 10:27:31～10:32:23。用户要求逐步指导 A01/A02，随后将连续录放改为 1 分钟；本轮结束前进一步明确“原定 10 分钟稳定性检查删掉”。据此将 A01 的正式时长由 600 s 改为 60 s，删除原 10 分钟待办，其余设备、计数、时长误差及重连条件不变。修订依据保存在本次证据目录的 `acceptance-revision.json`，历史 §34/44 的当时结论保留。

**A01、A02 的 P1 实机验收通过，P1-02 完成。** P1-03 自然人声独立材料与人工试听/转写审核仍待办；本次不替代后续实时链路、模型共驻或 A14 长稳验收。

### 45.1 设备及执行方式

预检时板端尚未出现目标耳机。用户完成连接后，vivo TWS 4（`1C:7A:43:7F:83:8F`）的配对、连接与双向节点均已出现；主代理将这副已配对耳机设为 trusted，以便物理开合盖重连。当前控制器地址为 `3E:17:C7:6F:C0:41`，与历史地址不同，本轮未修改控制器启动设置。准备及三次恢复均保持 `headset-head-unit-msbc`；原生输入/输出为 s16le、16 kHz、单声道，内部为归一化 float32、320 样本/帧。程序始终绑定本机配置指定的耳机 source/sink，无 monitor 或其他声卡回退。

A01 使用现有 `build/bin/audio_probe --config config/local.yaml --duplex --duration-sec 60`，自动连续运行至结束，未添加 stdin 控制或短播放文件。A02 分别使用 duplex 空闲、capture-only 录音中、playback-only 连续测试音播放中三次独立交互运行；每次由用户将双耳放入盒内合盖，再开盖取出佩戴，恢复 READY 后显式 start，并在完成新一轮后 stop、排空、quit。A02 的 900 s 参数仅是交互时间上限，三次均提前正常退出。

### 45.2 结果与人工确认

| 用例 | 客观结果 | 人工确认 / 结论 |
|---|---|---|
| A01，60 s 双向录放 | 有效采集 951,811 样本、2,975 帧，对应 59.4881875 s PCM；活动采集 59.996947 s；相对请求时长误差 0.853021%，相对活动时长误差 0.847976%；未掉线 | 用户确认耳机提示音正常；修订后的 A01 通过 |
| A02，空闲断开/恢复 | generation 0→1→2；检测到设备可用至 READY 67.492 ms；恢复后完成约 12.419 s 双向轮次 | 新轮次提示音正常；通过 |
| A02，录音中断开/恢复 | generation 1→2→3；检测到设备可用至 READY 51.482 ms；恢复后完成约 26.312 s 录音轮次，采集帧继续增长 | 用户在新轮次说完测试句；通过 |
| A02，播放中断开/恢复 | generation 1→2→3；检测到设备可用至 READY 41.594 ms；恢复后队列为 0，新轮次完成约 13.797 s 播放 | 用户确认没有自动续播，显式 start 后测试音正常；通过 |

四次运行退出码均为 0；capture_overflows、capture_holes、playback_overflows、playback_underruns、stale_rejections、dropped_events 和无效采集帧均为 0。三次重连各发生一次，旧 generation 失效；READY 至新 start 之间保持无活动轮次、无待播 PCM、无播放计数增长。每次新轮次均完成 stop/排空，最终 reset_pending=false。

表中的毫秒数是后端检测到目标设备可用后重建流的时间，不是完整蓝牙握手耗时。将最后一次设备缺失报告到 READY 的整个区间也算入，三次分别为 5.103447、5.094577、5.078360 s，均小于 10 s，覆盖末次重试等待；没有把用户合盖等待时间当作软件恢复延迟。

### 45.3 证据及同步范围

板端证据根目录：`output/runs/audio-acceptance-20261022-102425/`。

- `a01-60s/`、`a02-idle/`、`a02-capture/`、`a02-playback/`：各自的 `run.json`、完整 `events.jsonl`、`summary.json`。
- `preflight.json`、`pairing-scan.log`、`headset-before-preparation.json`、`headset-trust.log`：连接准备及环境快照。
- `human-review.json`、`acceptance-revision.json`、`review-evidence.py`、`review.json`、`progress.json`：人工反馈、用户修订依据、独立日志核对及最终进度。
- `a02-playback-tone.wav`：本次生成的 120 s、440 Hz、峰值 0.04、16 kHz 单声道 PCM16 测试音；不含私人录音。所有探针均 `record_wav=false`，采集只在内存中处理。

原始工具报告的 A01/A02 仍保持 not_run；上面的通过结论来自用户反馈与独立复核，保存在 `review.json`，没有修改原始报告。四次运行使用同一程序/配置，SHA-256 分别为 `4f2a7c007d42c679d70feb6e7dbaddb3d2867f7aaf2f7df086e28a9be9bf0568` 和 `3e5247d91d71caf50ff139170a6f126ffafd86d4a6df40e0a43dd773c05b1d4d`；模型/依赖清单哈希与运行前一致，模型均未加载。

全部录放通过 `/tmp/rk3588_voice_engine_model_validation.lock` 独占执行，期间没有并行编译或大文件校验。本轮统一更新入口、执行计划、验收说明与环境记录，并在迁移文档增加当前进度入口；保留上轮未提交的自动化修复，未修改生产代码、配置或 SDK，未提交、推送。文档同步只做差异、引用和证据检查，不重复已经完成的实机测试。


## 46. P1-03 自然人声与人工材料审核

2026-10-22（Asia/Shanghai），用户授权继续完成 P1-03 的自然人声材料与人工审核，已知录音会保存供固定语料使用；本轮不提交或推送。**P1-03 材料与审核完成，P1-01～03 全部交付。** 自然 20/20 试听通过；原合成 20 条全部审完，19 条通过，`mix_02` 的“调低→吊低”发音缺陷明确保留。审核完成不等于所有材料发音通过，也不等于 A03/A05 或后续模型/应用验收通过。

### 46.1 自然材料与隐私边界

材料组织、清单接口及审核规则集中在[固定测试集](file-fixtures.md)。自然集为 20 条耳机朗读录音：普通中文 8、数字日期 5、短问句 4、中英混说 3；实际 16 kHz、单声道、PCM S16LE，2.98～4.68 s，合计 75.42 s。vivo TWS 4 保持 `headset-head-unit-msbc`，用户确认环境安静并佩戴耳机。参考文本、归一化参考与关键词在录音前写入 `recording-plan.json`；只裁剪原始 PCM，未补零、降噪或调整波形增益。

用户按五句一组朗读与回听，最终 20 条文字一致、首尾完整。第 9 条 `num_01` 和第 20 条 `mix_03` 因用户自报读错各重录一次；原始尝试保留，最终重录版在回听确认后冻结。所有选择发生在 ASR 之前，本轮没有运行任何模型。`natural_q_02` 有 1/47680 个 `abs(PCM)>=32760` 的近满幅样本（约 0.0021%），用户确认该组声音正常；其余 19 条为 0，不把单点近满幅描述成持续失真或整集零削顶。

同一人完成朗读与试听，使用原合成集相同文本，独立性仅指人声波形。没有独立文本、盲审、另一位审核者、多说话人或噪声泛化证据；材料未用于本轮量化校准。自然 WAV 和原始录音均保存在被忽略的 `output/`，未获公开再分发授权；版本化 [natural-corpus.json](../tests/fixtures/natural-corpus.json) 仅保存目录清单、参考、格式、哈希和审核元数据。

### 46.2 合成材料审核与缺陷

20 条原合成主体均由用户对照固定文字试听。普通中文、数字日期、短问句及另两条混合英文材料共 19 条通过；第 19 条 `mix_02` 先报告“调低读错了”，单独重放后确认听成“吊低”，其余该组四句正常。保留原句、WAV、参考与关键词，仅记录用户报告的错误片段，不猜测前端/词典/模型的具体成因。该问题纳入 P2-03 后续定位。

`boundary_long15` 的原句四遍重复、`boundary_tail` 的末尾“新鲜空气进来”均由用户确认完整。另四条程序构造边界按结构校验覆盖，没有声明完整语句人工发音通过。15 s 重复短句仍不代表自然连续发言或 max_speech 端点验收。

新增 [synthetic-human-review.json](../tests/fixtures/synthetic-human-review.json) 作为独立审核记录，逐条绑定原 manifest/WAV/用户决定的哈希。原 manifest 中 `human_audio_review: not_run` 等字段保留生成时语义，26 个合成 WAV、所有参考及关键词不变。既有 ASR 三批 13/20、CER 15.96% 不重算、不删样本；发现输入发音缺陷后不能把全部历史差错归因于 ASR。A03 自然集模型评测和 A05 正式 10 条 TTS 输出试听仍待对应阶段，P0/M0 verified 范围不扩大。

### 46.3 校验、音量与证据

| 检查 | 本轮结果 |
|---|---|
| 私有自然包 | 20/20 哈希、格式、2～8 s 时长、8/5/4/3 覆盖、归一化参考与关键词、最终重录选择及人工确认绑定通过 |
| 原始波形追溯 | 20/20 冻结文件的 PCM 与记录样本边界内的原录音逐字节一致，无补零或波形增益处理 |
| 合成工程集 | 原 26 WAV 的现有完整性/构造校验通过；原 manifest SHA-256 仍为 `49cd7104e1eb635c4a428c6d52788b0694d3e4d0fd3caf513fd7288374472369` |
| 录放过程 | 六次录音、十一轮回放，共 17 次退出码 0；软件计数检查通过，未掉线/重连、无采集溢出/洞、播放溢出/欠载或无效采集帧；全部 `models_loaded=false` |
| 原有工作保护 | HEAD、8 项基线文件哈希、既有非文档 tracked 差异不变；模型/依赖清单、配置、audio_probe 未改，既有自动化修复保留 |
| 资源与执行方式 | 所有录放和材料校验由共享 `flock` 独占，期间没有并行编译或大文件校验；Astra / Max 子代理只读审查 |

用户在第一组回放后要求降低播放音量，耳机 sink 从 74%（-7.96 dB）改为 50%（-18.06 dB），麦克风仍为 74%；后续回放保持降低后的输出音量，证据为 `playback-volume.json`。这些逐组录音是按用户说完后停止，不拿手动停止期间的时长统计冒充 A01 一分钟稳定性测量；A01/A02 沿用 §45 的已通过证据。

板端证据根目录：`output/runs/p1-natural-review-20261022-104035/`。

- `baseline.json` / `before.diff`：原工作状态、授权范围及 8 项保护文件。
- `recording-plan.json`、`captures/`、`*-segments.json`、`clips/`、`decisions/`：录前固定参考、原始录放报告、裁剪位置、失败尝试与人工确认。
- `natural/manifest.json` / `natural/audio/`：冻结私有包，manifest SHA-256 为 `f51b14f9e6e23a9af935ab534094c95cba09dcc21c5b4dacd3e96b1e2abd8142`；需使用此 manifest 而非公共 catalog 作为后续自然集 probe 输入。
- `freeze_materials.py`、`verify_materials.py`、`material-verification.json`、`check-fixtures.json`：冻结及不加载模型的材料核对结果。
- `progress.json` / `delivery-audit.json`：最终阶段与交付差异/引用检查。

本轮统一同步 README、执行计划 1.38、验收说明、固定材料文档和本节；模型探针文档补充材料入口与范围提示。仅新增两份版本化 JSON 元数据，不改生产代码、测试实现、配置或模型，不因文档更新重复构建/CTest/实机验收。没有提交、推送。

## 47. A03 自然人声评测与 A05 正式试听

2026-10-22（Asia/Shanghai），按用户指定先评测 A03 自然人声集，再指导 A05 正式十条试听，最后统一同步文档。**本轮执行与审核完成；自然集范围内达标，整体 A03 仍未关闭，A05 因一条发音失败未通过。** 沿用固定模型/SDK 和 P0/M0 verified 范围，不调整验收门槛、不剔除失败样本。A01/A02 与材料录制不重复执行。

### 47.1 本轮结果与范围

| 项目 | 实测或复用结论 |
|---|---|
| A03 自然三批 | 每批 18/20 关键词全对、全文精确 15/20；中文 CER 3/213 = 1.40845%，三批重复及 10/20/37 ms 一致 |
| A03 速度 | 本轮 20 ms 后两批共 40 条预热样本，RTF P95 0.175357；其他分块仅一批，不报告预热 P95 |
| A03 工程边界 | 默认等待超时、超时后语音抑制、20.76 s 六遍重复强制端点与自然尾部严格参考均通过；旧严格尾部仍失败，各边界进程 exit 6 |
| A03 未完成项 | 原合成历史每批 13/20、CER 15.96% 不改；自然中英混说两条关键词失败及长片段全文错误保留；实时端点接入未实施 |
| A05 正式试听 | 统一采用历史四线程十条 r0 输出，9/10 通过；`tts_09`“搜索”听成“欧索”，原生及应用 16 kHz 两版均复现，根因未定 |
| A05 历史性能 | 复用 2026-10-20 四线程 30/30 有效输出、合成 RTF P95 0.882977；两线程 1.326 未达标不改。本轮未重合成或性能采样 |
| 材料独立问题 | 原 fixture `mix_02`“调低→吊低”继续保留，不混入正式十条的分母 |
| 运行资源 | 六个 ASR 进程最大子进程 RSS 196668 KiB、采样最小 MemAvailable 7141744 KiB、子进程 swap 采样最大 0；资源/超时保护未触发，不能解释为全部 NPU 内存 |

详细样本差异、边界配方和统计见[ASR 本轮评测](asr-probe.md#2026-10-22-自然人声评测)，正式逐条试听、两版对照和词典诊断范围见[TTS 本轮试听](tts-probe.md#2026-10-22-正式试听与发音诊断)，此处不重复完整记录。自然集仍是同文本、单说话人、安静耳机录音，新增重复/尾部工程边界不代表自然长发言或音素标注。

### 47.2 执行方式与保护

ASR 的六个进程由共享 `/tmp/rk3588_voice_engine_model_validation.lock` 独占串行运行，外层每 50 ms 采样，1.5 GiB MemAvailable 底线及每进程 180 s 保护只针对自建进程组；性能窗口没有并行编译或大文件校验。构建预检无待编译工作，未重建二进制。Astra / Max 子代理只读核对元数据、结果及静态发音线索，不运行第二套模型或修改工作副本。

A05 在 ASR 完成后执行，显式 vivo TWS 4 sink、mSBC、输出 50%；三轮 audio_probe 回放均正常结束、软件计数通过、无设备丢失，另两段 paplay 对照 exit 0。原生 44.1 kHz 也经服务转换到耳机路径，不能据“两版同错”就确定模型根因。没有新录音、LLM 初始化/推理或 TTS 生成。固定参考/关键词、原 WAV、模型/依赖、配置、生产代码与原有自动化修改均保留；原报告的未通过字段不改写。

### 47.3 证据与交付

板端私有根目录为 `output/runs/a03-a05-acceptance-20261022-111537/`，仅[模型审核摘要](../tests/fixtures/model-acceptance-review.json)进入版本化材料目录，不纳入私有录音：

- `baseline.json`、`before.diff`、`snapshot/`：本轮入口状态、文件哈希、原有差异及快照。
- `a03-plan.json`、`boundary-plan.json`、`run_asr.py`、`*-commands.json`、`*.metrics.json`、`a03/*/`：冻结方案、串行命令/保护、逐样本输出与原探针报告；`boundary-fixtures/` 为独立工程输入。
- `a05-listening-plan.json`、`a05-human-*.json`、`playback/`：正式输出选择、用户确认及原始播放报告；两版对照与静态诊断分别保存在 `a05-native-converted-diagnostic.json`、`a05-diagnostic-human.json` 和 `pronunciation-diagnostic.json`。
- `verify_model_reviews.py`、`a03-review.json`、`a05-review.json`：独立重算和证据核对；后续诊断补充由独立报告引用，原模型/播放报告不覆盖。
- `peer-review.json`、`delivery-audit.json`、`progress.json`：只读复核范围、交付差异/引用/保护检查及最终进度。

本轮同步 README、执行计划 1.39、验收、ASR/TTS 探针及材料文档。P2-01/03 保持进行中，下一步为识别质量、通用发音路径诊断及既定实时适配；不把本轮工作结束写成 A03/A05 全部通过。模型/录放验证完成后只做文档、元数据和证据一致性检查，不重复构建、CTest 或实机测试。未提交或推送。

## 48. P2 模型适配、端点与流式合成

日期：2026-10-22（Asia/Shanghai）。用户授权按执行计划推进 P2-01～03，使用 Astra / Max 子代理、共享推理锁，性能窗口避免编译或大文件校验，最后统一更新文档和进度，不提交或推送。本轮沿用板端唯一开发副本并保留入口未提交修改；未修改 Windows 参考副本、系统 SDK、原模型/词典、固定题目/参考/关键词或历史失败报告。

### 48.1 状态与实际覆盖

| 任务 | 本轮结论 |
|---|---|
| P2-01 / A03 | LISTENING 单轮端点/预录/收尾/复位已落地并实测；自然三批仍18/20关键词、15/20全文、CER3/213=1.41%。原合成质量和严格尾部仍失败，任务继续进行 |
| P2-02 / A04、A08模型侧 | 常驻 adapter/probe、固定 R 模板、预算/历史、超时/取消/关闭实现完成；20题×3、10次加载/释放、5组多轮、超长/超时恢复、首token前及生成中各5次取消通过，达到该任务模型侧交付。完整A08不因此关闭 |
| P2-03 / A05、R1 | 有界流式TTS及LLM局部联调完成；“调低”候选修正在原句与新上下文试听通过，R1两种输出试听通过。正式“搜索”发音仍未解决，A05/P2-03继续进行 |
| P2-04、P3、P4 | 未据此次两模型局部文件实验验收四模型共驻、完整文件链路、实时录放或耳机取消/延迟 |

原 10 分钟稳定性检查仍为已删除项；A01 的正式时长仍是 60 s。本轮不重做 P1 录音/重连，也没有新增自然录音。试听使用已确认的耳机，保持输出 50%；诊断临时 A2DP 两段均被反馈有电音，已恢复原 mSBC 16 kHz 路径。

### 48.2 模型与工程证据

**ASR。** 最终自然20条×3，预热N=40，实际消费PCM的RTF P95为0.121424；端点确认或手动finish到final/reset的P95为0.106615s，不是声学延迟。进程峰RSS196920KiB、最低MemAvailable7121552KiB、VmSwap0。最终二进制 `b28bc93bf85499983a746a9bce39f41456dc7236bdfb5c99ebee511a8136a9f1` 对应起点修正后的独立批次，修正前报告另存，不能拼成受控性能提升。

自然静音端点在70144样本=4.384s结束，后接第二句的73216样本未送入旧轮；无声等待80384样本=5.024s，带500ms预录时仅新输入80064样本=5.004s计时。最长发言从旧241664样本修正为240128样本=15.008s，剩92032不消费。均有512窗口量化；不冒充人工音素标注。109项独立核验通过。三条既有质量错误在固定官方整WAV demo复现，与工程raw一致；整体A03失败不被结构改造掩盖。接口与诊断口径见[ASR文档](asr-probe.md#p2-单轮端点控制2026-10-22)。

**LLM。** 固定20问三批各20/20，通过原有相关性、事实一致、短答、无思考/控制泄漏四维评分；逐条输出由实施代理审阅，主代理复核20个相同答案，区别于用户声学试听。60条均观察到EOS token248046，不把SDK未提供的内部stop_reason当成已知。预热60条TTFT P50/P95/max为435.874/475.657/493.83ms，SDK生成速率P50约12.17tokens/s；本批init3.122s。十次独立句柄加载/问答/释放全通过，init2.545～3.306s、冷句柄TTFT P95/max885.903ms；冷/预热分开，不据短循环声称长期无泄漏。

首token前5次取消到模型idle为440.20～444.31ms；生成中5次79.63～86.93ms，均在原2s门槛内。各次仍收到一个尾token但丢弃，旧提交被拒绝、取消轮不入历史、清KV后同句柄恢复和立即新会话通过。5组多轮召回正确，新会话不带旧history；8192字节输入明确拒绝。首token5ms和总800ms超时后分别约441.845ms/3.518ms确认idle，并恢复“恢复成功。”。实测最低MemAvailable5,080,068KiB、峰RSS2,206,196KiB，进程VmSwap为0；不外推全模型共驻。

审阅修复了三项边界问题：异常run返回但SDK仍active时的callback保活；close后完成轮次继续提交；探针空poll到wait间漏末packet。精确stub通过；poisoned/活动SDK异常没有在实机触发，不能把stub当硬件复现。旧smoke/固定批次/取消共73个完成轮逐包核对文本与数量无丢失，性能绑定原构建保留；新边界/生命周期/最终能力题smoke采用修复后构建。最终 `llm_10` ×3及预热均正确声明无实时联网信息。详见[LLM接口和运行](llm-probe.md)。

收尾补测同一工程输入的1/4 token上限：实际分别输出1/4个有效token ID，SDK性能计数为1/3，暴露原探针只按SDK计数判断终止原因的缺陷。已修为可见EOS优先、按有效ID数判断达到预算，并明确这是观测推断而非SDK内部stop reason；超预算单独失败，无ID冲刷包不计token。保留原始失败分类，修复后两条均正确标记token_limit，原计数与输出不改；新增9个替身回归通过。最终LLM探针二进制为 `9118e9ce4f66081e9d0de594c67e82d53ad71e2df3d77b0aa580f955e9ab1ea2`。这是独立边界补测，不并入固定20题的内容分数，也不替换原60条性能；原6个进程的105个正常完成轮次逐包核对通过。

**TTS/R1。** 原调低的实际ORT输入是diào dī，独立词级补充后变为tiáo dī；两种上下文及原有调整/调节由用户确认正确。原“搜索”模型输入已有正确s音素；新独立“搜索本地文件”听感正确，正式中英句仍不清，A2DP对照有电音，故不宣布根因已定或A05通过。[独立候选清单](../models/pronunciation-candidates.json)与原主模型清单分开；本轮插桩仅作诊断，不纳入性能统计。

R1真实LLM→TTS首次局部运行首PCM2.342s、完整5.766s；最终收尾构建的联合smoke为2.290s/5.640s；先前构建的2.282s/5.719s原报告另存，不覆盖。使用首次真实可播token到达时间线做相同配置的重放（仅TTS、新进程、无预热），stream与batch各3次：

| 文件对照 | 首PCM秒 | 完整处理秒 |
|---|---|---|
| stream三次 | 1.859 / 1.831 / 1.823 | 5.712 / 5.712 / 5.616 |
| batch三次 | 6.769 / 6.765 / 6.874 | 6.770 / 6.767 / 6.875 |

这是同一回答的小样本分段对照；不是A10端到端延迟，batch/stream的随机原生音频和分段长度不同。用户试听预先选定的第一对，确认“都完整正常，不影响听感”。另以预先固定1000ms在首段Generate中取消5次，软件提交失效为0.0041～0.0056ms量级，等待阻塞Generate返回/producer idle为0.784～0.971s，各次旧输出丢弃、PCM为0；这不是底层SDK abort能力，也没有测设备flush或耳机尾音。74项R1独立格式/样本/队列/文本/取消核验通过。原A05四线程RTF P95 0.883仍是2026-10-20数据，未用此次单句替代正式十条三次。

### 48.3 实施、回归与保护

两个 Astra / Max 子代理分别实现ASR与LLM并交叉审阅，主代理实现补充读音、TTS worker、联合探针和整合。所有模型、构建、CTest与大文件核验使用同一个 `/tmp/rk3588_voice_engine_model_validation.lock`；模型采样期间没有并行编译或大校验。代码编辑、小文件只读审阅与文档准备可并行。

本轮 `build/model-probes` 全量构建及CTest **23/23** 通过，含原引擎/音频/fixture检查、独立构建边界和新增ASR/LLM/TTS/联合CLI回归。随后预算报告补修完成全量重链接及受影响的LLM单元、LLM CLI、联合CLI **3/3** 回归，两条实际token上限及最终联合真实模型smoke也通过；联合输出确认完整文本、文件落盘后的history提交和正常关闭。各独立模型报告按其构建保存，不因最终重链接改写旧性能证据。

本轮证据根目录为 `output/runs/p2-01-03-20261022-113554/`：

- `baseline.json`、`snapshot/`、`before.diff` 保存入口与原修改；`final-source/`、`final-source-hashes.json` 保存整合后的源码。
- `asr/handoff.json`、`review.json`、最终自然/端点/边界/官方对照；`review-llm.json`、`review-llm-packet-audit.json` 保存交叉审阅与旧包审计。
- `llm/` 包含固定60题、内容/EOS审核、cancel-matrix、boundaries、lifecycle-10、最终smoke及对应代码版本、监督器/资源记录。
- `tts/` 包含实际ORT音素追踪、派生词典、用户逐次确认、播放模式恢复、R1逐次运行及 `pronunciation-review.json`/`r1-review.json`。
- `final-build-tests.json`、`final-build-test-*.log` 保存全量23项回归；`llm/budget-cap-initial-review.json`、`llm/supplemental-token-limit/`、`llm/budget-cap-review.json`、`post-token-fix-validation.json` 保存额外边界发现、修复、3项相关回归与模型复测。
- `post-token-fix-source/` 保存补修后源码；`peer-doc-review.json`、`peer-doc-followup.json`、`peer-token-limit-review.json` 保存交叉审阅。`delivery-source-hashes.json`、`delivery-audit.json` 与 `progress.json` 保存最终交付保护与进度。

文档统一更新为执行计划1.40，并同步README、各探针说明、验收、兼容性、材料入口和迁移当前状态。新增[本轮审核摘要](../tests/fixtures/p2-adapter-review.json)链接本轮证据，原模型审核/材料审核不追改。所有原有未提交修改保留；未commit或push。


## 49. A03 自然人声正式验收范围

2026-10-22（Asia/Shanghai），用户提出自然人声集通过即可、旧合成样本可以不再阻塞。按当前自然人声使用目标，正式指定已冻结的20条耳机自然人声及相关工程边界作为A03/P2-01完成依据，旧合成质量与旧尾部全文失败保留为非阻塞回归。**A03在该范围通过，P2-01完成；P2-02仍已完成，P2-03/A05的正式“搜索”问题仍待处理。**

数值门槛、完整20条分母、输入/参考/关键词、归一化与三批要求均不改。复用此前实际结果：自然三批各18/20关键词、15/20全文精确，中文CER3/213=1.40845%，预热N40的RTF P95为0.121424；10/20/37ms分块一致。自然尾部在三种分块下全文匹配、finish和reset通过；空/静音/短片段/噪声、15s最长发言、开口超时、预录、端点后不消费下一句话等工程检查保留，相关独立核验共109项通过。

旧合成13/20、CER34/213及旧尾部“请把→行吧”仍是失败记录。旧尾部的末句完整，失败来自全文中的起首识别差异；现在不再把它作为自然人声正式验收的阻塞项。自然中英两条关键词错误继续计入原20条，满足原18/20门槛即可关闭，不新增要求全部20条完美。单说话人、安静耳机、同文本以及重复句工程边界的限制继续保留；不外推噪声、多说话人、自然连续长发言或完整实时链路。

本次是基于已取得结果的验收范围调整，原§47/48、旧审核JSON和原探针报告保持当时状态；新的[自然集验收记录](../tests/fixtures/asr-natural-acceptance.json)承接当前决定并绑定证据哈希。现有probe对显式传入的旧合成manifest仍可返回exit 6，不修改实现让失败静默变成通过。

只同步执行计划1.41、README、验收规范、ASR/材料入口、迁移当前状态及本节；没有编译、推理、性能采样或重复CTest。私有证据目录为 `output/runs/asr-natural-scope-20261022-124107`，保存入口快照、决定和文档/引用/源码保护检查。原模型、SDK、WAV、参考、源码和已有未提交改动均保留；没有提交或推送。

## 50. P2-03 搜索修正与 A05 复验

2026-10-22（Asia/Shanghai），按用户要求继续P2-03；两个Astra / Max子代理分别审查前端、播放路径与正式结果，主代理整合修正并组织试听。**P2-03模型侧交付完成，A05复验通过；下一项P2-04尚未执行。** 本轮不重新开启已完成的A01/A02或原10分钟待办。

### 50.1 修正与听感

检查未发现应用WAV转换、拼接或播放丢帧能解释“搜索→欧索”；旧模型输入中s音素原本正确。实际追踪发现固定前端在逗号后独立合成短中英子句。adapter现保留中文逗号两侧的声学上下文，具体alias/blank约定见[TTS契约](tts-probe.md#2026-10-22-搜索修正与-a05-复验)，没有改词语、固定输入或模型权重。七条候选与生产输入核验通过；直接调大上游分句批量的诊断被弃用并保留。

预先选定的候选三次“搜索”均被用户确认清楚，另外四句新中英/中文上下文、独立搜索及可选“调低”都正确完整。随后以新正式三批的r0十条及第九条r1/r2复验，用户确认内容、数字日期、英文、首尾、停顿、声音和无多余标记全部通过。播放始终显式选vivo TWS 4、mSBC、50%音量，没有切换A2DP或录音。旧A05 9/10、原“欧索”和原A2DP电音记录保持不变。

### 50.2 新正式性能与局部流式回归

单模型CPU、4线程、一次预热、原十条各三次；固定顺序种子20261020，原噪声参数不变。正式性能无插桩、不启用补充词典。所有推理、构建、CTest和大文件校验由同一`/tmp/rk3588_voice_engine_model_validation.lock`串行保护，采样期没有并行编译或大校验。

| 项目 | 本次结果 |
|---|---:|
| 正式成功 / 失败，预热成功 / 失败 | 30 / 0，1 / 0 |
| Generate RTF P50 / P95 / max | 0.806690 / 0.863339 / 0.879886 |
| 完整处理 RTF P50 / P95 / max | 0.820533 / 0.882919 / 0.893231 |
| 首段可用时间 P95 | 3.154235 s |
| 完整处理时间 P95 | 3.154249 s |
| 模型初始化（不含资产校验） | 8.694477 s |
| 峰值RSS / 最小MemAvailable | 588868 / 6681152 KiB |
| WAV格式、非空、时长/尾帧与哈希 | 62 / 62通过 |
| 原固定十条用户试听 / 第九条正式重复 | 10 / 10，3 / 3通过 |

分位数为nearest-rank，P95取30条升序第29条；所有正式样本计入，无失败剔除。词典外英文为空；第七条全角冒号`Ignore OOV`三次单列，用户确认停顿内容正常。旧2线程失败和旧4线程性能继续作为历史数据，不覆盖或混入本轮统计。原始自动summary保持`a05_pass=false`，人工确认由[独立审核记录](../tests/fixtures/tts-model-acceptance.json)承接。

另以固定四块文本时间线验证流式worker调用新adapter：首PCM 2.229647s，完整2.230224s，输出43653个16k样本；这是一条局部回归，不是新的R1总体对照或实时延迟验收。生成中1000ms取消一次，旧生成结果丢弃1次，提交PCM为0，至producer idle为1188.252ms。同步Generate未被底层打断，不据此关闭完整A08。

### 50.3 构建、复核与归档

Release构建成功；全量CTest首次22/23，唯一失败为`audio_probe`在模拟采集信号测试前等待数据超时。该测试及音频生产代码本轮均未改，使用全新私有PipeWire图原样定向复跑1/1通过（13.22s）。首次失败原日志保留，根因未确定，不能写成本轮单次23/23。TTS文本、词典、worker、CLI及联合CLI回归均通过；没有放宽测试或验收门槛。

私有证据根目录`output/runs/p2-03-search-20261022-125833/`包含：

- `baseline.json`、`before-source/`与`before.diff`：入口及既有未提交修改；`binary-hashes.json`、`binaries/`与最终源码快照：测试构建标识。
- `diagnostic-plan*.json`、三个诊断分支、`input-equivalence.json`和`production-trace/`：候选、弃用方案及实际ORT输入证据。
- `formal-plan.json`、`formal/`、`verify-artifacts.py`、`artifact-review.json`：新正式批次、62个WAV核验和独立分位数重算。
- `formal-group*-selection/playback/human.json`：预选文件/哈希、显式播放路线及用户确认；`acceptance-review.json`为当前人工验收汇总。
- `build-tests.json`、`ctest.log`、`audio-focused-retest.*`：完整失败及定向复跑；`peer-*-review.json`保存独立审阅。
- `stream-search/`、`cancel-search/`：新前端的流式与取消回归；`delivery-audit.json`、`progress.json`保存最终保护与进度。

执行计划统一更新至1.42，README、TTS契约、验收、兼容性、材料入口和迁移摘要同步。原模型/SDK/主manifest、词典、固定文本、历史报告及已有未提交修改均保留；不提交、不推送。

## 51. P2-04 四模型共驻与五分钟验收

日期：2026-10-22，Asia/Shanghai。按用户授权完成 P2-04；用户此前将 A06 从连续 30 min / 至少 30 轮改为连续 5 min / 至少 5 轮，本轮再确认“按原计划审核持续换页（推荐）”。**本轮正式 A06 通过：304.870 s、44 个完整测量轮次。** 本次固定四模型文件负载无需将 LLM 隔离到另一进程；P2 全部任务完成，下一项为 P3-01。两名 Astra / Max 子代理分别实施及独立审核，所有编译、CTest、模型初始化/推理与大文件校验共用运行锁，采样期没有并行编译或校验。

### 51.1 实现与验收范围

新增 [coexist_probe](coexist-probe.md)、专用 [config/coexist-probe.yaml](../config/coexist-probe.yaml) 和 [监督器](../tests/integration/coexist_validate.py)。一个 PID 常驻 Silero VAD、RKNN Zipformer、RKLLM Qwen3.5-2B 与 CPU MeloTTS；每轮 ASR/VAD 完成后启动 LLM，确认 LLM 返回、清 KV 并空闲后才开启下一轮 ASR。TTS worker 可与本轮 LLM 流水重叠，PCM 在有界文件测试 sink 消费。加入 LLM/TTS 队列当前值只读快照，并修正采样线程结束后的最终错误复核。

输入为已冻结的 20 条自然语音，LLM 使用工具记录的五个独立固定短问题，逐轮清会话。该负载验证共驻/调度；识别输出尚未作为 LLM 问题，不计 P3/A07，也不打开耳机。默认五轮完整预热覆盖五个固定问题，预热耗时 28.235 s，全部排除在正式时长和轮数之外；正式窗口持续完成轮次，没有达到五轮后空等凑时长。

### 51.2 正式结果

| 项目 | 本轮观测 |
|---|---|
| 正式进程与轮次 | PID 137041，304.870 s，44 轮全部完成；监督器退出 0，无崩溃/OOM |
| MemAvailable | 50 ms 守卫全程最小 4286836 KiB（4.088 GiB），高于原 1.5 GiB 门槛 |
| RSS / PSS | 测量期最大 2971716 / 2974858 KiB；原始逐秒曲线与首尾一分钟摘要归档 |
| 线程 / FD | 测量期线程 11～11，FD 99～100 |
| 当前队列与峰值 | LLM 文本峰值 12/8192 B；TTS 输入 81/8192 B；分段 1/4；PCM 100/100 帧；所有轮末排空 |
| NPU 调度与 CPU 合成 | 原始事件复核 ASR/LLM 无推理区间重叠；TTS/LLM 观测重叠累计 53.959 s |
| 采样完整性 | 探针最大间隔 1.001 s，监督器 1.055 s；满足预声明 1.5 s 的间隔/端点容差 |
| 加载库 | 固定 RKNN 2.3.2 / RKLLM 1.3.0 / sherpa-onnx 1.12.14 / ORT 1.17.1-patched；测量前哈希及真实映射匹配，结束时路径集合不变 |
| 独立审核 | 原始 events/rounds/resources、进程身份、库路径与队列逐项复核；[版本化结论](../tests/fixtures/coexist-acceptance.json)绑定报告与证据哈希 |

正式窗口两路计数均为 swap-in 0 页、swap-out 0 页增量，活动桶为 0，最后 60 s 计数平台成立；测试进程 VmSwap 所有可读样本均为 0。独立审核 50/50 项通过。预热期间的换出另行保留，不混入正式窗口；不把探针 VmSwap=0 单独当作全系统没有换页的证据。NPU 专用内存计数及 PSI 在本机没有已验证可读接口，明确标注不可得；RSS 不代表全部 NPU 分配。

热与频率已随每秒资源曲线保存，最高观测温度 90.333 °C。全轮耗时 P95 前半 8.403 s、后半 9.198 s；另按五个固定问题分组核对。频率变化及冷热半程对比只能提供描述性证据，不单独证明降频因果；输入顺序/ASR 内容与流式分段也会影响耗时，本轮 CPU policy0/4/6 分别观测 0.408～1.8 / 0.408～2.352 / 0.408～2.208 GHz，NPU 0.8～1.0 GHz；后半全轮 P95 增加约 9.46%，五个问题的轮耗时中位数增加约 8.66%～10.50%，而 ASR/LLM 本段 P95 均未恶化。持续负载下的合成与散热值得后续性能阶段跟进，不能写成完全没有后半程变慢；具体比较见独立审核。五分钟结果不代替 A14 的一小时增长/长稳条件。

首次独立审核的额外总样本数断言误判了监督器：304.870 s 内 296 个测量期样本，最大间隔 1.055 s、两端缺口 0.993/0.871 s，均符合运行前固定的 1.5 s 容差。该总数断言不在冻结计划中；原审核及原脚本保留，纠正审核只移除这项冲突断言，继续要求完整有序样本和相邻/端点间隔，未改运行记录、分类器或门槛。采样覆盖补充回归 6/6 通过；纠正说明与针对性覆盖测试见 `review/review-correction.json`。

### 51.3 三次诊断失败与换页口径

最初额外采用严格 `zero` 诊断策略，任何预热后页计数增量都立即失败。第一次在测量约 9.488 s 后观察到 +7 个换出页，原样复跑在约 10.338 s 后 +5 页；两者只完成一轮。扩展为五轮代表性预热后，第三次仍在约 4.182 s 后 +1 页，未完成测量轮次。它们均以 6 退出，未达到 300 s，保持失败记录，不能追改为通过。

第三次活动发生在已经预热过的相同语音片段，故“仅因首次长片段分配”不足以解释。测试进程逐秒 VmSwap 观测为零，测试后输入法 fcitx5 有 120 KiB 换出，系统只观察到后台回收、未观察到直接回收/分配停顿/OOM；这些只能说明存在其他进程换出，不能逐页归属。未修改 swap、缓存、频率、内核、驱动、模型或 runtime。

用户确认回到原 A06“没有持续换页”口径后，新增 `review` 策略完整记录五分钟；`zero` 仍可显式选择作诊断。下一次运行前固定审核方法：两路计数分别按测量起点对齐到 1 s 桶，连续至少 5 个活动桶或任意 30 s 内至少 15 个活动桶判持续；60 s 内至少三段不相邻活动且首末跨度至少 30 s，或最后 60 s 未形成计数平台，判证据不足并保持待审。只有两路完整曲线均未发现这两类模式、其余 A06 条件满足，才允许通过。没有按总页数设置豁免上限，也未事后改规则；采样时刻不确定性受 1.5 s 最大间隔限制。

### 51.4 自动化、证据与后续

全量 CTest 25/25 通过；五轮预热修订及最终换页策略修订分别重跑受影响的共驻 CLI/监督器 2/2，通过参数边界、预检、进程组超时与低内存保护。换页审核器另有 15/15 合成曲线测试，覆盖孤立活动、持续/周期活动、尾段活动及缺测/倒退；后续纯文档同步不重复程序测试。

私有证据根为 `output/runs/p2-04-coexist-20261022-132341`：

- `baseline.json`、`before-source/`、`before.diff` 保留入口与既有未提交修改。
- `smoke/` 为通过的短测，A06=not_run；`formal/`、`formal-retry/`、`formal-representative-warmup/` 及对应独立失败审核均原样保留。
- `sustained-swap-review-plan.json` 为用户口径与运行前规则；`formal-swap-review/` 保存五分钟正式命令、stdout/stderr、配置/资产/二进制身份、库映射、逐轮结果与双路资源曲线。
- `review/formal-swap-review-audit-corrected.json`、`review/swap_review.py` 及其测试绑定正式审核；`swap-review-tested-source-hashes.json`、`swap-review-tested-source/` 绑定最后受测源码与二进制。
- `build-tests.json`、`warmup-build-tests.json`、`swap-review-compile.json`、`swap-review-tests.json`、`swap-review-classifier-tests.json` 保留每次构建/测试；最终 `delivery-audit.json` 与 `progress.json` 保存保护核对与进度。

执行计划更新至 1.44，同步 README、工具契约、验收、兼容性与迁移摘要。原模型/SDK/主清单的 P0 verified 范围、原词典、固定文本、自然录音及历史报告保持；本轮未提交、未推送。后续为 P3-01 完整文件链路，再按 P4 接入真实设备、状态机和完整 A08。

## 52. VS Code Remote-SSH 连通性复核

2026-10-22（Asia/Shanghai），用户要求检查 Windows VS Code 能否通过 SSH 连接板端。复核结果：**14:16:53 起远程工作区已成功连接并打开 `/home/orangepi/projects/rk3588_voice_engine`。** Windows `code --status` 显示 `Remote: SSH: orangepi5`，远端系统为 Linux arm64 / 6.1.99-rockchip-rk3588；窗口标题含工程名及 `[SSH: orangepi5]`。

- 现有 SSH 别名 orangepi5 正常，目标为 orangepi@192.168.137.2:22，Windows 绑定地址 192.168.137.1；BatchMode 免密命令可执行 whoami、pwd 和 Git 状态读取。
- Windows VS Code 为 1.142.0，Remote-SSH 为 0.128.0；板端 glibc 2.35，既有 VS Code Server 的 Node 可运行。
- 初查时 SSH 隧道与 Exec Server 已连通，但远端编辑器尚未启动。日志显示 Agent Host 正在下载 217259935 字节的 ARM64 服务，进度持续增加；等待下载及解包完成后，连接恢复。单独看到隧道 resolved 不等于完整远程工作区已就绪。
- 本地编辑器对应 commit `6d62b1d5dace93a33b3a6d3eca1317bd5e15bcb1`；CLI Agent Host 自动取得 1.143.0 / commit `2d0b7506a36c1d6db9a466440131a8e01eab2289`。两者属于 VS Code 自身组件；本轮没有更换工程 SDK。
- 板端当日 remoteagent.log 已记录 ManagementConnection、ExtensionHostConnection 建立，以及 Extension Host 进程启动；结合客户端状态确认工作区连接成功。没有重写 SSH 配置、VS Code 用户设置，也没有删除远程安装目录或清理项目文件。

本机证据（Git 忽略）：`output/runs/vscode-ssh-check-20261022-141456/baseline.json`、`verification.json` 和脱敏的 `remote-agent-proof.log`；记录不含连接令牌。本次仅打开远程工程并完成连通性检查，没有修改或暂存源码，没有执行提交或推送。
