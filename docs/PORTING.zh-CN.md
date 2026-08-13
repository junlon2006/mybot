# 将 mybot 移植到新平台

> [English](PORTING.md) | 简体中文

本文档定义 mybot 1.0.0 的跨平台集成规范。公开 API 与 ABI 遵循语义化版本。平台代码必须只
包含 `include/mybot` 下的头文件，并链接 `mybot::sdk`。

mybot 设计为可移植到几乎任意平台——Linux、RTOS 或裸机——只要 AOSL 有对应的
`CONFIG_PLATFORM` 移植、且目标 ABI 存在 Agora RTSA 库。无论宿主操作系统或芯片厂商如何，
以下规范完全一致。

## 第 1 步：核实前置条件

提供 C99 编译器、CMake 3.16+、AOSL `CONFIG_PLATFORM` 移植，以及为目标精确 ABI 构建的
Agora RTSA 头文件与库。从 git 检出构建时，先执行 `git submodule update --init --recursive`
初始化 AOSL submodule。设备还需要到兼容设备服务端的 TLS 连接能力、持久化凭据存储，以及
16 kHz 单声道有符号 16-bit PCM 输入输出。随仓库附带的 Agora 库仅用于 x86_64 Linux，不能
复用于其他架构。

## 第 2 步：创建平台目录布局

将平台代码保持在 `src/` 之外：

```text
platforms/my_mcu/
  CMakeLists.txt
  my_mcu_platform.c
  my_mcu_platform.h
  my_mcu_audio.c
  my_mcu_wifi.c
  my_mcu_https.c
  my_mcu_kv_store.c
  my_mcu_key.c
  my_mcu_lcd.c          # 可选
  my_mcu_announce.c     # 可选（配对码语音播报）
  my_mcu_wake_words.c   # 可选
```

独立（out-of-tree）固件工程可使用相同布局，无需改动本仓库。

## 第 3 步：实现必需平台能力

在 `mybot_start()` 之前，每个必需实现恰好注册一次。

### 音频

从 `mybot_audio.h` 实现完整的采集与播放回调表。生命周期为
`init -> start -> 反复 read/write -> stop -> destroy`。

- 格式为 16000 Hz、单声道、每样本 16 位。
- 传入与返回 `read` / `write` 的计数是帧，不是字节。
- 有进展返回短的正计数，无进展返回 0，出错返回负值。绝不能返回超过请求的帧数。
- PCM 指针仅在回调期间借用。
- I/O 运行在专用的 AOSL MPQ 工作线程上。阻塞必须有界，以便关闭流程能完成。
- `stop` 应解除在途 I/O 的阻塞；`destroy` 在工作线程停止后释放上下文。

使用 `mybot_audio_register_capture()` 与 `mybot_audio_register_playback()`
注册。

#### 设备音量（可选）

音量由 SDK 统一管理，不对外提供音量控制 API；音量变化（例如音量按键事件）走以下两条
路径之一：

- **设备音量**是首选路径。实现 `mybot_audio_volume_ops_t` 并调用
  `mybot_audio_device_register_volume()`，即可让 SDK 的音量变化路由到真实硬件（Codec
  寄存器、功放或混音器）。`init`、`set_volume`、`destroy` 为必需；`get_volume` 可选，
  仅用于同步 SDK 的音量状态。SDK 在启动时初始化该实现，关闭时释放。
- **媒体音量**是未注册设备音量实现时的兜底。SDK 保存 0..100 的软件增益，播放流水线在
  PCM 到达设备前以数字方式应用（线性幅度，100 为原增益）。无需平台代码。

初始化失败仅禁用设备音量控制；SDK 回退到软件增益，播放不受影响。

### Wi-Fi 配网

实现 `mybot_wifi_ops_t`。`init` 无需等待用户即启动 APSTA。将 connected、
disconnected 与 failed 事件仅在连接状态转换时发出。只有 STA 已获取 IP 且网络可供 SDK
请求使用时才能上报 connected。事件可来自平台线程，但必须串行传递，且 `destroy` 返回后
不得再运行任何事件。Destroy 必须停止传输并等待在途回调。实现在首次成功连接后必须持续监控
网络连接，并上报运行期断开与重连事件；SDK 在离线期间暂停设备服务流量，重连后恢复。使用
`mybot_wifi_register()` 注册。

Linux 参考实现立即上报已连接，不是真正的 APSTA 参考实现。

### 持久化键值存储

实现 `mybot_kv_store_ops_t` 的全部回调。

- `get` 成功返回 0，条目缺失返回 `MYBOT_ERR_NOT_FOUND`，失败返回负值。必须遵守
  `capacity`，且只在成功时设置 `out_len`。
- `set` 应能在断电时存活，且不暴露部分替换。
- `erase` 是幂等的。
- 使用适当的访问控制或加密保护设备 token。

使用 `mybot_kv_store_register()` 注册。

### HTTPS 传输

生产构建保持 `MYBOT_ENABLE_HTTPS=ON`，并在 `mybot_start()` 之前注册一个
`mybot_https_ops_t`。封装 mbedTLS、BearSSL 或芯片厂商 TLS socket API；SDK 核心
不链接 OpenSSL。实现必须：

- 在给定超时内建立 TCP 与 TLS；
- 将 DNS 主机作为 SNI 发送，对照受维护的信任库验证证书链，并校验证书主机名；
- 有进展时 `send` / `recv` 返回正的字节数，`recv` 仅在对方干净关闭时返回 0，出错或超时
  返回 -1；
- `close` 释放整个 TLS 连接。

使用 `mybot_https_register()` 注册。不要为开发证书关闭证书或主机名校验；请将
所需 CA 安装到设备信任库。Linux 参考实现使用 OpenSSL 与系统 CA 库。明文 HTTP 仅存在于
配置了 `MYBOT_ENABLE_HTTPS=OFF -DMYBOT_ALLOW_INSECURE_HTTP=ON` 的隔离开发构建。

### 按键

实现 `mybot_key_ops_t`，将硬件输入转换为会话开始、停止、配对、退出、音量增大与
音量减小事件。SDK 收到音量事件时将音量步进 10——设备音量实现激活时调整真实硬件音量，
否则回退到媒体音量软件增益；音量事件为可选。事件可以是异步的。Destroy 必须停止输入源
并等待所有处理器。使用 `mybot_key_register()` 注册。

### LCD（可选）

存在显示设备时实现 `mybot_lcd_ops_t`。渲染接收语义内容，可能从不同 SDK 线程调用；SDK
负责串行化调用。内容为借用。使用 `mybot_lcd_register()` 注册。

### 唤醒词（可选）

仅在 `MYBOT_WAKE_WORDS=ON` 时必需。`process()` 回调接收借用的 PCM。异步实现必须复制需要保留的
数据，destroy 必须等待所有检测处理器。使用 `mybot_wake_words_register()` 注册。

### 配对码语音播报（可选）

实现 `mybot_announce_ops_t` 在扬声器播报配对码提示（“请在控制台输入配对码 xxx”）。SDK 与
平台交换的 PCM 一律为原始 16 kHz 单声道有符号 16-bit——SDK 核心不含音频解码器，平台需
自行把资源解码/重采样到该格式。

拿到配对码后，SDK 将固定提示音与逐位数字音依次入队，通过正常播放链路**只播报一次**；
设备离开 `awaiting_claim` 状态（认领成功、重新配对或离线）时立即停止。提示音缺失则跳过
整段播报，某个数字音缺失则只跳过该位——配对流程不会被音频阻塞。

接口约定：`init` 初始化实现；`open` 打开一个逻辑声音（`MYBOT_ANNOUNCE_SOUND_PROMPT`、
`MYBOT_ANNOUNCE_SOUND_DIGIT_0`..`9`），可做 I/O；`read` 最多拷贝 `max_frames` 帧，必须
轻量（运行在实时播放线程上）；`close` / `destroy` 释放句柄与实现。在 `mybot_start()` 之前
用 `mybot_announce_register()` 注册。该实现可选：未注册时 SDK 跳过本地播报，仅记日志。

Linux 参考实现按语言从 `./assets/locales/<locale>/` 读取原始 PCM 文件（`prompt.pcm`、
`0.pcm`~`9.pcm`）；可用环境变量 `MYBOT_ASSETS_DIR` 覆盖目录（默认 `./assets`），
`MYBOT_LOCALE` 选择语言（默认 `zh-CN`）。

## 第 4 步：添加一个注册入口

```c
int my_mcu_platform_register(void) {
    if (my_mcu_wifi_register() < 0 || my_mcu_https_register() < 0 ||
        my_mcu_kv_register() < 0 ||
        my_mcu_key_register() < 0 || my_mcu_audio_capture_register() < 0 ||
        my_mcu_audio_playback_register() < 0) {
        return -1;
    }
    return 0;
}
```

逐一检查并返回每个注册调用的错误。启用 LCD、唤醒词或配对码播报时，再加入对应的注册调用。

## 第 5 步：与 CMake 集成

```cmake
set(CONFIG_PLATFORM my_mcu CACHE STRING "" FORCE)
set(AGORA_SDK_DIR /opt/agora-rtsa-target CACHE PATH "" FORCE)
set(AGORA_RTC_LIBRARY /opt/agora-rtsa-target/lib/libagora-rtc-sdk.a CACHE FILEPATH "" FORCE)
set(MYBOT_BUILD_LINUX_PLATFORM OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MYBOT_ENABLE_HTTPS ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/mybot)
add_library(my_mcu_platform STATIC ${MY_MCU_PLATFORM_SOURCES})
target_link_libraries(my_mcu_platform PUBLIC mybot::sdk)
target_link_libraries(device_firmware PRIVATE mybot::sdk my_mcu_platform)
```

两个相互独立的变量选择平台代码：`CONFIG_PLATFORM` 选择 `third_party/aosl` 消费的 AOSL HAL
端口（如 `linux`、`esp32`）；`MYBOT_BUILD_LINUX_PLATFORM` 构建随附的 Linux 参考实现
（`platforms/linux/`）并要求 `CONFIG_PLATFORM=linux`。MCU 移植保持
`MYBOT_BUILD_LINUX_PLATFORM=OFF`。宿主可预定义名为 `agora-rtc-sdk` 的导入目标。本版本支持
`add_subdirectory()` 源码集成，也支持通过 `find_package(mybot)` 消费已安装包。源码集成需先
初始化 mybot 的嵌套 AOSL submodule（在 mybot 检出目录执行
`git submodule update --init --recursive`）。

## 第 6 步：启动与停止

```c
if (my_mcu_platform_register() < 0) fail_startup();

mybot_config_t config = {0};
copy_checked(config.server_base, sizeof(config.server_base), server_url);
copy_checked(config.device_id, sizeof(config.device_id), device_id);
if (mybot_start(&config) < 0) fail_startup();

while (mybot_is_running()) platform_sleep_ms(100);
mybot_stop();
```

`server_base` 必须是 HTTPS URL，且两个字段都必须是非空、以 NUL 结尾的字符串。未注册 TLS
传输时，启动会在全局初始化之前失败。启动是非阻塞的；Wi-Fi 上报网络可用后服务继续运行。不要
从平台回调中调用 stop，因为它会等待工作线程与回调。mybot 当前拥有进程级 AOSL 与 Agora
生命周期。

## 第 7 步：交叉编译

```bash
cmake -S firmware -B build-target \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
  -DCONFIG_PLATFORM=my_mcu \
  -DAGORA_SDK_DIR=/opt/agora-rtsa-target \
  -DAGORA_RTC_LIBRARY=/opt/agora-rtsa-target/lib/libagora-rtc-sdk.a
cmake --build build-target -j
```

对照 Agora 库核实字节序、指针宽度、libc、编译器与浮点 ABI。

## 第 8 步：验收清单

- 公开头文件以警告即错误编译通过，宿主只链接文档化的目标。
- HTTPS 拒绝不受信任的 CA、过期证书、错误主机名、缺失 SNI 与握手超时。
- Wi-Fi 连接、断开与失败路径无死锁完成。
- KV 在重置后存活，处理缺失与溢出，并保护凭据。
- 采集/播放通过 20、40 与 60 ms 的 16 kHz 单声道 S16 测试。
- 短 I/O 有进展，stop 能在设备丢失时解除阻塞。
- `destroy` 返回后没有按键或唤醒词回调运行；LCD 不保留借用的内容。
- 部分启动失败与重复 start/stop 释放全部资源。
- 真实设备完成配网、配对、RTC 加入、双向音频、挂断与重启。
- 日志与存储不暴露 token。

## 已知限制

- HTTPS 需要平台 TLS 实现与受维护的 CA 信任库。Linux 提供 OpenSSL 参考实现；MCU 移植必须
  集成其 TLS 栈。
- RTC 是 Agora 专用，注册表是单例，且仅支持单应用实例。
- Linux 参考实现是开发参考，不是生产级配网或安全存储。
- 第三方再分发权需单独核实；见 `THIRD_PARTY_NOTICES.md`。
