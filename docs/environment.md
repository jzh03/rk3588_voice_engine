# 开发环境与已完成基线

核对日期：2026-10-16（Asia/Shanghai）。来源为本轮 Windows 本机只读检查、现有 `ssh orangepi5` 会话的只读命令，以及用户明确确认。后续已按用户提供的安装结果及音频配置输出更新；基础依赖已安装，C++17 与依赖编译链接检查已通过；模型推理尚未验证。

## 1. 状态结论

- **已完成：**板端系统/资源/驱动盘点；电脑转换条件盘点。
- **已完成（用户确认）：**蓝牙连接和耳机同时播放、录音测试。用户要求直接记为完成，后续无需重复基础验证。
- **已完成（用户终端输出）：**基础开发依赖安装及版本查询；pactl 可访问 PipeWire 兼容服务。
- **已完成（用户终端输出）：**C++17 与依赖编译链接检查，程序输出 `ENV_CHECK_OK`。
- **已完成（用户终端输出）：**板端 Git main 分支初始化和 .gitignore；本轮通过 SSH 配置该项目提交身份为 jzh03 / 1053442572@qq.com。
- **待实施：**CMake/Ninja 工程构建、SDK/模型兼容性验证、应用录放和完整链路。
- **已完成：**vivo TWS 4 的 source/sink、profile、codec 和节点格式已由用户提供；用户确认默认输入输出已设置且查询结果一致。应用缓冲、实际延迟仍待应用阶段测量。

用户确认没有提供基础录放测试时长或测量文件，因此不补造“已连续测 10 分钟”等细节。后续 `audio_probe` 的 10 分钟检查验收的是新程序，不是重新验证用户已完成的系统能力。

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
| RKLLM 1.3.1 驱动要求 | 官方手册要求 `0.9.8`，当前驱动低于要求 | 手册第 12 页 §2.4；详见 [compatibility.md](compatibility.md) |
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

`pactl info` 成功连接 `/run/user/1000/pulse/native`，服务名称为 `PulseAudio (on PipeWire 1.0.5)`。上述查询证明工具可用、包元数据可发现，C++17 与依赖实际编译链接已由后续检查通过。安装前 apt 提示下载 95.2 MB、新增 262 MB、升级 43 包、新装 66 包；这不是安装后实测磁盘占用。

`ldconfig -p` 可见 `libpulse.so.0`、`libpulse-simple.so.0` 和 `libsamplerate.so.0`。常规系统库位置及 ldconfig 缓存未发现 `librknnrt`、`librkllmrt`；这不等于扫描过整块磁盘，不排除其他路径有用户保存的副本。板端工程目录 `/home/jzh/projects/rk3588_voice_engine` 已由用户创建；检查程序位于 `env-check-Ca991G` 子目录。用户已提供 Git 初始化结果：main 分支尚无提交，.gitignore 为待提交文件；本轮 SSH 复核一致。

## 3. 耳机当前配置与历史音频快照

PipeWire、pipewire-pulse、WirePlumber 用户服务均为 active；PipeWire 和 pipewire-pulse 包版本为 `1.0.5-1ubuntu1`；BlueZ 包版本 `5.72-0ubuntu5`。PulseAudio 服务包未安装，libpulse 客户端可通过 PipeWire 兼容服务接入。

初次检查时（用户后来已接回耳机），`wpctl status` 仅枚举 DP0、ES8388 和 HDMI0；ES8388 为当时活动默认输出。配置中仍有以前的 `bluez_output.…` 名称，但没有对应蓝牙运行节点。`systemctl is-active bluetooth` 当时返回 inactive，BlueZ 对象树没有适配器对象，rfkill 列表为空，USB 枚举没有明确的蓝牙设备。由此只能判断**本次快照未枚举到可用蓝牙音频设备**，不能据此否定用户之前已经完成的连接和录放测试。

`bluetoothctl show/devices` 的初次非交互查询超时，后改用有界 D-Bus、服务和 PipeWire 枚举取得快照。没有据此重配设备、改 profile 或启动录音。

原始精简结果：[board-audio-2026-10-16.json](evidence/board-audio-2026-10-16.json)。该文件记录当时的状态，不是录放验收日志。

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

用户明确不使用 WSL。实施方式为现有 Windows + SSH/SCP，板端原生编译，优先使用配套预转换模型；不把安装新的电脑运行环境列为前置任务。

| 项目 | 本轮结果 | 规划影响 |
|---|---|---|
| 系统 | Windows 11 家庭版中文版，`10.0.26300` | 沿用 Windows + SSH 工作方式 |
| RAM | 16,889,036,800 B（约 15.73 GiB），16 逻辑处理器 | 不以总内存保证 2B 模型转换成功 |
| 空闲内存 | 3,255,272 KiB（约 3.10 GiB） | 快照；转换前重新检查实际余量 |
| C 盘空闲 | 26,291,716,096 B（约 24.49 GiB） | 模型下载缓存优先放 F 盘 |
| F 盘空闲 | 575,062,822,912 B（约 535.57 GiB） | 可保存模型下载和校验文件 |
| 其他 Linux 主机/GPU | 未核实 | 不假设已具备可用转换服务器或 CUDA |

原始资源结果：[pc-resources-2026-10-16.json](evidence/pc-resources-2026-10-16.json)。仅说明当前资源，不代表自行转换模型的环境或工具已经具备。

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

| 待验证内容 | 负责阶段 |
|---|---|
| RKLLM 1.3.1 已明确要求驱动 0.9.8；待落实当前 Ubuntu 内核的驱动适配与恢复，再验证模型；RKNN 所选版本要求另行核对 | P0-02～04 |
| Qwen3.5-2B 文本链路及取消行为、实际模型大小/内存 | P0-04、P2-02 |
| RKNN 与 RKLLM 同进程共存、总资源峰值 | P2-04 |
| 配套预转换产物的来源和生成版本；无法取得时的独立转换需求 | P0-03，不预设安装电脑运行环境 |
| 新程序协商缓冲/延迟及重连后绑定，设备名/profile/节点格式已记录 | P1-02、P4 |
| 实际识别质量、TTS 声音和端到端延迟 | P2～P4 |
| 无桌面会话时的音频/NPU 权限、自启动和恢复 | P7 |
| 原计划中的下载代理 `192.168.137.1:17897` 是否仍可用 | 首次下载前检查；本轮没有验证 |

已知事实、用户确认和工程目标在主计划中分别标注，不将这里的空闲资源或历史测试替换成未运行的应用性能结果。


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

结论：该最小程序的 C++17 编译、libpulse/libsamplerate/libsndfile/yaml-cpp 链接和运行、YAML 解析及 JSON 序列化通过。未打开音频流，未测试 NPU，也尚未验证 CMake/Ninja 工程构建。Git 初始化和忽略规则随后已完成，正在准备云端首次提交；之后进入工程构建与 SDK 准备。

## 8. Git 与云端托管准备

用户已在板端 `/home/jzh/projects/rk3588_voice_engine` 执行 `git init -b main`，并创建忽略构建/模型/运行库/本机配置的 `.gitignore`。本轮 SSH 复核仍无提交和远程地址。

用户指定提交身份为 `jzh03 <1053442572@qq.com>`，已通过 `git config --local` 配置到板端本项目。用户选择 GitHub 私有仓库，目标为 `jzh03/rk3588_voice_engine`；本轮确认 Windows Git Credential Manager 已有对应账号登录。

Windows 规划文件与板端检查源码整理为同一首次提交，模型、编译结果、临时检查目录和临时图片排除；检查源码保存为 `tools/env_check.cpp`。云端创建、首次推送及 Windows/板端相同提交验证尚在进行中，只有实际推送验证后才标记完成。
