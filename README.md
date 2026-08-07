# mybot

`mybot` 是面向设备端的跨平台语音机器人 SDK。它负责 APSTA 配网、设备配对和认证、会话状态机、双向音频、Agora RTC 连接、按键/LCD 工作流，以及可选的本地唤醒词识别。平台相关能力通过小型 `ops` 接口注册，SDK 核心不依赖 Linux ALSA、stdin 或文件系统实现。

当前版本为 **0.1.0-rc.1**，公开 API 尚未承诺稳定。仓库内置的 Agora RTSA 二进制和 AOSL 有独立的许可及使用条件；用于产品前请先阅读[许可证与第三方依赖](#许可证与第三方依赖)。

## 能力与边界

- 音频固定为 16 kHz、单声道、16-bit PCM；`ptime` 可配置为 20/40/60 ms，默认 60 ms。
- Wi-Fi 接口面向 APSTA 配网，非阻塞启动，通过事件推进应用状态机。
- 本地 ASR 唤醒词为可选平台后端，默认关闭；唤醒行为与物理按键启动会话一致。
- LCD 接收配网、配对码、就绪和会话中等语义状态，由平台决定具体显示方式。
- RTC 实现专用于 Agora RTSA，不提供其他 RTC 协议适配层。
- 设备服务默认只接受 HTTPS。Linux 使用 OpenSSL 和系统 CA；MCU 由平台接入 mbedTLS
  或芯片厂商 TLS，并必须验证证书链和服务端主机名。
- 设备服务端不属于本仓库；运行示例需要兼容的服务地址。

## 仓库结构

```text
mybot/
├── include/mybot/          # SDK 公共头文件和平台契约
├── src/                    # 跨平台实现；internal/ 不属于公共 API
├── platforms/linux/        # Linux 参考后端（ALSA/stdin/file/console）
├── examples/linux/         # Linux 示例应用入口
├── tests/                  # 单元、平台和宿主集成测试
├── docs/PORTING.md         # 新平台逐步移植指南和验收契约
├── cmake/                  # 工具链辅助文件
└── third_party/            # AOSL 与 Agora RTSA SDK
```

主要 CMake 目标：

- `mybot::sdk`：跨平台 SDK 核心。
- `mybot::platform_linux`：Linux 参考后端，不属于跨平台核心。
- `mybot::linux_example`：Linux CLI 示例。

## Linux 快速开始

环境要求：Linux x86_64、CMake 3.16+、C99 编译器、ALSA 和 OpenSSL 开发包。仓库当前附带的 Agora RTSA 静态库也是 x86_64 Linux 版本。

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libasound2-dev libssl-dev
cmake -S . -B build -DCONFIG_PLATFORM=linux -DMYBOT_ENABLE_ASAN=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
```

运行示例：

```bash
./build/examples/linux/mybot \
  --server https://api.example.com \
  --device-id AG-DEMO-001 \
  --fw-ver 0.1.0-rc.1 \
  --hw-model linux-reference
```

Linux 后端直接使用宿主机现有网络，并立即报告 STA 已连接；它只是开发用替身，不实现真实 APSTA 配网。音频使用 ALSA `default` 设备。KV 数据默认写入当前目录的 `.mybot-kv-store/`，可通过 `MYBOT_KV_STORE_DIR` 修改。就绪后可输入 `s` 开始会话、`q` 停止会话、`p` 重新配对、`e` 退出。

## 集成到宿主工程

推荐将仓库作为源码子模块引入。宿主需要为目标架构准备匹配的 Agora RTSA 头文件/静态库，并确保 AOSL 已支持目标平台。

```cmake
set(CONFIG_PLATFORM my_mcu CACHE STRING "" FORCE)
set(AGORA_SDK_DIR /opt/agora-rtsa CACHE PATH "" FORCE)
set(AGORA_RTC_LIBRARY /opt/agora-rtsa/lib/libagora-rtc-sdk.a CACHE FILEPATH "" FORCE)

set(MYBOT_BUILD_LINUX_PLATFORM OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MYBOT_AUDIO_PTIME_MS 60 CACHE STRING "" FORCE)
set(MYBOT_WAKE_WORDS OFF CACHE BOOL "" FORCE)
set(MYBOT_ENABLE_HTTPS ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/mybot)
target_link_libraries(device_firmware PRIVATE mybot::sdk)
```

平台必须在 `mybot_app_start()` 之前注册 Wi-Fi、KV、按键、音频采集、音频播放和
HTTPS 传输后端。LCD 可选；只有 `MYBOT_WAKE_WORDS=ON` 时才必须注册本地 ASR 后端。
Linux 平台自动注册 OpenSSL 后端；其他平台实现
`mybot_https_transport_ops_t`。完整实现顺序、最小代码、线程约束和验收清单见
[docs/PORTING.md](docs/PORTING.md)。

最小应用生命周期：

```c
platform_register_all();
mybot_app_start(&config);
while (mybot_app_is_running()) {
    platform_sleep_ms(100);
}
mybot_app_stop();
```

`mybot_app_start()` 非阻塞。它首先启动配网，收到 STA connected 事件后才异步初始化存储、按键、音频、设备服务和 RTC。`mybot_app_stop()` 会等待工作线程退出，不应从平台事件回调内部调用。

## 构建配置

以下选项可在宿主 `add_subdirectory()` 前设置，也可通过 CMake 命令行传入：

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `MYBOT_AUDIO_PTIME_MS` | `60` | 音频包长，只接受 20、40、60 ms |
| `MYBOT_CLOUD_AEC` | `ON` | 服务端 AEC；上行包含麦克风和参考声道 |
| `MYBOT_WAKE_WORDS` | `OFF` | 启用本地 ASR 唤醒词平台后端 |
| `MYBOT_AI_QOS` | `ON` | Agora AI QoS |
| `MYBOT_FAST_SEND_MULTIPLIER` | `3` | 快发倍数，只接受 1 到 5 |
| `MYBOT_SHOW_TRANSCRIPT` | `OFF` | 请求实时转写数据流 |
| `MYBOT_ENABLE_HTTPS` | `ON` | 启用平台 HTTPS 传输，生产构建应保持开启 |
| `MYBOT_ALLOW_INSECURE_HTTP` | `OFF` | 仅本地开发：显式允许明文 HTTP |
| `MYBOT_ENABLE_ASAN` | `OFF` | GCC/Clang 地址消毒器，建议在宿主测试中开启 |

例如：

```bash
cmake -S . -B build-wake \
  -DCONFIG_PLATFORM=linux \
  -DMYBOT_AUDIO_PTIME_MS=20 \
  -DMYBOT_WAKE_WORDS=ON
```

Linux 参考平台没有本地 ASR 后端，因此开启 `MYBOT_WAKE_WORDS` 后需要由宿主额外注册后端，否则应用会明确启动失败。

明文 HTTP 不会自动回退。仅在隔离的本地开发环境中，可显式配置
`-DMYBOT_ENABLE_HTTPS=OFF -DMYBOT_ALLOW_INSECURE_HTTP=ON`。该组合会传输设备凭据和
RTC 参数的明文，不得用于设备、共享网络或发布构建。

## 工作流

```text
APSTA provisioning -> STA connected -> pairing -> awaiting claim
                                             |
                                             v
                              runtime <-> in conversation
```

```text
microphone -> capture worker -> local wake words (idle only)
                         \-> capture ring buffer -> Agora RTC uplink

speaker <- playback worker <- playback ring buffer <- Agora RTC downlink
```

## 开发与验证

```bash
cmake -S . -B build -DCONFIG_PLATFORM=linux -DMYBOT_ENABLE_ASAN=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
find include src platforms examples tests -type f \
  \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format --dry-run --Werror
```

自研 C 代码遵循根目录 `.clang-format`；`third_party/` 保持上游内容。提交补丁前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。版本变化记录见 [CHANGELOG.md](CHANGELOG.md)，安全问题报告方式见 [SECURITY.md](SECURITY.md)。

## 许可证与第三方依赖

仓库自研代码按根目录 [LICENSE](LICENSE) 中的 Apache License 2.0 发布，但这不改变第三方组件的许可：

- AOSL 带有其 `third_party/aosl/LICENSE` 中列出的附加条件。
- Agora RTSA SDK 二进制受其软件许可、试用期和商业授权要求约束。
- `mybot_json` 派生自 cJSON，保留 MIT 许可声明。

发布产品或重新分发前必须单独核实这些条款。详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
