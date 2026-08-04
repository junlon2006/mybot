# mybot

跨平台语音聊天机器人。基于 **Agora RTC** 实时音视频协议实现语音传输，**AOSL** 提供跨平台系统抽象层。

设备端通过 HTTP API 与服务端通信完成配对、认证和对话管理，配对的设备使用 RTC 与 ConvoAI Agent 进行语音交互。

## 架构

```
mybot/
├── main/
│   ├── main.c                      # 入口：CLI 解析、信号处理、按键循环
│   ├── app.h / app.c               # 跨平台应用层：音频/MPQ/状态机，非阻塞启动
│   ├── audio/
│   │   └── audio_device.h / .c     # 音频平台抽象（ops 函数指针注册表）
│   ├── board/
│   │   └── linux/                  # 板级适配层：Linux ALSA 采集/播放实现
│   └── protocols/
│       ├── rtc_session.h / .c      # Agora RTC 会话管理
│       ├── device_api.h / .c       # 设备端服务 API（配对/对话/轮询）
│       └── device_state.h / .c     # 设备生命周期状态机
└── components/
    ├── aosl/                       # AOSL 跨平台系统库
    ├── agora_rtsa_sdk/             # Agora RTSA SDK
    ├── ringbuf/                    # 通用锁无关 SPSC 环缓冲
    ├── http_client/                # 阻塞式 HTTP 客户端（AOSL HAL）
    └── cJSON/                      # JSON 解析库（以 mybot_cJSON_* 前缀导出）
```

### 生命周期

```
unprovisioned → pairing → awaiting_claim ──→ runtime ←→ in_conversation
                              │               │
                          expired         unbound
                          重启配对        回到起始
```

| 阶段 | 说明 |
|------|------|
| `pairing` | 调用 `POST /devices/pair-codes` 获取配对码 |
| `awaiting_claim` | 轮询 `GET /binding-status`，等待用户在 Web 端认领 |
| `runtime` | 获得 `device_token`，定期 poll 检测解绑，可启动对话 |
| `in_conversation` | 通过 RTC 与 ConvoAI Agent 语音交互 |

### 数据流（对话中）

```
麦克风 → cap_mpq 线程(10ms) → capture ringbuf → send_timer(20ms, mybot_mpq) → RTC 发送
                                                                                  ↓
扬声器 ← pb_mpq 线程(10ms) ← playback ringbuf ← RTC on_audio_data 回调（SDK 线程）
```

设备状态机运行在独立的 `state_mpq` 线程（100ms 轮询，含阻塞 HTTP 请求），不影响实时音频。上行开启云 AEC 时，`send_timer` 将麦克风 PCM 与扬声器下行 PCM 交错发送。所有线程均通过 `aosl_mpq_create()` 创建（跨平台，不依赖 `aosl_hal_thread_join`）。

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

## 使用

```bash
# 需要先启动设备端 API 服务（参考 DEVICE_API.md）
./mybot --server http://localhost:3001 --device-id AG-DEMO-001
```

### 命令行参数

| 参数 | 必填 | 说明 |
|------|------|------|
| `--server` | 是 | 设备 API 服务端地址 |
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

## 设备 API

服务端端点定义见 [DEVICE_API.md](DEVICE_API.md)，已封装的调用：

| 端点 | 函数 | 用途 |
|------|------|------|
| `POST /devices/pair-codes` | `mybot_device_api_create_pair_code()` | 申请配对码 |
| `GET /devices/{id}/binding-status` | `mybot_device_api_get_binding_status()` | 轮询绑定状态 |
| `POST /devices/{id}/conversations/start` | `mybot_device_api_start_conversation()` | 启动对话 |
| `POST /devices/{id}/conversations/stop` | `mybot_device_api_stop_conversation()` | 停止对话 |

## 跨平台扩展

音频设备通过 ops 函数指针表实现平台无关化。板级适配层位于 `main/board/`，添加新平台只需在 `main/board/` 下新建对应平台目录（如 `linux/`）并注册 ops：

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

## 项目结构

| 目录 | 内容 |
|------|------|
| `main/` | 应用入口、跨平台应用层、音频抽象、协议层 |
| `main/board/` | 板级跨平台适配层（当前仅 `linux/`，ALSA 实现） |
| `components/aosl/` | 跨平台系统抽象层（线程/内存/网络/日志） |
| `components/agora_rtsa_sdk/` | Agora RTSA SDK v1.10.1 |
| `components/ringbuf/` | 通用锁无关 SPSC 环缓冲 |
| `components/http_client/` | 基于 AOSL HAL 的 HTTP 客户端 |
| `components/cJSON/` | JSON 解析库 |

## 项目状态

早期开发阶段。
