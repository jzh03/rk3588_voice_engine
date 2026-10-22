# 新镜像迁移与 Codex 配置交接

更新：2026-10-22，Asia/Shanghai（迁移阶段的历史日期保留）。适用设备为**普通 Orange Pi 5 / RK3588S / 8 GB**。用户最终选择保留官方 Orange Pi 1.2.4 / Ubuntu 22.04.5 / BSP 6.1.99，工程已通过 GitHub 恢复；此前 26.04 选择与旧系统内容保留为迁移背景。本文件负责新系统的恢复顺序；详细配置契约和日常命令仍以 [README](README.md)、[配置模板](config/default.yaml)为准。

**2026-10-20 恢复记录：**网络、公钥 SSH、自动校时、构建依赖及工程已恢复，Release / env_check / 8 项通用测试通过。音频组件已安装并恢复 PipeWire；AP6275P M.2 已启用并完成扫描，vivo TWS 4 播放及 10 秒录音回放已由用户确认正常；用户指定的约一分钟双向检查已结束且听感正常，短测时长偏差保留、不排查；空闲重连检查未通过，出盒重连及恢复 mSBC 后双向节点可用，本机配置已校验，见[一分钟检查与重连](docs/environment.md#34-ap6275p-耳机长时稳定性及重连检查)。音频集成已运行失败，长时录放、重连与完整链路未验收；见[音频恢复记录](docs/environment.md#31-音频链路恢复)。SDK/runtime 的同日更新以环境记录为准。下文旧环境摘要不是新系统参数。

当前阶段以 [README](README.md#当前进度)和[执行计划](EXECUTION_PLAN.md)为准：2026-10-22 已完成 P1-02 的 A01 一分钟录放与 A02 三种状态重连，原10分钟检查已删除，见[实机结果](docs/environment.md#45-p1-耳机一分钟录放与三种状态重连)。P2-01按自然人声正式范围通过、P2-02模型侧已交付；P2-03 的“搜索”修正与正式 A05 复验已通过；P2-04/A06 已完成 304.870 s / 44 轮四模型共驻，下一项为 P3-01，见[当前 TTS 结果](docs/environment.md#50-p2-03-搜索修正与-a05-复验)和[共驻验收](docs/environment.md#51-p2-04-四模型共驻与五分钟验收)。下列接手条件保留迁移当时的背景。

## 1. 接手时先确认这几件事

- 这是独立 C++17 / CMake / Ninja 工程，板端原生编译。参考项目的 ROS2 Humble、colcon、M1066、WebRTC AEC3、PREEMPT_RT 和在线豆包服务，**不是当前工程的安装依赖**。不配置 WSL。
- P1-01 工程骨架已完成；P1-02 真实音频后端、DSP 和 `audio_probe` 已实现，旧环境自动回归 9/9 通过，但耳机连接超时，A01/A02 实机验收未完成。模型适配及完整语音对话尚未实现。
- P0-02 仍在进行中。当前内核 6.1.99-rockchip-rk3588，厂商 RKNPU 0.9.8 已核验；后续准备配套 SDK/runtime 与模型，不再执行旧系统菜单测试或候选安装脚本。
- 用户决定不备份整卡；工程已恢复到 /home/orangepi/projects/rk3588_voice_engine。本轮明确授权网络、时间与构建依赖配置，已完成；未扩展到音频服务或 SDK 安装。

### 26.04 的 NPU 核验条件

**Ubuntu 26.04 或“有 NPU 驱动”不能单独证明 RKLLM/RKNN 可用。** 此前查到的官方文件是 `Orangepi5_1.2.4_ubuntu_resolute_desktop_gnome_linux6.18.49.7z`；对应公开 next 配置启用 `CONFIG_DRM_ACCEL_ROCKET=m`，没有 `CONFIG_ROCKCHIP_RKNPU` 项。Rocket 使用另一套用户态接口，不能直接计为本项目所需的厂商 RKNPU `0.9.8+`。本次没有拿到用户最终选择的 26.04 镜像，不能判断其二进制是否与该文件相同。[固定源码及官方接口依据](docs/compatibility.md#官方固件与-ubuntu-2604-镜像核对)

新 Codex 应记录**实际镜像完整文件名、下载来源、校验值、内核、驱动名称/版本与 NPU 绑定设备**，再判断 SDK 条件。若只有 Rocket，先完成普通开发/音频环境，明确保留 NPU 阻塞；不要自动移植驱动、混装旧内核或把安装 `.so` 当成解决办法。若确认厂商 RKNPU 达标，仍需配套 runtime 的最小推理验证。

## 2. 当前开发板配置摘要

以下是迁移前的实测/已有配置，**供恢复意图参考，不能直接当成新系统的参数**。完整历史见 [环境记录](docs/environment.md)。

| 项目 | 旧环境配置 | 新镜像处理 |
|---|---|---|
| 板型、架构 | Orange Pi 5，`rockchip,rk3588s-orangepi-5`，aarch64 | 不使用 5 Plus / Pro / Max 镜像；SDK 目标按例程选 `rk3588` |
| 系统、内核 | Ubuntu 24.04.1；`6.1.0-1025-rockchip`；RKNPU `0.9.7` | 用新镜像实际值重新登记 |
| 内存、存储 | 约 7.74 GiB RAM；约 32 GB TF 卡；1 GiB `/swapfile` | 重查 `free`、`df`、`swapon`；镜像若已有 swap/zram，先核对，不重复创建 |
| 用户与工作区 | 普通用户 `jzh`，uid 1000；`~/projects/rk3588_voice_engine` | 可用新用户名，路径跟随 `$HOME`；编译和运行不使用 sudo |
| 有线网络 | NetworkManager；旧接口 `end1`；静态 `192.168.137.2/24`，网关/DNS `192.168.137.1` | 仅在沿用原 Windows 共享网络时恢复，接口名重新枚举 |
| 下载代理 | HTTP 代理 `http://192.168.137.1:17897`；旧系统在 `/etc/environment` 与 `/etc/profile.d/orangepi-proxy.sh` 设置环境变量 | 依赖 Windows 代理允许局域网访问；先测试，按需配置，不覆盖整个环境文件 |
| SSH / 编辑 | Windows + VS Code Remote SSH；alias `orangepi5`；端口 22；板端为唯一日常工作区 | 启用 OpenSSH，恢复客户端 alias，重装后核验新主机指纹 |
| 音频服务 | PipeWire + pipewire-pulse + WirePlumber，普通用户会话 | 保留 libpulse 客户端接口，不另启竞争的 PulseAudio 服务 |
| 会话 | `/run/user/1000`；用户 D-Bus；`Linger=no`；有桌面登录 | 新 uid 动态获取；无桌面自启动尚未验收 |
| 外设 | vivo TWS 4；Realtek RTL8821C USB Wi-Fi/蓝牙，USB ID `0bda:c820` | 重查固件、蓝牙和耳机连接；旧驱动组合下蓝牙曾持续超时 |

本次未在用户及 `/etc` 下的 PipeWire/WirePlumber 配置目录发现自定义配置文件，也没有发现工程专用用户服务；无需为迁移凭空添加旧 Lua 配置、实时调度或开机自启规则。旧模型/SDK 目录只有 `.gitkeep`，尚无真实资产清单；系统库缓存中未发现 RKNN/RKLLM runtime。

## 3. 上传与克隆工程

仓库：[jzh03/rk3588_voice_engine](https://github.com/jzh03/rk3588_voice_engine)，分支 `main`。本文基于含未提交修改的工作区；当时 HEAD 为 `bd759b5bf3c41041d9af064b3a9b868abbee764c`，**这个提交本身不包含当前全部音频实现或本文**。

用户上传前按 [README 的 Git 操作](README.md#版本管理约定)检查暂存内容。尤其要包含目前尚未跟踪的 `include/voice_engine/audio_dsp.hpp`、`include/voice_engine/live_audio.hpp`、`src/audio/audio_dsp.cpp`、`src/audio/pulse_audio.cpp`、`tools/audio_probe.cpp`、`tests/unit/audio_dsp_test.cpp`、`tests/integration/audio_probe.py`，以及对应 CMake、其他源码/测试、文档修改和本文件。`scripts/patches/` 可作为历史适配资料上传，但新镜像不用执行它。仅提交本文会遗漏尚未上传的实现。

`.gitignore` 排除了 `config/local.yaml`、`build/`、`output/`、`tmp/`、模型/运行库实体和凭据；它们不会随 push/clone 恢复。用户不保留整卡时，旧 `output/runs/` 的路径只能作为历史线索，不能要求新 Codex 读取不存在的证据。耳机本机配置按下文重新生成即可；不用把令牌、SSH 私钥、蓝牙配对密钥或录音提交 Git。

新系统已具备 Git 后，以普通用户执行：

```bash
mkdir -p ~/projects
cd ~/projects
git clone https://github.com/jzh03/rk3588_voice_engine.git
cd rk3588_voice_engine
git status --short --branch
git log -1 --oneline
```

如果目录已存在，先检查其中内容再同步，不覆盖已有工作树。新机只需克隆时不要求 GitHub 登录；需要推送时再通过 `gh auth login` / `gh auth setup-git` 由用户认证，步骤见 README。

## 4. 恢复网络与远程入口

本节可由 **Windows 上的 Codex** 执行。标为 PowerShell 的命令在 Windows 运行；标为板端 Bash 的命令在开发板本地终端或 SSH 登录后的终端运行。工程的安装、编译和测试始终在开发板上完成。

### 4.1 当前连接方式与配置位置

| 项目 | 已确认情况 |
|---|---|
| Windows SSH 别名 | `orangepi5`，用户提供的 `ssh -G` 输出已确认 |
| 实际目标 | `jzh@192.168.137.2`，TCP 端口 `22` |
| 当前连接来源 | 板端 `SSH_CONNECTION` 显示客户端 `192.168.137.1`，服务端 `192.168.137.2:22` |
| 当前认证 | sshd 日志为 `Accepted publickey`，使用 RSA 公钥；不是已证实的密码登录 |
| 登录公钥指纹 | `SHA256:7+F0bkeV2qlNX9VPTgm+ptZc5Vk8soxo0fdKfxA+TX8`，用于在 Windows 找到同一把**用户公钥** |
| Windows 客户端配置 | 默认 `%USERPROFILE%\.ssh\config`；若 VS Code 设置了 `remote.SSH.configFile`，还须读取其实际指定文件 |
| Windows 密钥 | `ssh -G` 列出 `~/.ssh/id_rsa`、`id_ecdsa`、`id_ecdsa_sk`、`id_ed25519`、`id_ed25519_sk`、`id_xmss`、`id_dsa` 默认候选；这些输出不能证明文件都存在。实际 RSA 私钥路径由 Windows Codex 本机检查 |
| 板端授权位置 | 当前 `/home/jzh/.ssh/authorized_keys` 有 1 条有效记录；新系统需重新加入 Windows 公钥 |
| 板端服务配置 | `/etc/ssh/sshd_config` 包含 `/etc/ssh/sshd_config.d/*.conf`；当前没有该目录的额外配置文件。`ssh.service` 和 `ssh.socket` 均 enabled，实际监听 `*:22` |

板端当前活动配置含 `KbdInteractiveAuthentication no`、`UsePAM yes`、`X11Forwarding yes`、`PrintMotd no`、`AcceptEnv LANG LC_*` 以及 SFTP subsystem。普通用户执行 `sshd -T` 因不能读取主机密钥而失败，因此**未确认完整的生效默认项，也不保证新镜像允许密码登录**。恢复连接不要求复制旧 sshd 配置，先使用新镜像的服务默认值检查公钥登录。

### 4.2 新板第一次启动与网络

先在板端显示器/键盘完成新镜像初始用户、网络和时间设置。客户端交接日期是 **2026-10-19**，旧板本轮时钟读数仍为 **2026-10-15**；新系统先检查 `timedatectl`，证书报错时核查时钟，保留 TLS 校验。

若仍使用旧 Windows 共享网络，保持 Windows 连接开发板的网口为 `192.168.137.1/24`，上联网卡开启对该网口的 Internet 连接共享。先用 Windows `Get-NetIPConfiguration` 核对，不覆盖其他网卡配置。板端 NetworkManager 的 IPv4 手动参数按 §2 填写；用 `ip -br address`、`ip route`、`nmcli connection show --active` 识别实际接口与连接。旧连接名是 `Wired connection 1`，但新镜像的名称可能不同。

若已经通过 DHCP/其他路由器联网，就采用该网络分配的地址，不强制套用 `192.168.137.*`。变更 SSH 所在接口时在板端本地操作。网络管理服务不是 NetworkManager 时，按镜像实际服务配置，不混用两套管理方式。

SSH 未安装/启动时，在确认依赖变更后于**板端 Bash**执行：

```bash
sudo apt-get update
sudo apt-get install openssh-server
sudo systemctl enable --now ssh
systemctl status ssh --no-pager
ss -ltn
ip -br address
```

确认监听端口 22。如果新系统启用防火墙，只按实际局域网管理需要放行 SSH；不关闭整个防火墙，不对公网开放端口。新用户名可以继续使用 `jzh`，否则同步改 Windows alias 和工程路径。

### 4.3 Windows 客户端与别名

**Windows PowerShell：**

```powershell
Get-Command ssh, ssh-keygen, scp
ssh -V
$sshConfigPath = Join-Path $env:USERPROFILE '.ssh\config'
Test-Path $sshConfigPath
ssh -G orangepi5 | Select-String '^(hostname|user|port|identityfile) '
Test-NetConnection 192.168.137.2 -Port 22
```

若没有 `ssh`，先检查 Windows“可选功能”中的 OpenSSH 客户端，取得确认后补装；不安装 WSL，也不需要 Windows OpenSSH 服务端。Codex 应查看现有配置并只修改 `Host orangepi5` 对应段，保留其他主机。基础段如下，地址/用户依新镜像修改：

```sshconfig
Host orangepi5
    HostName 192.168.137.2
    User jzh
    Port 22
```

用下面的 **Windows PowerShell** 查找现有公钥及 agent 身份，无需读取私钥内容：

```powershell
Get-ChildItem (Join-Path $env:USERPROFILE '.ssh') -Filter '*.pub' |
    ForEach-Object { ssh-keygen -lf $_.FullName }
ssh-add -l
```

`ssh-add -l` 在未启用 agent 时可能失败，不代表磁盘上没有密钥。若 `id_rsa.pub` 指纹与 §4.1 一致，且对应 `id_rsa` 存在，可在该 Host 段增加：

```sshconfig
    IdentityFile ~/.ssh/id_rsa
    IdentitiesOnly yes
```

如果实际公钥位于其他路径或由 agent 提供，使用找到的实际身份，不硬套 `id_rsa`，不为了文档默认值覆盖旧密钥。私钥留在 Windows；重装开发板不需要重新生成这把用户密钥。

### 4.4 重装后的主机指纹与用户公钥

先在**板端 Bash**查看新系统主机指纹：

```bash
ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
```

这与 §4.1 的用户登录公钥是不同用途。若客户端实际协商其他主机密钥类型，核对对应的 `/etc/ssh/ssh_host_*_key.pub`。确认是本人刚重装的这块板后，才在 **Windows PowerShell** 删除对应旧记录并重新连接：

```powershell
ssh-keygen -R orangepi5
ssh-keygen -R 192.168.137.2
ssh orangepi5
```

接受新指纹前与板端显示值核对；不要删除整个 `known_hosts`、关闭主机校验或用未经核对的 `ssh-keyscan` 结果建立信任。若换了地址/端口，按实际目标处理对应条目。

新镜像没有旧 `authorized_keys`，因此公钥登录初次失败是可解释的。恢复方法任选适用的一条：

- **新镜像允许用户密码登录时：**在 Windows 确认实际 `.pub` 路径，执行 `scp "$publicKeyPath" orangepi5:windows-codex.pub`，其中 `$publicKeyPath` 先赋值为该公钥路径；密码由用户交互输入。随后 `ssh -t orangepi5` 登录，在板端追加公钥。
- **不能远程密码登录时：**在 Windows 读取并复制该 `.pub` 的完整一行，通过板端本地终端将其存入当前用户的 `~/windows-codex.pub`，再执行下列步骤。无需放宽 SSH 认证或启用 root 登录。

**板端 Bash，以将来登录的普通用户执行：**

```bash
ssh-keygen -lf ~/windows-codex.pub
umask 077
mkdir -p ~/.ssh
touch ~/.ssh/authorized_keys
if ! grep -qxF -f ~/windows-codex.pub ~/.ssh/authorized_keys; then
  printf '\n' >> ~/.ssh/authorized_keys
  cat ~/windows-codex.pub >> ~/.ssh/authorized_keys
fi
chmod 700 ~/.ssh
chmod 600 ~/.ssh/authorized_keys
rm ~/windows-codex.pub
```

确认临时公钥指纹正确后再执行追加部分；保留已有其他授权项，不上传私钥。若账户目录下文件归属错误，先查明再修正。新系统重装后授权文件的缺失与 GitHub 登录失效是两件事，分别恢复。

### 4.5 Windows Codex 连接、执行与取回结果

**Windows PowerShell：**

```powershell
ssh -o BatchMode=yes -o ConnectTimeout=10 orangepi5 'id; uname -rsm'
ssh -o BatchMode=yes orangepi5 'cd ~/projects/rk3588_voice_engine && git status --short --branch'
```

第一条成功表示 Windows Codex 能无交互执行普通远程命令；第二条需先按 §3 在板端克隆工程。若私钥带口令，先由用户解锁已有 SSH agent 或使用交互式客户端，不把口令写进脚本。agent/IdentityFile 问题用 `ssh -v orangepi5` 本机定位，分享诊断前去掉不必要的个人路径和地址。

Windows Codex 可读取本机克隆/下载的本文件，再通过 `ssh orangepi5 '<板端命令>'` 检查或配置板端。它运行在 Windows，不代表要把工程改成 Windows 编译，也不要在 Windows 参考副本和板端同时开发。复杂修改应把待审脚本通过 `scp` 传到板端，再执行；不要把包含 `$`、引号的多层 shell 代码未经检查直接拼接。

需要 sudo 的安装先向用户提交具体变更；用户可在 `ssh -t orangepi5` 交互终端输入密码执行。不把密码放进 `ssh` 命令或 `sudo -S` 管道；不要假设另一终端的 sudo 缓存能被自动任务使用。只有获得相应授权并具备实际执行条件，Codex 才执行系统变更。

VS Code 使用 Remote-SSH 的“连接到主机 → orangepi5”，打开板端 `~/projects/rk3588_voice_engine`。若 VS Code 与 PowerShell 行为不同，核对 `remote.SSH.path`、`remote.SSH.configFile` 是否选择了不同客户端/配置。Codex 使用用户选择的 Windows 客户端或远程扩展，账号重新登录；工程不携带登录凭据。

远程证据默认留在板端 `output/runs/<新run_id>/`；必要时 Windows 可用 `scp -r orangepi5:~/projects/rk3588_voice_engine/output/runs/<新run_id> <本机目标目录>` 取回，替换占位值后执行。不要将整个 `output/` 自动提交 GitHub。

### 4.6 下载代理

原代理确实仍可用时，可先仅对当前终端设置：

```bash
export http_proxy=http://192.168.137.1:17897
export https_proxy=http://192.168.137.1:17897
export HTTP_PROXY="$http_proxy"
export HTTPS_PROXY="$https_proxy"
export all_proxy="$http_proxy"
export ALL_PROXY="$http_proxy"
curl -I --connect-timeout 5 --max-time 15 https://github.com
```

`curl` 可随下节开发工具安装；未安装时先用现有工具检查网络。`sudo apt` 未必继承代理，若它无法联网，应针对 apt 配置代理并先确认地址可达；不要把代理失败误判为依赖不存在。长期设置按需要写入一个明确的位置，避免无条件复制旧系统文件。

## 5. 工程依赖安装清单

以下清单由当前 [CMakeLists.txt](CMakeLists.txt) 和测试实际调用生成，面向新系统 apt 仓库。先 `sudo apt-get update`，再将计划安装命令中的 `install` 临时换成 `--simulate install` 查看变更；向用户确认后安装。若镜像已提供某项，按缺项补齐。不要为复刻旧版本添加 Noble/Jammy 源或降级 26.04 的系统库。

### 编译、普通测试和诊断工具

```bash
sudo apt-get install \
  build-essential cmake ninja-build pkg-config python3 git curl ca-certificates \
  coreutils kmod usbutils \
  libpulse-dev pulseaudio-utils libsamplerate0-dev libsndfile1-dev \
  libyaml-cpp-dev nlohmann-json3-dev
```

| 依赖 | 工程用途 | 旧系统参考版本，不作为新系统锁版本 |
|---|---|---|
| G++ / CMake / Ninja | C++17；CMake 最低 3.20 | G++ 13.3.0 / 3.28.3 / 1.11.1 |
| libpulse / libsamplerate / libsndfile | 异步录放、流式重采样、诊断 WAV | 16.1 / 0.2.2 / 1.2.2 |
| yaml-cpp / nlohmann-json | 配置解析、JSONL 与资产校验 | 0.8.0 / 3.11.3 |
| Python 3 / coreutils | 集成测试；`sha256sum` 资产校验 | Python 3.12.3 |
| pkg-config / pthread | CMake 查找开发库、线程支持 | pthread 由工具链/系统 libc 提供 |

当前工程不需要 pip/conda 环境、OpenSSL 开发包、内核 headers 或整套模型转换工具链。`gh` 是推送认证的可选工具；`openssh-server` 用于远程登录；`alsa-utils` 用于额外 ALSA 诊断，均可按实际缺项单独安装。

### 用户音频服务与蓝牙

```bash
sudo apt-get install \
  pipewire pipewire-bin pipewire-pulse wireplumber \
  libspa-0.2-bluetooth bluez dbus-user-session
```

上述包还提供音频集成测试用的 `pipewire`、`pipewire-pulse`、`wireplumber`；`pactl` 来自 `pulseaudio-utils`。新仓库若调整包名，按提供的可执行文件/库核对，不先跳过测试。`pulseaudio-utils` 和 `libpulse-dev` 是客户端工具/库，不要求安装 PulseAudio 服务包。

普通登录用户检查 `systemctl --user is-active pipewire pipewire-pulse wireplumber` 和 `pactl info`，系统侧检查 `systemctl is-active bluetooth`。缺失或未运行时核对新镜像提供的 unit，再启动相应用户音频服务与系统蓝牙服务。`pactl`、`audio_probe` 和工程程序都不用 sudo。

SSH 中若找不到用户音频服务，先核查 `id -u`、`loginctl show-user "$USER"`、用户会话和 `/run/user/$(id -u)` 下的 socket；仅在该用户的 socket 确实存在时恢复 `XDG_RUNTIME_DIR` 和 `DBUS_SESSION_BUS_ADDRESS`。不要固定复制 uid 1000，也不要指向 root 或其他用户的音频服务。旧环境没有启用 linger，P7 的无桌面自启动仍待实现/验证。

## 6. 耳机与本机 YAML

当前意图是 vivo TWS 4 的 HFP/mSBC 双向录放，历史正常配置为 `headset-head-unit-msbc`、16 kHz 单声道；设备地址和当时完整节点名在 [环境记录 §3](docs/environment.md#3-耳机当前配置与历史音频快照)。它们只帮助识别设备，新版 PipeWire 的节点后缀可能改变。

1. 插入 RTL8821C USB 适配器，核查 `lsusb`、蓝牙控制器、固件加载和 rfkill。新镜像使用其配套固件/模块，不复制旧 `.ko` 或为绕过权限直接开放所有设备。
2. 用系统蓝牙界面或 `bluetoothctl` 在用户配合下重新配对、信任并连接耳机；确认 Connected，再枚举 `pactl list cards`、`pactl list short sources`、`pactl list short sinks`。配对成功不等于音频已连接。
3. 从设备实际提供的 profiles 选择带麦克风的双向模式；若可用，优先 mSBC。确认输入不是 `.monitor`。若没有 mSBC，记录实际模式及格式后评估，不伪填旧 profile。
4. 若尚无 `config/local.yaml`，从 `config/default.yaml` 复制，填写实际 source、sink、profile；已有文件先阅读再作最小修改。模板的其余字段、单位和范围以模板注释为准。

本机配置的最小形状如下，三个占位值必须替换为实际枚举值：

```yaml
audio:
  source: "<实际麦克风 source，不能是 monitor>"
  sink: "<实际耳机 sink>"
  profile: "<实际已启用的双向 profile>"
  sample_rate: 16000
  channels: 1
  frame_ms: 20
```

程序一次加载一个完整 YAML，不合并 default/local；省略的分组采用解析器缺省值。开始恢复时所有模型保持禁用。默认 source/sink 可按个人需求设置，但程序显式绑定配置中的设备，不依赖全局默认值。当前旧系统没有耳机节点，默认输入已回到 monitor；这个临时状态不应照搬到新配置。

## 7. 构建与验证顺序

在新机新克隆的目录中，按 [README 基础环境检查](README.md#基础环境检查)完成 Release 配置、`-j2` 编译、CTest 和 `env_check`；不搬运旧 `build/` 或 CMakeCache。旧环境成功只是参考，新版工具库必须重新验证。

| 顺序 | 操作入口 | 通过依据 |
|---|---|---|
| 1 | README 基础环境检查 | 编译成功；`ENV_CHECK_OK`；CTest 全部通过，当前完整构建注册 9 项 |
| 2 | [配置检查](README.md#配置检查) | 实际 local.yaml 输出 `CONFIG_OK`、`ASSETS_OK`；模型未启用时资产计数为 0 |
| 3 | [mock 控制](README.md#终端控制与-jsonl-日志) | `MOCK_READY`；start/stop/cancel/quit 正常，区分模拟与真实录放 |
| 4 | [真实音频诊断](README.md#真实音频诊断) | 耳机节点和 profile 确认后进行 10 s 双向试听，再做 [A01/A02](docs/ACCEPTANCE.md#a01a02-实机执行步骤) |
| 5 | 下节 NPU/SDK | 驱动前提与最小模型分别验证，不能由编译或 mock 通过推断 |

**当前 Jammy 音频测试限制：**PipeWire 0.3.48 / WirePlumber 0.4.8 的私有音频测试已实测失败，候选适配未通过全部断言；详细结果及试验差异见[音频恢复记录](docs/environment.md#31-音频链路恢复)。不能因组件已安装就计为 9/9。

**已识别的跨版本测试风险：**旧机 WirePlumber 为 `0.4.17`。[音频集成测试](tests/integration/audio_probe.py)的私有 `POLICY_CONFIG` 直接引用 0.4 的 Lua 策略脚本和组件。WirePlumber 0.5 改变了配置/组件加载方式，因此新机若使用 0.5，需实际检查这份测试策略是否可加载；不能直接宣称兼容，也不要为测试降级整套系统音频服务。[WirePlumber 官方迁移说明](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/migration.html)

若该项失败，保留 CTest 和临时服务日志，定位是测试策略适配还是应用 libpulse 后端问题；可单独验证不受影响的目标，但报告必须注明音频集成未通过。后续修改测试仍须保持独立临时 socket、合成输入/空输出，不接管用户真实音频设备。修复后运行受影响检查，再更新环境记录。

## 8. NPU 与模型环境恢复

新系统先只读核验，下面命令不是驱动安装步骤：

```bash
cat /etc/os-release
uname -rsm
tr '\0' '\n' < /proc/device-tree/compatible
if [ -r /sys/module/rknpu/version ]; then
  cat /sys/module/rknpu/version
fi
modinfo -F version rknpu
modinfo rocket
ls -l /dev/dri /dev/accel
for node in /sys/class/drm/renderD* /sys/class/accel/accel*; do
  [ -e "$node" ] || continue
  printf '%s\n' "$node"
  readlink -f "$node/device"
  readlink -f "$node/device/driver"
done
```

某个模块或目录不存在时记录结果，再查当前内核配置及启动日志；内置驱动不一定出现在 `lsmod`，`modinfo` 也不能单独证明驱动成功绑定。需要读取受限日志时再用 sudo 只读查看 `journalctl -k -b`。旧 NPU 是 `renderD129`、`fdab0000.npu`、绑定 `RKNPU`，新编号不得硬编码。以普通用户核对真实 NPU 节点权限；按镜像现有 udev/组策略处理，不使用 `chmod 777`。

驱动前提达到后，按 [执行计划 §3.2](EXECUTION_PLAN.md#32-sdk-选择规则)实施，当前候选是：

- RKLLM 1.3.1 配套 aarch64 runtime / 头文件；先验证 Qwen3.5-0.8B，再验证目标 Qwen3.5-2B 文本模式。不是参考项目的 Qwen3-VL-2B。
- Zipformer 先评估固定版本的 Model Zoo / RKNN Toolkit2 / runtime 2.3.2 配套组合；VAD、CPU TTS 与后续 KWS 使用计划中的 sherpa-onnx 组合。
- 这些都是**待取得、待核验的候选**，本项目尚未安装或验证。新镜像若预装 runtime，先记录实际版本、架构、路径、哈希及 GLIBC/GLIBCXX 需求，不直接覆盖。
- 运行库/SDK 放 `deps/`，模型/词典放 `models/`；按 [资产清单契约](README.md#资产清单)记录真实来源、commit/tag、许可证、工具链、runtime、大小和哈希。未取得产物时不建立虚假的 verified 条目。
- 优先使用可追溯预转换模型；模型转换工具不是恢复 C++ 工程的前置安装项。不复制 `.so` 到系统目录覆盖原库，不全量下载模型后才检查驱动。

## 9. 交付给下一次 Codex 的任务

可让 Windows Codex 先读取已上传仓库中的本文，再直接发送：

> 你现在运行在 Windows。请先阅读 AGENTS.md、MIGRATION_GUIDE.md、README.md 和 EXECUTION_PLAN.md。我已重装香橙派官方 Ubuntu 26.04，请按迁移文档 §4 检查 Windows 的 ssh 客户端、orangepi5 alias 和现有 RSA 密钥，配合我核对新主机指纹并恢复板端公钥授权；不能连接时明确指出需我在板端本地完成的步骤。SSH 恢复后，在开发板克隆工程并只读盘点实际板型、内核、NPU 驱动/设备、网络和音频服务，列出缺失依赖及需要修改的配置，安装前向我说明并确认。编译和测试均通过 SSH 在板端执行，恢复 C++17 构建、实际耳机 local.yaml、配置/mock/音频检查；若新版 WirePlumber 使私有音频测试失败，保留证据并适配。先证明厂商 RKNPU 与 SDK 前提，不能把 Rocket 或新 Ubuntu 版本号当成 RKLLM 已通过；满足条件再继续 P0-02/03/04。不要重新执行旧内核安装/菜单脚本，不安排整卡备份或 WSL，不上传凭据。将新机实际结果写入环境记录，证据保存到新的 output/runs/，同步 README 与执行计划；失败或未执行项保持明确。

恢复完成应交付：新镜像身份与驱动结果、实际依赖版本、本机 YAML、构建/CTest/配置检查记录、耳机实测结果或明确阻塞、模型/runtime 状态，以及对应的任务进度。P1-01 的历史完成记录保留，A01/A02 与 SDK 推理未通过前仍不能标为完成。

本次原始只读记录位于旧机被 Git 忽略的 `output/runs/new-image-handoff-2026-10-19/`；关键结论已写入本文与环境记录，新机不依赖该目录存在。
