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
| 最小消费程序（SDK 核心 + AOSL + Agora RTSA，不含 Linux 后端） | text 约 890 KB，data 约 74 KB，bss 约 19 KB |

目标上的测量方法：

    size <firmware.elf>
    ls -l <build>/libmybot_sdk.a

特性开关直接影响代码体积——`MYBOT_CLOUD_AEC`、`MYBOT_WAKE_WORDS`、`MYBOT_SHOW_TRANSCRIPT`、
`MYBOT_ENABLE_HTTPS` 是主要项；产品用不到的功能请关闭。在 MCU 上 Agora RTSA 库通常占掉
大部分 Flash，且必须使用目标架构的包（随附归档仅限 x86_64 Linux）。

## 内存模型

- **静态逐帧音频缓冲**（位于应用状态内）：每个工作线程一份 16 kHz 单声道 16 位帧——采集、
  待播放、上行发送与 AEC 参考各约 1.9 KB（60 ms ptime 即 960 样本）；AEC 交织缓冲约 3.8 KB。
- **环形缓冲**：每个容纳 2 秒音频 = 64 KB。采集与播放恒有；开启 `MYBOT_CLOUD_AEC=ON` 时
  额外创建 AEC 参考环形缓冲，合计 192 KB。
- **堆**：HTTP 响应初始分配 4 KB、单请求最大增长到 32 KB（用后即释放）；JSON 解析与平台
  后端（ALSA、OpenSSL、文件 KV）存在临时分配。核心分配统一走 `aosl_hal_malloc`，各平台可
  重定向。
- 控制面状态（应用、设备生命周期、RTC 会话）均为静态分配；核心没有按会话的堆分配，除上述
  HTTP/JSON 临时缓冲外。

## 线程与栈

| MPQ 线程 | 职责 | 栈 |
| --- | --- | --- |
| `startup_mpq` | 串行化 Wi-Fi 事件处理与启动迁移 | 16 KB |
| `state_mpq` | 设备状态机、阻塞 HTTP 轮询 | 16 KB |
| `mybot_mpq` | 按 ptime 节奏上行发送音频 | 16 KB |
| `cap_mpq` | 麦克风采集 | 16 KB |
| `pb_mpq` | 播放与 AEC 参考 | 16 KB |
| `key_stdin_mpq`（仅 Linux 参考） | 标准输入按键事件 | 4 KB |

核心栈预算合计 5 × 16 KB = 80 KB。栈大小为编译期常量（`src/core/mybot_app.c` 中的
`MPQ_STACK_SIZE`），请在目标上实测后再调整。实时音频定时器位于独立 MPQ，单个阻塞后端不会
拖垮整条音频通路；状态机因 HTTP 轮询会阻塞而独占线程。Agora RTSA SDK 内部另有厂商管理的
线程。

## 时序与实时性

- 音频格式固定为 16 kHz、单声道、16 位有符号；ptime 为 20 / 40 / 60 ms（默认 60 ms，
  即每帧 960 样本 / 1920 字节）。
- 平台 `read` / `write` 必须让阻塞有界（Linux ALSA 后端用 50 ms 轮询超时），工作线程才能
  感知关闭并快速退出。
- 状态机每 100 ms tick 一次；设备服务轮询由服务端驱动，配对阶段最小 3 s，运行时默认 30 s。
- HTTP 请求总时限 5 s。

## 功耗管理

现状：SDK **没有待机 / 低功耗模式**。只要 `mybot_app_is_running()` 为真，工作线程与定时器
持续运行。省电手段属于集成者：

- **休眠**：进入低功耗前调用 `mybot_app_stop()`，唤醒后 `mybot_app_start()`；这会释放
  工作线程、音频设备、TLS 与 RTC 资源。
- **射频**：Wi-Fi 配网后端拥有射频，平台的低功耗策略在那里实现。
- **音频通路**：在音频后端门控 Codec/功放；媒体音量（软件增益）与设备音量（硬件钩子）
  提供音量控制面。
- **轮询**：间隔由服务端驱动（配对最小 3 s，运行时默认 30 s）；若空闲功耗重要，与服务端
  协商放宽间隔。

开启 `MYBOT_WAKE_WORDS=ON` 时，平台本地 ASR 后端运行在采集 MPQ 上，必须功耗感知（这也是
空闲时仅保持麦克风通路存活的天然位置）。

## 日志

- 日志来自 AOSL；用 `aosl_set_log_level()` 设置运行级别（debug 到 error）。Linux 参考打印
  到 stdout。
- 热路径（音频定时器）日志尽量少——每次调用都会做格式化。
- 绝不打设备 token。参考应用在 INFO 级打印配对码；生产构建应脱敏。

完整平台契约与验收清单见 [PORTING.md](PORTING.zh-CN.md)。
