# mybot

跨平台语音聊天机器人。基于 **Agora RTC** 实时音视频协议实现语音传输，**AOSL** 提供跨平台系统抽象层。

设备端通过 HTTP API 与服务端通信完成配对、认证和对话管理，配对的设备使用 RTC 与 ConvoAI Agent 进行语音交互。

## 架构

```
mybot/
├── main/
│   ├── main.c                      # 入口，CLI 参数解析
│   ├── app.h / app.c               # 主循环：MPQ 事件驱动 + 状态机 tick
│   ├── audio/
│   │   ├── audio_device.h / .c     # 音频平台抽象（ops 函数指针注册表）
│   │   └── platform/alsa/          # ALSA 采集/播放实现
│   └── protocols/
│       ├── rtc_session.h / .c      # Agora RTC 会话管理
│       ├── device_api.h / .c       # 设备端服务 API（配对/对话/轮询）
│       └── device_state.h / .c     # 设备生命周期状态机
└── components/
    ├── aosl/                       # AOSL 跨平台系统库
    ├── agora_rtsa_sdk/             # Agora RTSA SDK
    ├── ringbuf/                    # 通用锁无关 SPSC 环缓冲
    └── http_client/                # 阻塞式 HTTP 客户端（AOSL HAL）
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
麦克风 → ALSA采集线程 → capture ringbuf → MPQ定时器(20ms) → RTC发送
                                                               ↓
扬声器 ← ALSA播放线程 ← playback ringbuf ← RTC on_audio_data 回调
```

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

## 设备 API

服务端端点定义见 [DEVICE_API.md](DEVICE_API.md)，已封装的调用：

| 端点 | 函数 | 用途 |
|------|------|------|
| `POST /devices/pair-codes` | `device_api_create_pair_code()` | 申请配对码 |
| `GET /devices/{id}/binding-status` | `device_api_get_binding_status()` | 轮询绑定状态 |
| `POST /devices/{id}/conversations/start` | `device_api_start_conversation()` | 启动对话 |
| `POST /devices/{id}/conversations/stop` | `device_api_stop_conversation()` | 停止对话 |

## 跨平台扩展

音频设备通过 ops 函数指针表实现平台无关化。添加新平台只需在 `main/audio/platform/` 下新建目录并注册 ops：

```c
typedef struct {
    const char *name;
    int  (*init)(void **ctx, int rate, int channels, int bits);
    int  (*start)(void *ctx);
    int  (*read)(void *ctx, void *buf, int frames);
    int  (*write)(void *ctx, const void *buf, int frames);
    int  (*stop)(void *ctx);
    void (*destroy)(void *ctx);
} audio_capture_ops_t;
```

## 项目结构

| 目录 | 内容 |
|------|------|
| `main/` | 应用入口、主循环、音频抽象、协议层 |
| `components/aosl/` | 跨平台系统抽象层（线程/内存/网络/日志） |
| `components/agora_rtsa_sdk/` | Agora RTSA SDK v1.10.1 |
| `components/ringbuf/` | 通用锁无关 SPSC 环缓冲 |
| `components/http_client/` | 基于 AOSL HAL 的 HTTP 客户端 |

## 项目状态

早期开发阶段。
