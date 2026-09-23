# 嵌入式集成说明

> [English](EMBEDDED.md) | 简体中文

面向 MCU / RTOS 集成者的资源预算与测量指南：体积占用、内存、线程、时序、功耗与日志。
平台契约以[公开头文件](../include/mybot/platform/)为准，集成和生命周期流程见
[PORTING.zh-CN.md](PORTING.zh-CN.md)。请使用目标工具链、产品特性配置和匹配的 Agora RTSA
包测量；主机数据不是 MCU 的资源上限。

## 体积占用

以下为先前记录的 x86_64 Linux 历史快照，使用 GCC 13 和默认优化。原记录没有确切的代码版本
及完整编译选项；这些数值未在当前版本重新测量，不代表当前发布版本的体积保证。

| 制品 | 历史记录体积 |
| --- | --- |
| `libmybot_sdk.a` | 约 490 KB |
| `libaosl.a` | 约 545 KB |
| 最小消费程序（SDK 核心 + AOSL + Agora RTSA，不含 Linux 参考实现） | text 约 890 KB，data 约 74 KB，bss 约 19 KB |

在目标构建上测量，并同时记录代码版本、编译器、编译选项、特性配置和 RTSA 包：

    size <firmware.elf>
    ls -l <build>/libmybot_sdk.a

特性开关直接影响代码体积——`MYBOT_CLOUD_AEC`、`MYBOT_WAKE_WORDS`、`MYBOT_ENABLE_VIDEO`、
`MYBOT_ENABLE_HTTPS` 是主要项；产品用不到的功能请关闭。通过最终链接的固件及 map 文件
归因 Flash 占用；静态库文件大小不等于链接后的 Flash 消耗。随附 RTSA 共享库仅限 x86_64 Linux。

## 内存模型

以下容量根据[管线存储结构](../src/internal/mybot_media_pipeline.h)及
[其实现](../src/media/mybot_media_pipeline.c)计算，不是实测堆峰值。60 ms ptime 下，
一帧 16 kHz 单声道 s16 PCM 为 `F = 16000 × 60 / 1000 × 2 = 1920` 字节。

| 存储 | 60 ms 下按源码计算的容量 |
| --- | --- |
| 应用状态中的固定 PCM 数组：采集、待播放、提示音临时帧、上行发送 | `4F = 7680` 字节 |
| 云端 AEC 额外固定数组：参考帧和双通道交织帧 | `3F = 5760` 字节 |
| 启动时申请的采集和播放环形缓冲 | `2 × 64000 = 128000` 可用字节 |
| 云端 AEC 额外参考环形缓冲 | `64000` 可用字节 |

每个 ring 可容纳 2 秒单声道 PCM（`16000 × 2 × 2 = 64000` 字节，即 62.5 KiB）。
[环形缓冲分配](../src/support/mybot_ringbuf.c)还为每个 ring 增加一个保护字节及元数据，
分配器开销另计。上述数字不包含应用状态的其他部分、队列、线程栈、RTSA 和平台缓冲。
固定 PCM 数组容量随 ptime 变化；2 秒环形缓冲容量不变。

PCM 数组预分配**不代表端到端音频链路没有逐帧堆分配**：

- **RTC 下行，每帧**：[RTC 适配层](../src/rtc/mybot_agora_rtc.c)申请一个 `rtc_event_t`
  和一份 PCM 数据副本，再投递到 `rtc_mpq`。[AOSL](../third_party/aosl) 的 `kernel/mpq.c`
  还会申请排队函数对象及函数名副本。回调将 PCM 复制到播放 ring 后才释放这些临时对象。
- **RTC 上行，每帧**：发送 worker 复用 PCM 数组，但同步调用 `rtc_mpq` 仍会申请 AOSL
  函数对象及函数名副本，并初始化用于等待的 mutex/condition，后者的资源开销取决于 HAL。
  PCM 在调用返回前保持借用。视频提交同样经过这一同步 MPQ 路径；
  SDK 没有视频 ring 不等于没有逐帧分配。
- **会话清理**：消费 worker 排空 ring 时，临时为被清理的 ring 申请一帧大小的缓冲。
  正常采集和播放定时器复用固定数组。
- **控制和 RTM**：服务响应、请求头、JSON 节点和 RTM 消息副本另有临时分配。
  [HTTP](../src/support/mybot_http_client.c)申请 2 KiB 请求缓冲，以及从 4 KiB 增长至
  32 KiB 的接收缓冲。独立的响应 body 在接收缓冲释放前申请；后续 JSON 解析再申请
  与 body 同时存活的节点。32 KiB 接收上限不是单请求总堆占用上限。
- **平台与 RTSA**：提示音资源、摄像头/编码器缓冲、TLS 和 RTSA 内部内存需要单独在目标上
  测量。提示音 `open()` 不要求整份资源载入 RAM，应按实际适配实现核算。

在持续音视频、提示音、HTTP 请求及反复启停场景中测量堆峰值、最小剩余堆、最大连续空闲块
和分配频率。纳入排队数据、AOSL 分配及分配器开销，不能只统计 MyBot 直接调用的
`aosl_hal_malloc()`。网络、配对和会话生命周期行为见 [PORTING.zh-CN.md](PORTING.zh-CN.md)。

## 线程与栈

| MPQ 线程 | 职责 | 请求的栈大小 |
| --- | --- | --- |
| `control_mpq` | 应用状态、设备生命周期、阻塞式 HTTP 控制、UI / 音量和资源转换 | 16 KiB |
| `rtc_mpq` | Agora RTSA 生命周期、串行状态和 vendor 回调分发 | 8 KiB |
| `mybot_mpq` | 按 ptime 节奏上行发送音频 | 16 KiB |
| `cap_mpq` | 麦克风采集 | 16 KiB |
| `pb_mpq` | 播放与 AEC 参考 | 16 KiB |
| `key_stdin_mpq`（仅 Linux 参考） | 标准输入按键事件 | 4 KiB |

五个核心 worker 都存在时，请求的栈大小合计 4 × 16 KiB + 8 KiB = 72 KiB。这是源码预算，
不是实测栈用量；HAL 的最小栈限制和对齐要求可能改变实际分配量。栈大小为编译期常量：
控制线程使用 `src/core/mybot_app.c` 中的
`CONTROL_MPQ_STACK_SIZE`，音频线程使用 `src/media/mybot_media_pipeline.c` 中的
`MEDIA_MPQ_STACK_SIZE`。调整前应在目标上测量每个 worker 的栈高水位，覆盖错误和停止路径。
平台编码器/驱动任务及 RTSA 内部线程需单独计入。线程与缓冲借用契约以
[音频](../include/mybot/platform/mybot_audio.h)和
[视频](../include/mybot/platform/mybot_video.h)头文件为准。

## 时序与实时性

- 音频定时器使用配置的 ptime（20 / 40 / 60 ms；随附 Linux RTSA 为 60 ms）。
  软件包匹配及 I/O 停止要求见 [PORTING.zh-CN.md](PORTING.zh-CN.md)。
- 控制定时器请求 100 ms 间隔，阻塞控制操作会推迟 tick。设备服务轮询由服务端驱动，
  `poll_after_seconds` 会被限制在 3..60 s。运行时在收到第一次 binding-status 响应前使用
  30 s 初始默认值。
- HTTP 客户端对连接、发送、接收检查共享的 5 s deadline，这不是绝对的实际耗时上限：
  普通 TCP 路径先调用不带 timeout 参数的 `aosl_hal_gethostbyname()`，再检查剩余时限。
  测量时应纳入目标 DNS/传输接口的阻塞时间。
- 设备服务 HTTP 在 `control_mpq` 上同步执行，会延迟排队的 UI 和视频控制操作。
  音频 worker 相互独立，但媒体发送及下行分发共享 `rtc_mpq`，还会竞争 CPU 和分配器。
  应在产品负载下测量调度延迟、音频欠载、编码帧 handler 时延及停止耗时。

## 功耗管理

现状：SDK **没有待机 / 低功耗模式**。只要 `mybot_is_running()` 为真，工作线程与定时器
持续运行。省电手段属于集成者：

- **休眠与射频**：分别测量空闲运行和停止 SDK 后的整机电流。配网和射频功耗由平台负责；
  进入低功耗或重新配网时，遵循 [PORTING.zh-CN.md](PORTING.zh-CN.md) 中的网络与启停流程。
- **音频通路**：在音频实现门控 Codec/功放；音量由 SDK 统一管理——已注册的设备音量实现
  （硬件钩子）是首选路径，媒体音量（软件增益）作为兜底。
- **轮询**：间隔由服务端驱动并限制在 3..60 s（运行时初始默认 30 s）；若空闲功耗重要，
  与服务端协商放宽间隔。

开启 `MYBOT_WAKE_WORDS=ON` 时，应将采集 MPQ 上的平台本地 ASR 负载纳入空闲功耗和时序测量；
启用唤醒词本身不会停止播放或发送 worker。

## 日志

- `mybot_start()` 将 AOSL 日志等级至少设为 `AOSL_LOG_NOTICE`；已有 INFO 或 DEBUG 等级保持
  不变。因此，启动前设置的更严格日志阈值不会保留。
- RTSA 使用 `RTC_LOG_ERROR` 初始化。其初始化会修改全局 AOSL 日志等级，适配层会提前保存，
  并在初始化成功或失败后恢复原 AOSL 等级。
- 常规 RTM 消息和转发日志使用 `AOSL_LOG_DBG`；异常保留 warning/error 等级。开启 DEBUG 时
  应测量格式化和日志输出开销，尤其是消息内容打印。
- 当前核心和提示音模块会在 NOTICE 级打印配对码，生产日志处理应考虑这一行为；
  不要增加设备 token 等凭证日志。

上述策略实现在[应用启动](../src/core/mybot_app.c)和
[RTC 适配层](../src/rtc/mybot_agora_rtc.c)中。
