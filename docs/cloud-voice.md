# 云端语音配置与部署

当前源码支持端侧档和云端档。端侧档在板端完成 ASR、LLM、TTS。云端档将这三项交给配置的网络服务，唤醒、VAD、耳机录放、对话与取消仍由板端控制。蓝牙耳机需提供双向录放节点。两档共用同一耳机输出时，音量由系统的该输出设备控制。[耳机与音频诊断](audio-diagnostics.md)

## 选择模板与准备凭据

| 模板 | ASR | LLM | TTS |
|---|---|---|---|
| [百炼北京档](../config/cloud-bailian.yaml) | `dashscope_asr_ws`，`qwen-audio-3.1-asr-flash-message` | `chat_completions_sse`，`qwen3.8-flash` | `dashscope_tts_ws`，`qwen-audio-3.1-tts-flash` |
| [通用兼容档](../config/cloud.yaml) | `audio_transcriptions` | `chat_completions_sse` | `audio_speech_wav` |

以百炼档为例，在板端工程根目录执行。已有本机文件会继续使用。

~~~bash
if [ ! -e config/cloud-local.yaml ]; then
  install -m 600 config/cloud-bailian.yaml config/cloud-local.yaml
fi
install -d -m 700 ~/.config/rk3588-voice-engine
if [ ! -e ~/.config/rk3588-voice-engine/.env.cloud ]; then
  install -m 600 packaging/.env.example ~/.config/rk3588-voice-engine/.env.cloud
fi
pactl list short sources
pactl list short sinks
pactl list cards
~~~

在 `config/cloud-local.yaml` 的 `audio` 中填写实际的 `source`、`sink` 和活动 `profile`。如需两档用同一耳机，端侧配置也填同一对节点。百炼档在 `~/.config/rk3588-voice-engine/.env.cloud` 填写 `DASHSCOPE_WORKSPACE_ID` 和 `DASHSCOPE_API_KEY`。三项服务通过各自的 `api_key_env` 引用密钥，模板共享同一个环境变量。兼容档按模板填写三个服务地址、模型和相应的密钥变量。凭据文件供 systemd 和板端终端预检读取，部署版本只保存环境变量名。[环境变量模板](../packaging/.env.example)

实时命令省略 `--config` 时从当前目录读取 `config/cloud-local.yaml`。显式传入完整的端侧或云端配置即可指定模式。系统服务使用已部署版本里的配置。

## 构建和准备资产

先准备通用[系统依赖](supported-platform.md)和[固定资产](assets-setup.md)。云端档需要本地 Silero VAD、KWS 及 Sherpa-ONNX/ONNX Runtime CPU 库。要同时运行端侧档，再准备本地 ASR、LLM、TTS 资产和对应 SDK。云端的 libcurl 由系统提供，WebSocket 构建另用 libwebsockets。以下命令针对 Ubuntu 板端。

~~~bash
sudo apt install libcurl4-openssl-dev libwebsockets-dev python3-yaml
~~~

如果板端语音服务已在运行，先执行 `systemctl --user stop rk3588-voice-engine`，再构建、打包或校验资产。构建和推理共用板端模型锁。

双后端构建包含两档，也启用百炼流式 ASR 和 TTS。

~~~bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake -S . -B build/dual-backend -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVOICE_ENGINE_BUILD_ASR_PROBE=ON -DVOICE_ENGINE_BUILD_LLM_PROBE=ON \
  -DVOICE_ENGINE_BUILD_TTS_PROBE=ON -DVOICE_ENGINE_BUILD_CLOUD=ON \
  -DVOICE_ENGINE_BUILD_STREAMING_ASR=ON -DVOICE_ENGINE_BUILD_STREAMING_TTS=ON
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake --build build/dual-backend --target voice_engine -j2
~~~

只需要云端档时，使用独立构建目录并关闭三个端侧模型选项。

~~~bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake -S . -B build/cloud-bailian -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVOICE_ENGINE_BUILD_ASR_PROBE=OFF -DVOICE_ENGINE_BUILD_LLM_PROBE=OFF \
  -DVOICE_ENGINE_BUILD_TTS_PROBE=OFF -DVOICE_ENGINE_BUILD_CLOUD=ON \
  -DVOICE_ENGINE_BUILD_STREAMING_ASR=ON -DVOICE_ENGINE_BUILD_STREAMING_TTS=ON
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake --build build/cloud-bailian --target voice_engine -j2
~~~

## 部署、切换与共用语速

双后端打包需要两份完整的本机配置和同一对资产清单。以下示例假定已有 `config/barge-in-local.yaml`，其中包含本地模型与实际耳机配置。只部署云端时，可用 `build/cloud-bailian/bin/voice_engine`，省略 `--local-config` 和端侧预检。每次打包使用新的版本名。

两档需要共用语速时，在工程的 `config/speech-local.yaml` 写入 `speed: 0.85`。双后端打包自动将这个值写入端侧 `tts.speed` 和云端 `cloud.tts.speed`。范围为 0.5～2.0。以后只调语速时，修改这一处，复用当前安装版本的程序和两份配置作为打包输入，再启用新版本。

~~~bash
if systemctl --user is-active --quiet rk3588-voice-engine; then
  systemctl --user stop rk3588-voice-engine
fi
python3 scripts/deploy.py stage --project . --binary build/dual-backend/bin/voice_engine \
  --config config/cloud-local.yaml --local-config config/barge-in-local.yaml --release dual-example
python3 scripts/deploy.py verify --release dual-example --profile local
(
  set -e
  set -a
  . ~/.config/rk3588-voice-engine/.env.cloud
  set +a
  python3 scripts/deploy.py verify --release dual-example --profile cloud
)
~~~

两档预检通过后启用版本。预检检查配置、凭据是否已提供、资产和运行库，真人对话需在服务启动后验证。

~~~bash
(
  set -e
  set -a
  . ~/.config/rk3588-voice-engine/.env.cloud
  set +a
  python3 scripts/deploy.py activate --release dual-example
  python3 scripts/deploy.py install-service --template packaging/rk3588-voice-engine.service.in --replace
  systemctl --user start rk3588-voice-engine
)
python3 scripts/deploy.py status
~~~

双后端版本首次启用时默认选云端。要换档，先停止服务，再选择配置并启动。端侧预检使用本地资产，进程启动时启用离线限制。换档会开始新会话。

~~~bash
# 切到端侧
systemctl --user stop rk3588-voice-engine
python3 scripts/deploy.py select-profile local && systemctl --user start rk3588-voice-engine

# 切回云端
systemctl --user stop rk3588-voice-engine
(
  set -e
  set -a
  . ~/.config/rk3588-voice-engine/.env.cloud
  set +a
  python3 scripts/deploy.py select-profile cloud
  systemctl --user start rk3588-voice-engine
)
python3 scripts/deploy.py status
~~~

状态输出区分当前选档和最近一次运行。版本回退、停止和取消命令见[部署与控制](build-and-run.md#本地部署与控制)。回退目标是云端档时，在执行回退预检的终端同样加载凭据。

## 流式识别、播报与检查

- **识别。** 监听开始后建立 WebSocket，本地 VAD 确认开口后补发最近的 `stream_pre_roll_ms` 音频，然后持续上传。百炼模板的开口和插话门槛都为 160 ms，ASR 预卷为 300 ms。预卷可配 0～1000 ms。`stream_queue_ms` 至少容纳预卷和一个 32 ms VAD 块，上限 5000 ms。`connect_ms` 限制连接等待，`first_response_ms` 限制任务启动等待，`idle_ms` 限制有数据待发却无发送进展的时间，`total_ms` 限制判停后的收尾。整段识别使用 `dashscope_asr_json` 和对应 HTTPS 接口。[百炼 WebSocket ASR](https://help.aliyun.com/zh/model-studio/qwen-asr-message-websocket-api)
- **播报。** LLM 增量文本沿用现有分句器。百炼流式 TTS 每段启动一个合成任务，收到二进制 PCM 后重采样并入队，`tts.prebuffer_ms` 设为 0。使用整段 WAV 时选择 `dashscope_tts_wav_url` 和对应 HTTPS 接口。[百炼实时 TTS](https://help.aliyun.com/zh/model-studio/realtime-tts-user-guide)
- **续问与插话。** 云端档正常播完后开始采集新一轮。检测到插话时取消旧播报并保留句首音频。检测到语音但转写为空时，在原 15 秒窗口的剩余时间内继续等待重说。
离线 `streaming_asr` 和 `streaming_tts` 测试使用本机 TLS 夹具，需安装 `openssl`。在服务停止时编译并运行这些测试。

~~~bash
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  cmake --build build/dual-backend --target streaming_asr_test streaming_tts_test -j2
flock -x /tmp/rk3588_voice_engine_model_validation.lock \
  ctest --test-dir build/dual-backend --output-on-failure -R '^streaming_(asr|tts)$'
~~~

真人测试从一次唤醒开始，覆盖首问、免唤醒续问、插话及连续多轮。响应软件计时可用日志中的 `endpoint` 到首个 `playback_submit`，耳机听感单独确认。[控制与日志](build-and-run.md#终端控制与-jsonl-日志)

云端协议适配集中在 `src/cloud/`，公开配置在 `config/`，凭据与用户服务模板在 `packaging/`。WebSocket 客户端的测试入口在 `tests/unit/`，离线 TLS 夹具在 `tests/integration/`。
