# 将 mybot 移植到新平台

> [English](PORTING.md) | 简体中文

本文介绍如何将 mybot 1.2.0 集成到产品。接口契约、所有权和回调线程要求以
[mybot.h](../include/mybot/mybot.h) 与[平台公共头文件](../include/mybot/platform/)为准。
公开 API 与 ABI 遵循语义化版本。平台代码必须只包含 `include/mybot` 下的头文件，
并链接 `mybot::sdk`。

mybot 设计为可移植到几乎任意平台——Linux、RTOS 或裸机——只要 AOSL 有对应的
`CONFIG_PLATFORM` 移植、且目标 ABI 存在 Agora RTSA 库。无论宿主操作系统或芯片厂商如何，
以下集成流程均适用。

## 第 1 步：核实前置条件

提供 C99 编译器、CMake 3.16+、AOSL `CONFIG_PLATFORM` 移植，以及为目标精确 ABI 构建的
Agora RTSA 头文件与库。从 git 检出构建时，先执行 `git submodule update --init --recursive`
初始化 AOSL submodule。设备还需要到兼容设备服务端的 TLS 连接能力、持久化凭据存储，以及
16 kHz 单声道有符号 16-bit PCM 输入输出。随仓库附带的 Agora 库仅用于 x86_64 Linux，不能
复用于其他架构。

## 第 2 步：创建平台目录布局

MCU 平台代码在独立固件仓库中实现，该仓库可采用以下布局：

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
  my_mcu_video.c        # 可选（编码视频上行）
  my_mcu_wake_words.c   # 可选
```

通过 SDK 公共平台 ops 集成，不将板级源码或资源复制到本仓库。

## 参考移植工程

以下独立仓库维护 MCU 平台适配、板级配置、资源与固件构建，设备集成请直接使用对应工程。
SDK 仓库保留 Linux 参考实现，不再同步 MCU 源码副本。

- [mybot-bk7258](https://github.com/junlon2006/mybot-bk7258)：BK7258；
- [mybot-bk7259](https://github.com/junlon2006/mybot-bk7259)：BK7259；
- [mybot-esp32](https://github.com/junlon2006/mybot-esp32)：ESP32。

可用这些工程了解目标构建集成、AOSL 平台接线、描述符注册和固件生命周期。参考工程存在
差异时，接口契约和所有权边界以 SDK 公共头文件为准。

## 第 3 步：实现必需平台操作

实现以下 ops 表，并在第 4 步通过平台描述符一次性注册。
完整的回调、返回值和生命周期契约请查阅对应公共头文件。

### 音频

从 `mybot_audio.h` 实现完整的采集与播放回调表。生命周期为
`init -> start -> 反复 read/write -> stop -> destroy`。

- 格式为 16000 Hz、单声道、每样本 16 位。
- 传入与返回 `read` / `write` 的计数是帧，不是字节。
- 有进展返回短的正计数，无进展返回 0，出错返回负值。绝不能返回超过请求的帧数。
- PCM 指针仅在回调期间借用。
- I/O 运行在专用的 AOSL MPQ 工作线程上。阻塞必须有界，以便关闭流程能完成。
- SDK 会从关闭线程调用采集与播放的 `stop`，然后才等待任一工作线程退出。`stop` 必须线程
  安全，并立即解除在途 `read` / `write` 的阻塞；对应工作线程退出后才会调用 `destroy`。

将两个 ops 表加入平台描述符。

#### 设备音量（可选）

音量由 SDK 统一管理，不对外提供音量控制 API；音量变化（例如音量按键事件）走以下两条
路径之一：

- **设备音量**是首选路径。实现 `mybot_audio_volume_ops_t` 并加入平台描述符，即可让 SDK
  的音量变化路由到真实硬件（Codec 寄存器、功放或混音器）。`init`、`set_volume`、
  `destroy` 为必需；`get_volume` 可选，
  仅用于同步 SDK 的音量状态。SDK 在启动时初始化该实现，关闭时释放。
- **媒体音量**是未注册设备音量实现时的兜底。SDK 保存 0..100 的软件增益，播放流水线在
  PCM 到达设备前以数字方式应用（线性幅度，100 为原增益）。无需平台代码。

初始化失败仅禁用设备音量控制；SDK 回退到软件增益，播放不受影响。

### 网络连接

实现 [mybot_wifi_ops_t](../include/mybot/platform/mybot_wifi.h)，监听产品已有的网络管理器，
并将其加入平台描述符。产品配网、凭据存储和建立网络连接独立于 MyBot。`init` 注册 SDK
监听并上报当前可用连接；`destroy` 解除该监听，不关闭产品网络。事件顺序和回调生命周期
遵循头文件契约。

MyBot 运行期间持续上报断网和恢复事件。SDK 离线时暂停设备服务请求，重连后恢复；被中断的
会话在本地结束。进入配网是独立的产品操作，应遵循第 6 步的停止/启动顺序。
Linux 适配器假定宿主已联网并立即上报已连接，不实现配网，也不监控宿主链路变化。

### 持久化键值存储

实现 `mybot_kv_store_ops_t` 的全部回调。

- `get` 成功返回 0，条目缺失返回 `MYBOT_ERR_NOT_FOUND`，失败返回负值。必须遵守
  `capacity`，且只在成功时设置 `out_len`。
- `set` 应能在断电时存活，且不暴露部分替换。
- `erase` 是幂等的。
- 使用适当的访问控制或加密保护设备 token。

将 ops 表加入平台描述符。

### HTTPS 传输

生产构建保持 `MYBOT_ENABLE_HTTPS=ON`，并提供一个 `mybot_https_ops_t`。封装 mbedTLS、
BearSSL 或芯片厂商 TLS socket API；SDK 核心
不链接 OpenSSL。实现必须：

- 在给定超时内建立 TCP 与 TLS；
- 将 DNS 主机作为 SNI 发送，对照受维护的信任库验证证书链，并校验证书主机名；
- 有进展时 `send` / `recv` 返回正的字节数，`recv` 仅在对方干净关闭时返回 0，出错或超时
  返回 -1；
- `close` 释放整个 TLS 连接。

将 ops 表加入平台描述符。不要为开发证书关闭证书或主机名校验；请将所需 CA 安装到设备
信任库。Linux 参考实现使用 OpenSSL 与系统 CA 库。明文 HTTP 仅存在于
配置了 `MYBOT_ENABLE_HTTPS=OFF -DMYBOT_ALLOW_INSECURE_HTTP=ON` 的隔离开发构建。

### 按键

实现 `mybot_key_ops_t`，将硬件输入转换为会话开始、停止、配对、退出、音量增大与
音量减小事件。SDK 收到音量事件时将音量步进 10——设备音量实现激活时调整真实硬件音量，
否则回退到媒体音量软件增益；音量事件为可选。事件可以是异步的。Destroy 必须停止输入源
并等待所有处理器。将 ops 表加入平台描述符。

对于单个硬件切换键，处理按键时查询线程安全的 `mybot_get_state()`：仅在
`MYBOT_STATE_READY` 时发出 `MYBOT_KEY_EVENT_CONVERSATION_START`，仅在
`MYBOT_STATE_IN_CONVERSATION` 时发出 `MYBOT_KEY_EVENT_CONVERSATION_STOP`。等待网络确认
（`MYBOT_STATE_WIFI_PROVISIONING`）、启动、配对、断网、失败和停止状态均忽略切换键。
`MYBOT_STATE_PAIRING` 覆盖设备服务的
`unprovisioned`、`pairing` 和 `awaiting_claim` 阶段。不要根据 LCD 推断会话状态，也不要在
平台侧维护第二份状态；运行期断网会报告 `MYBOT_STATE_WIFI_DISCONNECTED`，SDK 会在本地结束会话。

### LCD（可选）

存在显示设备时实现 `mybot_lcd_ops_t`。渲染由应用 control owner 调用，SDK 不会并发调用。
内容为借用。将 ops 表加入平台描述符。

`mybot_lcd_content_t.indicators` 是用于非互斥叠加提示的位图。在
`MYBOT_LCD_SCREEN_IN_CONVERSATION` 中，`MYBOT_LCD_INDICATOR_VP_REGISTERED` 表示当前会话的声纹
已由服务器注册成功，平台应将其渲染为会话界面的附加指示器。平台应忽略无法识别的位，不能
根据该指示器自行推断生命周期状态。该位清零时，平台可以自行显示“注册中/未注册”标记，
但这只是显示状态，不能改变 SDK 生命周期。`MYBOT_LCD_INDICATOR_LISTENING`、
`MYBOT_LCD_INDICATOR_THINKING` 和 `MYBOT_LCD_INDICATOR_SPEAKING` 分别表示服务端正在监听、
思考或播报，后三个位互斥。

### 唤醒词（可选）

仅在 `MYBOT_WAKE_WORDS=ON` 时必需。`process()` 回调接收借用的 PCM。异步实现必须复制需要保留的
数据，destroy 必须等待所有检测处理器。将 ops 表加入平台描述符。

### 配对码语音播报（可选）

实现 `mybot_announce_ops_t` 在扬声器播报配对码提示（“请在控制台输入配对码 xxx”）。SDK 与
平台交换的 PCM 一律为原始 16 kHz 单声道有符号 16-bit——SDK 核心不含音频解码器，平台需
自行把资源解码/重采样到该格式。

拿到配对码后，SDK 将固定提示音与逐位数字音依次入队，通过正常播放链路**只播报一次**；
提示音由播放 worker 直接选择并保持在独立的待播放帧中，不写入 RTC 播放 ring；末尾不足一帧时补零，
不会与 RTC 音频拼接。提示音播放期间收到的 RTC 下行音频会丢弃；
设备离开 `awaiting_claim` 状态（认领成功、重新配对或离线）时立即停止。提示音缺失则跳过
整段播报，某个数字音缺失则只跳过该位——配对流程不会被音频阻塞。

接口约定：`init` 初始化实现；`open` 打开一个逻辑声音（`MYBOT_ANNOUNCE_SOUND_PROMPT`、
`MYBOT_ANNOUNCE_SOUND_DIGIT_0`..`9`），可做 I/O；`read` 最多拷贝 `max_frames` 帧，必须
轻量（运行在实时播放线程上）；`close` / `destroy` 释放句柄与实现。将 ops 表加入平台
描述符。该实现可选：未提供时 SDK 跳过本地播报，仅记日志。

Linux 参考实现按语言从 `./assets/locales/<locale>/` 读取原始 PCM 文件（`prompt.pcm`、
`0.pcm`~`9.pcm`）；可用环境变量 `MYBOT_ASSETS_DIR` 覆盖目录（默认 `./assets`），
`MYBOT_LOCALE` 选择语言（默认 `zh-CN`）。

### 视频上行（可选）

启用 `MYBOT_ENABLE_VIDEO=ON` 时，实现 `mybot_video_ops_t` 并将其加入平台描述符。平台负责
摄像头采集和 JPEG、H.264 或 H.265 编码；SDK 不编码、不解码，也不接收服务端视频。平台应在
ops 表的 `min_bps` 和 `max_bps` 中设置编码器支持的视频码率范围。SDK 校验该范围，并在创建
RTC connection 后调用 `agora_rtc_set_bwe_param()` 设置；`start_bps` 使用范围中点。

编码器通过 `init()` 注册的 frame handler 推送完整编码帧，帧数据在 handler 返回前为借用内存，
且必须来自普通任务上下文而非 ISR。`stop()` 必须停止编码器并等待所有进行中的 handler 返回，
随后才能执行 `destroy()`。
SDK 在 `control_mpq` 上串行调用全部视频操作，包括码率调整和关键帧请求。RTC 回调只投递控制
通知，已经结束的会话通知会被丢弃；编码帧仍由编码器任务直接提交。

视频只在 RTC 状态为 `CONNECTED` 时启动，会话离开时先停止编码器再离开 RTC。SDK 不建立视频
ring buffer，发送失败直接丢弃当前帧。RTSA 会在 `agora_rtc_send_video_data()` 返回前完成分包
和输入复制；平台仍需将帧大小限制在 `MYBOT_VIDEO_MAX_FRAME_BYTES` 以内。
H.264/H.265 应提供 RTSA packetizer 要求的完整 Annex-B access unit；JPEG 应是完整 JPEG 帧。

`on_target_bitrate_changed()` 会收到 RTSA 根据上行带宽计算的目标码率（bit/s），编码器应在
自身支持范围内及时调整码率。`on_key_frame_request()` 可选，用于响应 RTSA 的关键帧请求。当前
SDK 只发送一个主视频流，并明确关闭远端视频订阅。

## 第 4 步：注册一个平台描述符

```c
#include <mybot/platform/mybot_platform.h>

static const mybot_platform_descriptor_t my_mcu_platform = {
    .wifi = &my_mcu_wifi_ops,
    .kv_store = &my_mcu_kv_ops,
    .key = &my_mcu_key_ops,
    .audio_capture = &my_mcu_capture_ops,
    .audio_playback = &my_mcu_playback_ops,
    .https = &my_mcu_https_ops,
    .lcd = &my_mcu_lcd_ops,
};

int my_mcu_platform_register(void) {
    return mybot_platform_register(&my_mcu_platform);
}
```

非空 ops 指针是平台支持对应功能的唯一声明；不支持的可选功能将指针留为 NULL。
`mybot_platform_register()` 会在提交前校验全部必需回调表以及每个已提供的可选回调表，
因此失败不会留下只注册一半的平台。它是唯一的平台注册入口。
`mybot_start()` 会另行按当前构建与运行配置检查所需 ops，例如 HTTPS URL 需要 HTTPS ops，
启用唤醒词时需要唤醒词 ops，启用视频时需要 video ops（包括码率回调）。

## 第 5 步：与 CMake 集成

```cmake
set(CONFIG_PLATFORM my_mcu CACHE STRING "" FORCE)
set(AGORA_SDK_DIR /opt/agora-rtsa-target CACHE PATH "" FORCE)
set(AGORA_RTC_LIBRARY /opt/agora-rtsa-target/lib/libagora-rtc-sdk.so CACHE FILEPATH "" FORCE)
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

产品配网与 SDK 生命周期控制应保持独立职责：

1. 产品使用已保存的凭据连接网络，或完成自身配网流程；网络可用后通知产品控制任务。
2. 产品控制任务调用 `mybot_start()`，Wi-Fi 适配器上报已有连接，MyBot 才开始设备服务配对、
   认证与会话流程。
3. 再次进入配网前，由产品控制任务调用 `mybot_stop()` 并等待其完成，然后才将共享设备交给
   配网流程；网络重新可用后再启动 MyBot。进入配网、配网成功等提示音归产品配网流程所有，
   SDK 的配对码播报归 MyBot 所有。

生命周期接口契约见 [mybot.h](../include/mybot/mybot.h)。下例由产品控制任务在网络可用后执行：

```c
if (my_mcu_platform_register() < 0) fail_startup();

mybot_config_t config = {0};
copy_checked(config.server_base, sizeof(config.server_base), server_url);
copy_checked(config.device_id, sizeof(config.device_id), device_id);
if (mybot_start(&config) < 0) fail_startup();

while (mybot_is_running()) platform_sleep_ms(100);
mybot_stop();
```

将 `server_base` 设置为 HTTPS URL，`device_id` 设置为非空标识符，并保证两个字段均以
NUL 结尾。`mybot_start()` 建立 SDK 控制线程与网络监听，首次收到已连接事件后才启动设备服务。
历史名称
`MYBOT_STATE_WIFI_PROVISIONING` 与 `MYBOT_LCD_SCREEN_WIFI_PROVISIONING` 表示等待初次网络
可用确认，不表示 SDK 正在执行配网。不要从平台回调中调用 stop，因为它会等待工作线程与
回调。`mybot_start()` 获取一份应用持有的 AOSL 引用，`mybot_stop()` 在工作线程、缓冲区和
RTC 回调队列全部销毁后最后释放该引用。RTC 生命周期、状态和 vendor 回调由专用
`rtc_mpq` 串行处理，应用回调不得重入 RTC 接口。
RTSA 生命周期通过 `agora_rtc_init()` / `agora_rtc_fini()` 管理。宿主若直接使用 AOSL，
必须自行配对 `aosl_ctor()` 与 `aosl_dtor()`，并在所有 AOSL 用户停止前保持该引用。

使用 `mybot_get_state()` 决定当前可执行的产品操作，返回状态的含义以
[mybot.h](../include/mybot/mybot.h) 为准。不要在平台侧重建 SDK 私有的设备服务状态机。

普通临时断网时保持 MyBot 运行，由产品网络管理器使用已有凭据重连，适配器依次上报断开与
重新可用的连接，不必每次断网都重新进入配网。SDK 会结束被中断的会话，并在重连后恢复
设备服务请求；新的会话需要重新触发。

### RTM 账号映射

Agora RTM 集成遵循 xiaozhi 参考流程。设备服务的启动会话响应提供服务端生成的
`rtc.uid`（也称 `local_uid`），并在顶层提供点对点消息使用的 `agent_uid`。SDK 原样使用
`rtc.uid` 作为 RTC user account 和本地 RTM UID。启用 RTM token 鉴权时应使用独立的 RTM
凭据；当前服务响应只提供 `rtc.token`，部署方必须确认服务端/项目鉴权模式允许该 token
用于 RTM，或扩展服务协议下发 RTM token。`agent_uid` 是传给
`mybot_agora_rtc_send_rtm_data()` 的对端 UID；`AG-*` 设备 ID 仅用于设备服务身份，不能替代
这两个账号。

RTM UID 必须非空、长度小于 64 字节，并且只能包含 Agora 接受的 ASCII 字母、数字、空格和
标点字符。`mybot_agora_rtc_rtm_uid_is_valid()` 暴露了登录前使用的同一校验规则。RTM 登录是
异步的：只有 `MYBOT_RTM_EVENT_LOGIN` 回调的错误码为 0 后才允许发送消息。RTM 负载上限为
31 KiB，custom type 上限为 32 字节。
设备服务客户端会拒绝超出目标缓冲区的响应字符串；RTC token 缓冲区为 512 字节内容预留了
额外的结尾 NUL 字节。

会话启动时 SDK 会先请求 RTM 登录，并最多等待 5 秒直到收到成功的
`MYBOT_RTM_EVENT_LOGIN` 回调；随后订阅与 RTC channel 同名的 RTM channel，并最多等待 5 秒
直到订阅成功。两步均成功前不会创建或加入 RTC connection，因此平台必须保证 control owner
等待期间 RTM 回调线程仍可运行。声纹状态只接受当前会话 channel、当前 agent 发送且字段严格
匹配 `"object":"message.sal_status"` 与 `"status":"VP_REGISTER_SUCCESS"` 的消息。SDK 在
会话开始和结束时重置这些状态，并通过 `MYBOT_LCD_INDICATOR_VP_REGISTERED` 暴露声纹成功指示，
通过三个服务端状态位暴露对话阶段；P2P RTM 回调继续保留用于点对点消息。

## 第 7 步：交叉编译

```bash
cmake -S firmware -B build-target \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
  -DCONFIG_PLATFORM=my_mcu \
  -DAGORA_SDK_DIR=/opt/agora-rtsa-target \
  -DAGORA_RTC_LIBRARY=/opt/agora-rtsa-target/lib/libagora-rtc-sdk.so
cmake --build build-target -j
```

`AGORA_RTC_LIBRARY` 可指向共享库或静态库。需对照所选 Agora 库核实字节序、指针宽度、
libc、编译器与浮点 ABI；若使用共享目标包，还需部署该库并配置固件或操作系统的运行时加载器。

所选 `MYBOT_AUDIO_PTIME_MS` 必须与 RTSA 软件包中的 `CONFIG_MINIMAL_TIMER_INTERVAL_MS`
一致。仓库附带的 x86_64 Linux 软件包固定为 60 ms；使用 20 或 40 ms 时，请通过
`AGORA_SDK_DIR` 和 `AGORA_RTC_LIBRARY` 指向匹配的软件包。CMake 会读取 `.config` 或
`include/global_config.cmake`；配置不一致或缺少元数据时直接失败。若宿主预先定义了
`agora-rtc-sdk` 目标，`AGORA_SDK_DIR` 仍必须指向同一个软件包以进行校验。
CMake 无法检查宿主导入目标的 ABI，宿主必须确保其头文件和库路径来自该软件包。

## 第 8 步：验收清单

- 公开头文件以警告即错误编译通过，宿主只链接文档化的目标。
- HTTPS 拒绝不受信任的 CA、过期证书、错误主机名、缺失 SNI 与握手超时。
- SDK 注册监听时能收到已有可用连接，运行期断开、重连和失败事件无死锁完成。
- 进入配网前等待 `mybot_stop()` 完成，产品仅在网络重新可用后启动 MyBot。
- KV 在重置后存活，处理缺失与溢出，并保护凭据。
- 使用对应 RTSA 软件包时，采集/播放通过所选 ptime 的 16 kHz 单声道 S16 测试（仓库附带
  Linux 软件包覆盖 60 ms）。
- 短 I/O 有进展，stop 能在设备丢失时解除阻塞。
- `destroy` 返回后没有按键或唤醒词回调运行；LCD 不保留借用的内容。
- 部分启动失败与重复 start/stop 会在 worker 可 join 后释放全部资源；若 worker join 失败，SDK
  会保留相关资源，等待后续 stop 再次尝试。
- 正常 runtime 会话期间 `mybot_get_state()` 依次报告 `READY -> IN_CONVERSATION -> READY`；通话中断网
  时报告 `WIFI_DISCONNECTED`，重连后恢复为对应的在线状态，停止和重新配对过程不得死锁。
- 真实设备完成配网、配对、RTC 加入、双向音频、挂断与重启。
- 会话结束时，采集、播放和 AEC reference 缓冲会在各自消费者 worker 中排空，完成后才允许新会话。
- AEC reference 仅在播放设备接受对应样本后提交；提示音 PCM 永远不会进入 reference 流。
- 日志与存储不暴露 token。

## 已知限制

- HTTPS 需要平台 TLS 实现与受维护的 CA 信任库。Linux 提供 OpenSSL 参考实现；MCU 移植必须
  集成其 TLS 栈。
- RTC 是 Agora 专用，平台注册为进程级，且仅支持单应用实例。
- Linux 参考实现是开发参考，不是生产级配网或安全存储。
- 第三方再分发权需单独核实；见 `THIRD_PARTY_NOTICES.md`。
