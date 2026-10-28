# 支持平台与系统依赖

更新日期：2026-10-28。当前完整语音链路只声明支持下表中的已验证组合。默认构建可在其他 Linux 环境检查通用代码，但不代表那些环境已支持 NPU 推理或蓝牙语音。

## 固定支持组合

| 层次 | 当前基线 | 约束 |
|---|---|---|
| 板卡 | Orange Pi 5，RK3588S，8 GB 机型 | 8 GB 是已用机型，不是经过验证的最低内存要求；其他容量及 5B / 5 Plus / RK3576 未列入本版支持范围 |
| 系统 | Orange Pi 1.2.4，Ubuntu 22.04.5 LTS / Jammy，aarch64，Xfce 桌面镜像 | 使用板厂 BSP；不是通用 Ubuntu ARM64 镜像 |
| 内核 | `6.1.99-rockchip-rk3588` | 与板卡设备树及驱动一起保持配套 |
| NPU 内核驱动 | RKNPU `0.9.8` | 它与下面的用户态 runtime 是不同组件 |
| NPU 用户态 | RKNN Runtime `2.3.2`；RKLLM Runtime `1.3.0` | 库与头文件按同一固定来源取得，文件身份以 [依赖清单](../deps/manifest.json) 为准 |
| CPU 语音库 | sherpa-onnx `1.12.14`；ONNX Runtime `1.17.1-patched` | 采用清单中的 Linux aarch64 预编译件；不要替换成系统 Python 包或其他 ORT 版本 |
| 编译与 C 运行库 | GCC `11.4.0`；glibc `2.35`；C++17 | 本工程 CMake 最低要求为 3.20，现用 CMake 3.22.1、Ninja 1.10.1、Python 3.10 |
| 音频服务 | PipeWire / pipewire-pulse `0.3.48-1ubuntu3.2`；WirePlumber `0.4.8-4` | 程序使用 libpulse 协议连接用户服务；隔离音频测试使用 WirePlumber 0.4 配置 |
| 蓝牙 | BlueZ `5.64-0ubuntu1.4`；vivo TWS 4 的 `headset-head-unit-msbc` profile | 其他适配器或耳机须单独确认双向节点；A2DP 播放模式不能替代耳机麦克风链路 |
| 引擎内部音频 | 16 kHz、单声道、20 ms 帧 | 系统原生节点格式由后端转换；设备名称由每位使用者配置 |
| 运行方式 | 登录桌面的普通用户、用户级服务按需启停 | 本版不承诺纯无桌面会话、开机自启或跨系统镜像搬移 |

当前结果及其样本范围见 [性能快照](performance-snapshot.md)。本页的版本复核只读取系统身份和包元数据，没有新增推理、耳机或新板验收。

系统包表中的 Debian/Ubuntu 修订号是已记录环境，不是全系统依赖锁。下文 apt 命令从 Jammy 仓库解析可用包；若解析结果与表中不同，应记录差异并完成相应回归，不能直接继承原组合的验证结论。模型、NPU runtime 和 CPU 语音库则按清单固定文件校验。

## 取得匹配系统并核对身份

板厂镜像入口为 [Orange Pi 5 官方资料页](https://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/service-and-support/Orange-pi-5.html)。现用镜像名称记录为 `Orangepi5_1.2.4_ubuntu_jammy_desktop_xfce_linux6.1.99`。板厂构建工程见 [orangepi-build](https://github.com/orangepi-xunlong/orangepi-build)。

仓库尚未提供该桌面镜像归档的 SHA-256 和固定下载文件身份，因此目前能固定运行环境组合，不能声称可以逐字节重建系统镜像。获取镜像时记录实际文件名、来源和校验值；若官方目录只剩其他版本，先视作新的适配组合。安装本项目的 apt 依赖不会把旧 RKNPU 驱动升级成 0.9.8，不应为凑版本混装其他板卡的内核、设备树或驱动。

在开发板普通用户终端执行以下只读检查：

```bash
tr -d '\0' < /proc/device-tree/model
printf '\n'
cat /etc/os-release
uname -m
uname -r
cat /sys/module/rknpu/version
gcc -dumpfullversion
getconf GNU_LIBC_VERSION
```

期望板卡是 Orange Pi 5，架构为 `aarch64`，系统为 Jammy 22.04.5，内核和 RKNPU 与上表一致。读取失败时先检查 BSP 与驱动加载情况，不用安装 RKNN/RKLLM 用户态库代替驱动排查。RKLLM 1.3.0 的配套说明见其 [固定版本手册](https://github.com/airockchip/rknn-llm/blob/878f9361fd3afa7e167b7079918918f78d2c1c2a/doc/Rockchip_RKLLM_SDK_EN_1.3.0.pdf)。

Windows 开发入口使用现有 SSH 别名：

```powershell
ssh orangepi5
```

此后所有安装、编译和运行命令均在开发板执行；不需要 WSL，也不要求在开发板安装 Codex。

## 安装系统包

以下用于首次准备新板，需 apt 源提供 Jammy 的 `main`、`universe`、`jammy-updates` 与安全更新。保留板厂针对该板卡的源和 BSP；不要把发行版升级作为本步骤的一部分。

```bash
sudo apt update
sudo apt install --no-install-recommends \
  build-essential cmake ninja-build pkg-config git \
  python3 python3-dev python3-venv python3-pip curl ca-certificates \
  bzip2 unzip tar coreutils util-linux \
  libpulse-dev libsamplerate0-dev libsndfile1-dev \
  libyaml-cpp-dev nlohmann-json3-dev libgomp1

sudo apt install --no-install-recommends \
  pipewire pipewire-pulse wireplumber libspa-0.2-bluetooth \
  pulseaudio-utils bluez dbus-user-session
```

`libpulse-dev` 提供客户端开发接口，不要求同时运行旧 PulseAudio 服务。`pulseaudio-utils` 提供 `pactl`。隔离音频回归需要 PipeWire、pipewire-pulse、WirePlumber 和 pactl；它不依赖物理耳机。当前包来源可交叉核对 [Ubuntu Jammy 更新仓库](https://packages.ubuntu.com/jammy-updates/pipewire-pulse) 和 [WirePlumber 包](https://packages.ubuntu.com/jammy/wireplumber)。

本页不通过 apt 安装 RKNN、RKLLM、sherpa-onnx 或 ONNX Runtime；这些固定资产按 [资产获取与准备](assets-setup.md) 安装到工程约定路径。尤其不要用 `pip install onnxruntime` 替代 C++ 程序依赖的固定 `.so`。

安装后可查看包版本与编译发现结果：

```bash
dpkg-query -W cmake ninja-build libpulse-dev libsamplerate0-dev \
  libsndfile1-dev libyaml-cpp-dev nlohmann-json3-dev \
  pipewire pipewire-pulse wireplumber libspa-0.2-bluetooth bluez
pkg-config --modversion libpulse samplerate sndfile yaml-cpp
```

## 普通用户的设备访问

NPU render 节点编号并非固定值。先找出绑定 RKNPU 的节点，并查看 DMA heap 权限：

```bash
for rk_node in /sys/class/drm/renderD*; do
  printf '%s -> %s\n' "/dev/dri/${rk_node##*/}" \
    "$(readlink -f "$rk_node/device/driver")"
done
ls -l /dev/dri/renderD* /dev/dma_heap/system*
id
```

匹配 BSP 下通常由 `render` / `video` 组提供相关设备访问。若当前普通用户缺少这两个组，在确认设备所属组后执行：

```bash
sudo usermod -aG render,video "$USER"
```

然后重新登录桌面并建立新的 SSH 会话，使组成员变更生效。只调整所需组，不将设备权限改为全员可写，也不通过 `sudo` 运行语音引擎。

## 准备用户音频会话

先在该普通用户的桌面会话中登录，再由同一用户通过 SSH 操作。检查：

```bash
systemctl --user is-active pipewire pipewire-pulse wireplumber
pactl info
```

预期三项服务均为 `active`，pactl 的服务端名称为 `PulseAudio (on PipeWire 0.3.48)`。已有正确配置时跳过下面的切换步骤。

若新镜像仍运行旧 PulseAudio，先保留自己的音频配置，再切换到本项目基线。以下只作用于当前用户，切换期间会中断该用户正在使用的音频：

```bash
systemctl --user --now disable pulseaudio.service pulseaudio.socket
systemctl --user mask pulseaudio.service pulseaudio.socket
systemctl --user enable --now pipewire.socket pipewire-pulse.socket wireplumber.service
systemctl --user start pipewire.service pipewire-pulse.service wireplumber.service
```

只在旧 PulseAudio 单元存在时执行前两行。不要同时运行 `pipewire-media-session` 和 WirePlumber。若 `systemctl --user` 无法连接用户总线，先检查桌面登录和用户会话，不临时改成系统级音频服务。

部分板厂镜像的 `/etc/pulse/client.conf` 指向 `/tmp/pulse-socket`。如果服务已启动而 pactl 仍连接错误地址，可先进行一次显式连接检查：

```bash
test -n "$XDG_RUNTIME_DIR"
PULSE_SERVER="unix:$XDG_RUNTIME_DIR/pulse/native" pactl info
```

显式连接成功后，为当前用户保存客户端设置。以下备份不会覆盖已有备份；已有复杂配置时应合并 `default-server` 和 `autospawn` 两项：

```bash
mkdir -p "$HOME/.config/pulse"
if test -f "$HOME/.config/pulse/client.conf"; then
  cp -an "$HOME/.config/pulse/client.conf" \
    "$HOME/.config/pulse/client.conf.before-rk3588"
fi
printf 'default-server = unix:%s/pulse/native\nautospawn = no\n' \
  "$XDG_RUNTIME_DIR" > "$HOME/.config/pulse/client.conf"
pactl info
```

蓝牙配对、选择双向 profile 和填写设备名见 [资产获取与准备](assets-setup.md) 的本机配置步骤；设备排查见 [音频诊断](audio-diagnostics.md)。其他机器应使用自己的 source/sink 名称，不复制现机硬件地址。

WirePlumber 0.5 已改变配置格式，本项目隔离测试使用的 0.4 Lua 配置不能直接迁移，详见 [上游迁移说明](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/migration.html)。升级音频栈属于新的适配与回归工作。

## 基础构建检查与下一步

已按[发布入口](releases.md)校验并解压源码包时，继续留在 `rk3588_voice_engine-0.2.0` 目录，跳过下面的 clone；后续构建不需要 `.git`。已有工作副本也直接进入原目录，不另建副本。

选择 Git 安装且 `v0.2.0` 已正式发布时，在板端获取该标签，避免默认分支的后续变更改变安装版本：

```bash
mkdir -p "$HOME/projects"
cd "$HOME/projects"
git clone --branch v0.2.0 --depth 1 https://github.com/jzh03/rk3588_voice_engine.git
cd rk3588_voice_engine
```

如果找不到目标标签或对应 Release 附件，先检查正式发布页面和网络连接，不以默认分支替代所选版本。

在工程根目录执行下列基础检查。锁覆盖整个构建和测试进程，避免与模型推理及性能采样争用：

```bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock bash -c '
  set -eu
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j2
  ctest --test-dir build --output-on-failure
  ./build/bin/env_check
'
```

预期 CTest 通过且最后输出 `ENV_CHECK_OK`。这些检查不初始化模型或访问物理耳机；默认构建关闭三个模型探针开关，尚不具备完整实时问答入口。继续按 [资产获取与准备](assets-setup.md) 获取固定资产、准备本机配置，再按 [构建与运行](build-and-run.md#模型文件诊断) 开启完整模型构建。
