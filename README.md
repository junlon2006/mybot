# mybot

跨平台语音聊天机器人。基于 **Agora RTC** 实时音视频协议实现语音传输，**AOSL** 提供跨平台系统抽象层（线程、内存、网络、日志等），所有硬件相关代码通过 AOSL 平台 HAL 适配。

## 架构

```
mybot/
├── main/
│   ├── main.c                      # 入口，CLI 参数解析
│   ├── app.h / app.c               # 应用生命周期（AOSL MPQ 主循环）
│   ├── audio/
│   │   ├── audio_device.h / .c     # 音频平台抽象（ops 函数指针注册表）
│   │   └── platform/alsa/          # ALSA 采集/播放实现
│   └── protocols/
│       └── rtc_session.h / .c      # Agora RTC 会话管理
└── components/
    ├── aosl/                       # AOSL 跨平台系统库
    ├── agora_rtsa_sdk/             # Agora RTSA SDK
    └── ringbuf/                    # 通用锁无关 SPSC 环缓冲
```

### 数据流

```
麦克风 → ALSA采集线程 → capture ringbuf → MPQ定时器(20ms) → RTC发送
                                                               ↓
扬声器 ← ALSA播放线程 ← playback ringbuf ← RTC on_audio_data 回调
```

## 环境要求

- Linux x86_64（首批支持）
- CMake >= 3.16
- GCC >= 13
- ALSA 开发库

### 安装依赖

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
# 需要两个终端/设备，使用相同的 App ID 和频道名
./mybot --app_id <APP_ID> --channel mytest --user client_a
./mybot --app_id <APP_ID> --channel mytest --user client_b

# 按 Ctrl+C 停止
```

### 命令行参数

| 参数 | 必填 | 说明 |
|------|------|------|
| `--app_id` | 是 | Agora 项目 App ID |
| `--channel` | 是 | 频道名称 |
| `--token` | 否 | 身份认证 Token（空值则使用无 token 模式） |
| `--user` | 否 | 用户标识字符串（默认: `mybot_user`） |
| `-h` | 否 | 显示帮助 |

## 跨平台扩展

音频设备通过 ops 函数指针表实现平台无关化。添加新平台只需在 `main/audio/platform/` 下新建目录并注册 ops：

```c
// audio_device.h 中定义的平台接口
typedef struct {
    const char *name;
    int  (*init)(void **ctx, int rate, int channels, int bits);
    int  (*start)(void *ctx);
    int  (*read)(void *ctx, void *buf, int frames);   // capture
    int  (*write)(void *ctx, const void *buf, int frames); // playback
    int  (*stop)(void *ctx);
    void (*destroy)(void *ctx);
} audio_capture_ops_t;   // 和 audio_playback_ops_t
```

## 项目状态

早期开发阶段 — 最小原型已跑通编译。
