# P7-01 本地部署、离线启动与回退

P7-01已完成，使用普通用户的版本目录和systemd用户服务，现有M2配置、模型、阈值及引擎二进制保持不变。7项部署回归通过；锁交接修正后，A13完成3次进程冷启动各5轮及回退后的5轮，缺资产/坏配置拦截通过。首次模型加载前的锁失败保留。整板重启、退出登录、无桌面登录和长稳属于P7-02，本轮不执行，M3未完成。结果见[环境§60](environment.md#60-p7-01-版本化部署与离线回退2026-10-27)和[自动化摘要](../tests/fixtures/p7-01-automation-review.json)。

## 安装布局

板端默认安装到 `/home/orangepi/.local/share/rk3588-voice-engine/`：

~~~text
rk3588-voice-engine/
├── assets/<manifest-sha256>/    # 按清单共享的固定模型、词典、SDK库、许可与来源文件
├── releases/<release-id>/
│   ├── bin/voice_engine
│   ├── config/engine.yaml      # 独立完整配置；运行参数不变
│   ├── config/wake-keywords.txt
│   ├── models -> ../../assets/<hash>/models
│   ├── deps -> ../../assets/<hash>/deps
│   ├── lib/                   # 固定私有运行库的链接
│   ├── deploy.py              # 与此版本一起冻结的启动管理器
│   └── release.json           # 程序、配置、管理器哈希及资产身份
├── current -> releases/<release-id>
├── previous -> releases/<previous-id>
├── runs/<run-id>/             # 有界运行记录，不保存原始麦克风音频
├── control.fifo               # 仅本机用户可写的控制入口
└── status.json                # 最近一次托管运行的状态记录
~~~

模型、依赖清单和所列文件作为独立副本保存在共享池中；同一组合的后续版本复用该池，不依赖开发目录中的权重、词典或私有库。程序仍使用当前板端系统库、内核和NPU驱动，并非跨系统通用包。资产内原candidate/verified状态及来源限制原样保留，不扩大为再分发资格。

打包入口是 [scripts/deploy.py](../scripts/deploy.py)。打包只把资产清单和关键词路径改为发布目录内的相对路径；检查其余配置完全一致。当前沿用单实例TTS 4线程、原2个原生音频槽、0.85语速、1500 ms储备、统一12/12/2分段、max40、128输出tokens、15秒窗口和既有唤醒/打断阈值。

## 准备与切换版本

在板端工程根目录执行，以下版本名仅为新版本示例；已存在的版本不能覆盖：

~~~bash
python3 scripts/deploy.py stage \
  --project . --binary build/model-probes/bin/voice_engine \
  --config config/barge-in-local.yaml --release m2-release-002
python3 scripts/deploy.py verify --release m2-release-002
~~~

stage复制时校验清单SHA-256，预留至少3 GiB磁盘余量；失败的暂存目录会清理。模型按整组清单哈希共享，新模型组合须保留旧资产以便回退。版本和资产不自动删除。

verify检查程序、配置和管理器哈希、资产链接、实际动态库解析路径，并调用引擎 `--check-config` 完整校验启用资产。私有库必须解析到安装目录中的固定RKNN/RKLLM/Sherpa/ORT，不能静默使用开发目录或系统中的另一份同名库。启动前再次执行同样的预检；缺文件、哈希变化和坏配置都会在打开设备/加载模型之前失败。

切换前先对候选执行所需的功能smoke；`verify` 本身是配置/资产检查，不冒充模型推理。可以用 `serve --release m2-release-002` 直接检查候选而不改current。当前版本须停止，再执行：

~~~bash
systemctl --user stop rk3588-voice-engine
python3 scripts/deploy.py activate --release m2-release-002
systemctl --user start rk3588-voice-engine
~~~

activate预检通过后才原子替换current，旧版本记录为previous。运行锁会拒绝运行期间的切换；坏候选不会覆盖当前指向。首次安装没有历史版本时，previous为空；后续升级产生回退目标。发布smoke和切换过程不得与其他模型/构建任务并行。

## 用户服务与本机控制

用户服务模板：[packaging/rk3588-voice-engine.service.in](../packaging/rk3588-voice-engine.service.in)。

~~~bash
python3 scripts/deploy.py install-service \
  --template packaging/rk3588-voice-engine.service.in
systemctl --user start rk3588-voice-engine
python3 scripts/deploy.py send status
python3 scripts/deploy.py send cancel
systemctl --user stop rk3588-voice-engine
~~~

install-service只安装并校验用户unit，不自动启动或启用开机运行，不覆盖不同内容的已有unit。实际服务状态用 `systemctl --user status rk3588-voice-engine` 查看；`python3 scripts/deploy.py status` 显示版本指向与最近运行记录。

需要开机运行时使用 `systemctl --user enable rk3588-voice-engine`。当前用户原有Linger=yes和音频用户服务设置沿用；其存在不等于已验证整板重启/退出登录后的音频和NPU权限，相关验收仍在P7-02。

服务仅连接本用户的PipeWire/Pulse Unix socket，遵守原设备名和profile，不修改耳机音量、默认路由或蓝牙配置。模型进程自身持有并继承项目公共锁，整个运行期阻止其他模型/构建任务并发。停止服务时通过原quit控制路径取消、flush并退出；10秒仍未退出则监督清理，systemd停止上限15秒。运行错误可自动重启，但5分钟最多3次；配置/资产/锁错误退出2，不反复重启。引擎原2秒模型取消期限不变。

本机FIFO只接受start、stop、cancel、new-session、status、help、quit。没有新增远程API；后台没有终端时也不会因stdin EOF立即退出。`serve` 前台运行时仍可从另一终端通过send控制，Ctrl+C同样受控退出。`--mock` 仅供软件回归，不代表真实模型或耳机验收。

## 离线与日志边界

启动器在执行引擎前安装继承式seccomp规则，只允许创建AF_UNIX socket。IPv4/IPv6创建必须实际返回拒绝才继续，并保存network.json；规则不可用时明确失败。模型加载、tokenizer、中文词典和全部后续推理均在该限制内。systemd另设置NoNewPrivileges和相同地址族限制。限制只作用于应用，不修改网络接口、防火墙或管理SSH。

托管运行日志总额度为500 MiB。启动前预留64 MiB，运行中定期检查；仅清理安装目录runs下有托管标记且已结束的最旧记录，保留当前、未完成和非托管目录，不跟随目录软链接。单次引擎JSONL仍沿用原轮转设置，控制台另做1 MiB轮转并保留3份旧文件。无法在额度内安全清理时停止新增运行或受控退出；不清理工程中原验收证据，不记录原始麦克风PCM或完整问答。自动验证的重要汇总另存工程output/runs及版本化fixture，避免将可清理的运行日志视为永久证据。

## 回退

先停止服务，恢复上一程序、配置及其固定资产组合，再启动：

~~~bash
systemctl --user stop rk3588-voice-engine
python3 scripts/deploy.py rollback
systemctl --user start rk3588-voice-engine
~~~

rollback同样先检查目标完整性，再切换current/previous；丢失或损坏的回退组合不会被启动。应用回退不变更系统库、内核或驱动，也不代替系统回退。

## 本轮自动验证与范围

普通CTest新增deployment，覆盖共享资产、参数保持、坏配置不替换当前版本、缺失/损坏资产拦截、版本回退、运行锁、越界路径、日志保留边界、本机控制和退出。初始7项检查通过；第一次真实服务启动在模型加载前被原有锁所有权规则拒绝，已修正为由将执行引擎的进程自身持锁，并增加exec后内核锁所有者检查，定向回归再次通过。未修改引擎核心或运行参数，首次失败保留。

[deployment_runtime.py](../tests/integration/deployment_runtime.py) 是显式选择的A13验证，不加入普通CTest：采用真实五模型、冻结版本及systemd用户单元；只替换私有音频设备标签。按原A13执行3次进程冷启动各5轮、一次坏候选/缺资产拦截及回退后5轮。进程冷启动不清除系统文件缓存，不代表整板冷启动。实际库映射、离线限制、退出码和逐轮完成结果分别保存；不重新评分ASR/LLM/TTS，不做声学计时或换页专项。

当前A13已通过：3次进程冷启动15/15轮、回退后5/5轮，4个模型进程均退出0；记录中的采集/播放溢出、播放欠载、采集缺口和丢日志均为0。四次均验证IPv4/IPv6创建受阻、AF_UNIX可用，实际私有库均从固定资产池加载。缺资产及frame_ms=0坏配置分别被拦截，当前版本未被替换；随后回退上一测试版本并完成5轮。

实际安装的current为`p7-m2-20261027-r2`，previous为`p7-m2-baseline-20261027-r2`。首次发布的previous是同一稳定程序/配置/模型组合的独立版本目录，不冒充曾存在更早的生产版本；后续升级会保存真正的上一版。正式用户unit已安装并校验，当前inactive/disabled；本轮不需要人工配合。首次锁失败、测试候选和修正后记录均保留，不把坏候选当正式入口。

板端证据目录为`output/runs/p7-01-automation-20261027-01/`，首次失败在`a13/`，修正后的20轮结果在`a13-r2/`，安装状态在`installation-review.json`。仅当前固定组合通过，不外推不同SDK/模型组合升级或耳机听感。P7-02的1小时长稳、系统重启和退出登录尚未执行，M3未完成。
