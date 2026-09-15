# 嵌入式集成说明

> [English](EMBEDDED.md) | 简体中文

面向 MCU / RTOS 集成者的资源与时序事实：体积占用、内存、线程、功耗与日志。以下数值来自
x86_64 Linux 参考构建（GCC 13，默认优化），**仅供参考**——请始终使用目标工具链、真实配置
（`-Os`、实际启用的特性开关）与目标架构的 Agora RTSA 包重新测量。

## 体积占用

| 制品 | 体积（x86_64 参考） |
| --- | --- |
| `libmybot_sdk.a` | 约 490 KB |
| `libaosl.a` | 约 545 KB |
| 最小消费程序（SDK 核心 + AOSL + Agora RTSA，不含 Linux 参考实现） | text 约 890 KB，data 约 74 KB，bss 约 19 KB |

目标上的测量方法：

    size <firmware.elf>
    ls -l <build>/libmybot_sdk.a

特性开关直接影响代码体积——`MYBOT_CLOUD_AEC`、`MYBOT_WAKE_WORDS`、`MYBOT_ENABLE_VIDEO`、
`MYBOT_ENABLE_HTTPS` 是主要项；产品用不到的功能请关闭。在 MCU 上 Agora RTSA 库通常占掉
大部分 Flash，且必须使用目标架构的包（随附共享库仅限 x86_64 Linux）。

## 内存模型

- **静态逐帧音频缓冲**（位于应用状态内）：每个工作线程一份 16 kHz 单声道 16 位帧——采集、
  待播放、上行发送与 AEC 参考各约 1.9 KB（60 ms ptime 即 960 样本）；AEC 交织缓冲约 3.8 KB。
- **环形缓冲**：每个容纳 2 秒音频 = 64 KB。采集与播放恒有；开启 `MYBOT_CLOUD_AEC=ON` 时
  额外创建 AEC 参考环形缓冲，合计 192 KB。
- **配对播报（可选）**：拿到配对码时，平台通过 announcement ops 提供原始 16 kHz 单声道
  s16 PCM（SDK 不含解码器）。播放 worker 将提示音与 RTC 播放 ring 分开，末尾不足一帧时补零，
  通过正常扬声器链路只播报一次。提示音和数字音资源在播放期间临时载入内存。
- **视频上行（可选）**：启用 `MYBOT_ENABLE_VIDEO` 后，平台负责摄像头采集和 JPEG/H.264/H.265
  编码，SDK 不做编码、解码或视频接收。编码帧通过借用内存的 handler 直接送入 RTC；SDK 不
  建立视频 ring buffer，RTSA 根据上行带宽回调 `on_target_bitrate_changed()`，平台编码器据此
  调整码率。`MYBOT_VIDEO_MIN_BPS` 和 `MYBOT_VIDEO_MAX_BPS` 限制初始 RTSA BWE 范围，初始值
  使用两者中点；每帧大小受 `MYBOT_VIDEO_MAX_FRAME_BYTES` 限制，目标需将 RTSA 每帧内部的
  分包分配计入堆预算。
- **堆**：HTTP 请求缓冲固定 2 KB，响应初始分配 4 KB、单请求最大增长到 32 KB（用后即释放）；
  生命周期的认证/会话响应和请求头、RTM LCD 状态解析的临时消息缓冲也通过
  `aosl_hal_malloc` 有界申请并在本次操作结束时释放。JSON 解析与平台实现（ALSA、OpenSSL、
  文件 KV）同样存在临时分配，各平台可重定向该分配器。
- 控制面状态（应用、设备生命周期、RTC 会话）仍为静态分配；上述 heap 只出现在控制/RTM
  事务路径，不用于 PCM 逐帧处理。平台应将这些上限纳入 heap 预算，并在分配失败时保留当前
  状态，不得继续使用未完成的响应。

线程安全的应用状态模型使用一个原子快照统一保存运行阶段、网络状态和设备生命周期投影。
`mybot_get_state()` 从该快照派生公开状态：离线时 `MYBOT_STATE_WIFI_DISCONNECTED` 优先；
在线但未配网、配对或等待认领时返回 `MYBOT_STATE_PAIRING`，只有认证后的 runtime 返回
`MYBOT_STATE_READY`；设备服务接受会话后返回 `MYBOT_STATE_IN_CONVERSATION`，正常拆除后回到
`MYBOT_STATE_READY`。

## 线程与栈

| MPQ 线程 | 职责 | 栈 |
| --- | --- | --- |
| `control_mpq` | 应用状态、设备生命周期、阻塞式 HTTP 控制、UI / 音量和资源转换 | 16 KB |
| `rtc_mpq` | Agora RTSA 生命周期、串行状态和 vendor 回调分发 | 8 KB |
| `mybot_mpq` | 按 ptime 节奏上行发送音频 | 16 KB |
| `cap_mpq` | 麦克风采集 | 16 KB |
| `pb_mpq` | 播放与 AEC 参考 | 16 KB |
| `key_stdin_mpq`（仅 Linux 参考） | 标准输入按键事件 | 4 KB |

核心栈预算合计 4 × 16 KB + 8 KB = 72 KB。栈大小为编译期常量：
控制线程使用 `src/core/mybot_app.c` 中的
`CONTROL_MPQ_STACK_SIZE`，音频线程使用 `src/media/mybot_media_pipeline.c` 中的
`MEDIA_MPQ_STACK_SIZE`；请在目标上实测后再调整。实时音频定时器位于独立 MPQ，PCM 保持
数据面直达而不经过 `control_mpq`，因此阻塞式 HTTP 或控制工作不会拖垮音频通路。控制回调
（包括唤醒词回调）只投递短事件或发布原子 mailbox。Agora RTSA SDK 内部另有厂商管理的
线程。RTSA 回调会复制借用的数据并投递到 `rtc_mpq`；应用回调在该 worker 中运行，不得重入
RTC 接口。

## 时序与实时性

- 音频格式固定为 16 kHz、单声道、16 位有符号；ptime 为 20 / 40 / 60 ms（默认 60 ms，
  即每帧 960 样本 / 1920 字节）。
- 所选 ptime 必须与目标 RTSA 软件包的 `CONFIG_MINIMAL_TIMER_INTERVAL_MS` 一致；仓库附带的
  Linux 软件包为 60 ms 版本。
- 关闭时 SDK 会先调用采集和播放的 `stop`，再等待音频工作线程退出。每个 `stop` 必须安全
  中断在途 `read` / `write`；有界 I/O 超时仍作为驱动异常时的兜底（Linux ALSA 实现使用
  50 ms 轮询超时）。
- 状态机每 100 ms tick 一次；设备服务轮询由服务端驱动，`poll_after_seconds` 会被限制在
  3..60 s。运行时在收到第一次 binding-status 响应前使用 30 s 初始默认值。
- HTTP 请求总时限 5 s。
- 设备服务 HTTP 在 `control_mpq` 上同步执行；已排队的 UI 或控制动作可能在在途请求后等待最长
  该时限，但 PCM 数据面仍独立运行。
- `mybot_start()` 与 `mybot_stop()` 是线程安全的。stop 会等待工作线程与回调，禁止从平台或
  SDK 回调内调用。

## 功耗管理

现状：SDK **没有待机 / 低功耗模式**。只要 `mybot_is_running()` 为真，工作线程与定时器
持续运行。省电手段属于集成者：

- **休眠**：进入低功耗前调用 `mybot_stop()`，唤醒后 `mybot_start()`；这会释放
  工作线程、音频设备、TLS、RTC 资源以及 mybot 持有的 AOSL 运行时引用。RTSA 结束流程会在
  mybot 释放应用引用之前完成。
- **射频**：Wi-Fi 配网实现拥有射频，平台的低功耗策略在那里实现。
- **音频通路**：在音频实现门控 Codec/功放；音量由 SDK 统一管理——已注册的设备音量实现
  （硬件钩子）是首选路径，媒体音量（软件增益）作为兜底。
- **轮询**：间隔由服务端驱动并限制在 3..60 s（运行时初始默认 30 s）；若空闲功耗重要，
  与服务端协商放宽间隔。

开启 `MYBOT_WAKE_WORDS=ON` 时，平台本地 ASR 实现运行在采集 MPQ 上，必须功耗感知（这也是
空闲时仅保持麦克风通路存活的天然位置）。

## 日志

- 日志来自 AOSL；用 `aosl_set_log_level()` 设置运行级别（debug 到 error）。Linux 参考打印
  到 stdout。
- RTC 会话按默认 NOTICE 阈值初始化 Agora RTSA SDK；除非平台移植自行覆盖，否则抑制较低优先级
  SDK 信息日志。
- 热路径（音频定时器）日志尽量少——每次调用都会做格式化。
- 绝不打设备 token。参考应用在 INFO 级打印配对码；生产构建应脱敏。

完整平台集成规范与验收清单见 [PORTING.md](PORTING.zh-CN.md)。
