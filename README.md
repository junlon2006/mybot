# mybot

跨平台语音聊天机器人。基于 **Agora RTC** 实时音视频协议实现语音传输，**AOSL** 提供跨平台系统抽象层。

设备端与服务端通信完成配对、认证和对话管理，配对的设备使用 RTC 与 ConvoAI Agent 进行语音交互。

## 架构

```
mybot/
├── main/
│   ├── mybot_app.h / .c                  # 跨平台应用编排
│   ├── mybot_build_config.h              # 编译期功能配置
│   ├── audio/
│   │   └── mybot_audio_device.h / .c     # 音频平台抽象（ops 函数指针注册表）
│   ├── storage/
│   │   └── mybot_kv_store.h / .c         # 键值存储抽象（设备凭证等）
│   ├── key_service/
│   │   └── mybot_key_service.h / .c      # 按键事件与平台后端抽象
│   ├── wifi/
│   │   └── mybot_wifi_provisioning.h / .c # APSTA Wi-Fi 配网抽象
│   ├── device/
│   │   ├── mybot_device_client.h / .c    # 设备服务客户端（配对/对话/轮询）
│   │   └── mybot_device_lifecycle.h / .c # 设备生命周期状态机
│   ├── rtc/
│   │   ├── mybot_rtc_session.h           # RTC 会话接口
│   │   └── mybot_rtc_session.c           # Agora RTC 会话实现
│   └── platform/
│       └── linux/
│           ├── mybot_main.c              # Linux 入口、CLI 与信号处理
│           ├── mybot_audio_*_alsa.c      # ALSA 音频后端
│           ├── mybot_kv_store_file.c     # 文件型键值存储后端
│           ├── mybot_key_stdin.c         # stdin 按键后端
│           └── mybot_wifi_host_network.c # Linux 宿主网络后端
└── components/
    ├── aosl/                              # AOSL 跨平台系统库
    ├── agora_rtsa_sdk/                    # Agora RTSA SDK
    ├── ringbuf/                           # SPSC 环缓冲（mybot_ringbuf_*）
    ├── http_client/                       # HTTP 客户端（mybot_http_client_*）
    └── json/                              # 命名空间化 cJSON 实现（mybot_json_*）
```

仓库根目录的 `.clang-format` 是唯一的 C 代码格式规范。格式化自研源码时执行：

```bash
find main tests components/ringbuf components/http_client components/json \\
    -type f \\( -name '*.c' -o -name '*.h' \\) -exec clang-format -i {} +
```

### 生命周期

```
APSTA provisioning → STA connected → unprovisioned → pairing → awaiting_claim ──→ runtime ←→ in_conversation
                                                                        │               │
                                                                    expired         unbound
                                                                    重启配对        回到起始
```

Wi-Fi 配网是应用启动后的第一个业务阶段。只有 STA 连接成功后，才会继续初始化持久化、
按键、音频、设备服务配对和 RTC。平台后端固定使用 APSTA，不提供其他 Wi-Fi mode 选择。
Linux 开发后端复用宿主机已经配置的网络，因此会立即报告 STA 已连接。

| 阶段 | 说明 |
|------|------|
| `APSTA provisioning` | 启动 AP 配网入口并由 STA 建立上行网络连接 |
| `STA connected` | 首次联网屏障已通过，允许启动其余应用服务 |
| `pairing` | 向服务端申请配对码 |
| `awaiting_claim` | 等待用户在 Web 端认领设备 |
| `runtime` | 获得 `device_token`，定期 poll 检测解绑，可启动对话 |
| `in_conversation` | 通过 RTC 与 ConvoAI Agent 语音交互 |

### 数据流（对话中）

```
麦克风 → cap_mpq 线程(ptime) → capture ringbuf → send_timer(ptime, mybot_mpq) → RTC 发送
                                                                                  ↓
扬声器 ← pb_mpq 线程(ptime) ← playback ringbuf ← RTC on_audio_data 回调（SDK 线程）
```

设备生命周期状态机运行在独立的 `state_mpq` 线程（100ms 轮询，含阻塞 HTTP 请求），不影响实时音频。上行开启云 AEC 时，`send_timer` 将麦克风 PCM 与扬声器下行 PCM 交错发送。所有线程均通过 `aosl_mpq_create()` 创建（跨平台，不依赖 `aosl_hal_thread_join`）。

## 环境要求

- Linux x86_64
- CMake >= 3.16
- GCC >= 13
- ALSA 开发库

```bash
sudo apt install -y libasound2-dev
```

## 构建

```bash
git clone <repo-url> mybot
cd mybot
mkdir -p build && cd build
cmake .. -DCONFIG_PLATFORM=linux
make -j$(nproc)
```

音频包时长由 `main/mybot_build_config.h` 中的 `MYBOT_AUDIO_PTIME_MS` 配置，支持 20、40、60 ms，默认 60 ms。Agora 上行 PCM 编码时长与下行 jitter buffer 输出帧长均使用该值。可通过编译参数覆盖：

```bash
cmake .. -DCONFIG_PLATFORM=linux -DCMAKE_C_FLAGS="-DMYBOT_AUDIO_PTIME_MS=20"
```

## 使用

```bash
./mybot --server http://localhost:3001 --device-id AG-DEMO-001
```

Linux 文件型键值存储默认将设备凭证保存在当前目录的 `.mybot-kv-store/`。
可通过 `MYBOT_KV_STORE_DIR` 指定其他持久化目录；部署时应选择仅设备进程可访问的位置。

### 命令行参数

| 参数 | 必填 | 说明 |
|------|------|------|
| `--server` | 是 | 服务端地址 |
| `--device-id` | 是 | 设备唯一标识符（如 `AG-A1B2C3`） |
| `--fw-ver` | 否 | 固件版本 |
| `--hw-model` | 否 | 硬件型号 |

### 交互按键

| 按键 | 功能 |
|------|------|
| `s` | 启动对话 |
| `q` | 停止对话 |
| `p` | 重新配对 |
| `e` | 退出程序（等同 Ctrl+C） |

## 跨平台扩展

Wi-Fi 配网、音频设备、持久化存储和按键服务通过 ops 函数指针表实现平台无关化。平台适配层位于 `main/platform/`，添加新平台时在该目录下新建对应平台目录并注册 ops：

```c
typedef struct {
    const char *name;
    int  (*init)(void **ctx, int rate, int channels, int bits);
    int  (*start)(void *ctx);
    int  (*read)(void *ctx, void *buf, int frames);
    int  (*write)(void *ctx, const void *buf, int frames);
    int  (*stop)(void *ctx);
    void (*destroy)(void *ctx);
} mybot_audio_capture_ops_t;
```

> 公开接口统一使用 `mybot_` 前缀（类型、函数、宏），防止集成到其它工程时符号冲突；`components/aosl` 与 `components/agora_rtsa_sdk` 保持上游命名。
>
> `components/json` 直接基于 cJSON 实现，但所有外部类型、宏和函数均使用 `mybot_json_*` / `MYBOT_JSON_*` 命名；工程不声明原始 `cJSON_*` API，避免作为子模块集成时发生符号冲突。

## 项目结构

| 目录 | 内容 |
|------|------|
| `main/` | 跨平台应用层与各子系统接口 |
| `main/device/` | 设备服务客户端与生命周期状态机 |
| `main/storage/` | 平台无关的键值存储接口 |
| `main/rtc/` | RTC 会话接口及服务商实现 |
| `main/wifi/` | APSTA Wi-Fi 配网抽象 |
| `main/platform/` | 平台后端（当前为 Linux 宿主网络、ALSA、文件存储和 stdin） |
| `components/aosl/` | 跨平台系统抽象层（线程/内存/网络/日志） |
| `components/agora_rtsa_sdk/` | Agora RTSA SDK v1.10.1 |
| `components/ringbuf/` | 通用锁无关 SPSC 环缓冲 |
| `components/http_client/` | 基于 AOSL HAL 的 HTTP 客户端 |
| `components/json/` | 直接基于 cJSON 的命名空间化 JSON 实现 |

## 项目状态

早期开发阶段。
