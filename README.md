# mybot

面向设备端的跨平台语音机器人 SDK。工程使用 Agora RTC 传输实时音频，AOSL 提供线程、消息队列、网络、内存和日志等跨平台基础能力。

设备通过服务端完成配网后的配对、认证和对话管理，并与 ConvoAI Agent 建立 RTC 语音会话。

## 目录结构

```text
mybot/
├── include/mybot/                  # SDK 公开头文件
│   ├── mybot.h                     # 应用生命周期与会话控制 API
│   ├── mybot_build_config.h        # 编译期功能配置
│   └── platform/                   # 平台移植接口（ops）
├── src/                            # SDK 私有实现
│   ├── core/                       # 应用编排与设备生命周期
│   ├── service/                    # 设备服务客户端
│   ├── media/                      # 音频与唤醒词抽象实现
│   ├── rtc/                        # Agora RTC 会话实现
│   ├── platform/                   # 平台接口注册层
│   ├── support/                    # HTTP、JSON、ring buffer
│   └── internal/                   # 非公开头文件
├── platforms/linux/                # Linux 平台后端与聚合注册入口
├── examples/linux/                 # Linux CLI 示例应用
├── tests/
│   ├── unit/                       # SDK 单元测试
│   └── platform/linux/             # Linux 平台测试
├── third_party/
│   ├── aosl/                       # AOSL 跨平台系统库
│   └── agora_rtsa_sdk/             # Agora RTSA SDK
└── cmake/                          # 工具链与构建辅助文件
```

目录边界对应三个独立 CMake 目标：

- `mybot::sdk`：跨平台 SDK 核心，不包含具体设备后端和程序入口。
- `mybot::platform_linux`：Linux ALSA、stdin、文件存储、控制台 LCD 和宿主网络后端。
- `mybot`：位于 `examples/linux/` 的示例可执行程序。

## 构建 Linux 示例

环境要求：Linux x86_64、CMake 3.16+、C99 编译器和 ALSA 开发库。

```bash
sudo apt install -y libasound2-dev
cmake -S . -B build -DCONFIG_PLATFORM=linux
cmake --build build -j
ctest --test-dir build --output-on-failure
```

示例程序输出为 `build/examples/linux/mybot`：

```bash
./build/examples/linux/mybot \
  --server http://localhost:3001 \
  --device-id AG-DEMO-001
```

Linux 文件型 KV 存储默认写入当前目录的 `.mybot-kv-store/`，可通过 `MYBOT_KV_STORE_DIR` 指定其他目录。Linux 交互按键为：`s` 启动对话、`q` 停止对话、`p` 重新配对、`e` 退出。

## 作为子模块集成

宿主 CMake 工程可直接引入 SDK：

```cmake
set(CONFIG_PLATFORM your_aosl_platform CACHE STRING "" FORCE)
set(AGORA_RTC_LIBRARY /path/to/target/libagora-rtc-sdk.a CACHE FILEPATH "" FORCE)

add_subdirectory(third_party/mybot)
target_link_libraries(device_firmware PRIVATE mybot::sdk)
```

当 mybot 作为子目录引入时，Linux 后端、示例、测试和 ASAN 默认关闭。宿主可在 `add_subdirectory()` 前显式覆盖：

```cmake
set(MYBOT_BUILD_LINUX_PLATFORM OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MYBOT_ENABLE_ASAN OFF CACHE BOOL "" FORCE)
```

平台移植代码只需包含 `<mybot/platform/*.h>` 并注册所需 ops：

- 音频采集和播放：`mybot_audio_device_register_capture/playback()`
- APSTA 配网：`mybot_wifi_provisioning_register()`
- KV 存储：`mybot_kv_store_register()`
- 按键事件：`mybot_key_service_register()`
- LCD：`mybot_lcd_register()`
- 本地唤醒词：`mybot_wake_words_register()`（功能开启时必需）

Linux 平台通过 `linux_platform_register()` 一次性注册全部后端；其他 MCU/RTOS 平台应在自己的 `platforms/<platform>/` 或宿主工程中提供对应聚合入口。

## 编译配置

配置头文件为 `include/mybot/mybot_build_config.h`，可由编译器宏覆盖：

- `MYBOT_AUDIO_PTIME_MS`：20、40 或 60 ms，默认 60 ms。Agora 上行 PCM duration 与下行 jitter frame duration 使用相同值。
- `MYBOT_WAKE_WORDS`：本地 ASR 唤醒词，默认关闭。开启后仅在应用已就绪且设备处于 runtime 时处理采集 PCM；识别成功走与物理按键相同的会话启动路径。
- `MYBOT_CLOUD_AEC`：服务端回声消除，默认开启。

示例：

```bash
cmake -S . -B build \
  -DCONFIG_PLATFORM=linux \
  -DCMAKE_C_FLAGS="-DMYBOT_AUDIO_PTIME_MS=20 -DMYBOT_WAKE_WORDS=1"
```

## 运行流程

```text
APSTA provisioning → STA connected → pairing → awaiting_claim → runtime ↔ in_conversation
```

Wi-Fi 配网是第一个业务阶段。`mybot_wifi_provisioning_init()` 非阻塞启动 APSTA 后端，只有收到 STA connected 事件后才初始化存储、按键、音频、设备服务和 RTC。

对话音频数据流：

```text
麦克风 → capture MPQ(ptime) → capture ring buffer → RTC send(ptime)
              └→ 本地 ASR（仅空闲 runtime 状态）

扬声器 ← playback MPQ(ptime) ← playback ring buffer ← RTC callback
```

## 开发约定

公开 API 统一使用 `mybot_` 前缀。外部集成只能依赖 `include/mybot/`；`src/internal/` 和 `src/support/` 不属于兼容性承诺。

仓库根目录的 `.clang-format` 是自研 C 代码的格式规范：

```bash
find include src platforms examples tests \
  -type f \( -name '*.c' -o -name '*.h' \) -exec clang-format -i {} +
```

`third_party/` 保持上游目录与命名，不参与自研源码格式化。

## 项目状态

早期开发阶段，API 尚未发布。
