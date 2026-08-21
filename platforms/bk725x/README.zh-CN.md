# BK725x 平台移植

[English](README.md) | [简体中文](README.zh-CN.md)

本目录包含 `mybot` SDK 的 BK725x 平台实现，负责将 SDK 的公开平台接口适配到 Beken
BK725x SDK，供基于 BK Armino 的固件工程集成使用。

完整且可构建的 BK7258 固件参考工程维护在以下配套仓库：

<https://github.com/junlon2006/mybot-bk7258>

本 SDK 中同步的示例工程位于
[`examples/bk725x`](../../examples/bk725x/)。示例包含 AP/CP 启动代码、板级配置、分区表、
内嵌语音资源以及 BK 专用的应用控制器。

## 功能范围

该平台移植提供以下 BK725x 实现：

- Wi-Fi 配网、连接状态监控和凭据持久化；
- 基于 BK mbedTLS 集成的 HTTPS 传输；
- 16 kHz 单声道 PCM 音频采集、播放、设备音量和共享音频电源管理；
- OGG/Opus 提示音解码和配对码播报；
- LCD 渲染、语义按键事件和 BK 按键输入；
- 基于 BK 环境存储的键值存储。

`adapter/` 包含将这些实现连接到 SDK `ops` 接口的注册适配器。可复用的 BK 模块按功能
分布在 `modules/audio`、`modules/network`、`modules/display`、`modules/button`、
`modules/key` 和 `modules/storage` 目录中。

控制器、AP/CP 入口、SD 卡流程、USB MSC 集成、板级配置、分区表以及产品资源属于应用
集成层代码，保留在 `examples/bk725x` 中，不属于平台无关的 SDK 核心。

## 平台注册

在调用 `mybot_start()` 之前，调用一次平台注册入口：

```c
#include "bk725x_platform_adapters.h"

if (bk725x_platform_adapters_register() < 0) {
    /* 中止启动并报告平台初始化失败。 */
}
```

该注册函数会安装 BK HTTPS、KV、Wi-Fi、音频、播报、按键和 LCD 实现。`examples/bk725x`
中的应用控制器会在启动流程中完成该注册。

## BK 构建环境

该平台移植由 BK Armino/AVDK 构建系统编译，而不是由 SDK 默认的 Linux CMake 目标编译。
固件工程必须提供 BK725x 工具链以及以下平台组件：

```text
bk_rtos, bk_common, bk_uid, bk_wifi, bk_event, bk_netif,
bk_display, psa_mbedtls, lwip, easy_flash,
audio_pipeline, onboard_mic_stream, onboard_speaker_stream,
raw_stream, multimedia, json, bk_vfs, fatfs
```

同步到 `examples/bk725x` 的目录包含项目入口和板级文件。完整的组件源文件清单以及
`armino_component_register()` 配置仍位于 BK 固件仓库的
`bk_solution_ai/components/mybot/CMakeLists.txt`。由于主机环境没有 BK 头文件和库，SDK
默认的 Linux 构建和 CI 不会编译本目录。

## 内嵌提示音

BK 示例会将 `examples/bk725x/assets/locales/` 下的 OGG 提示音转换为 C 资源表，并放置在
BK 存储模块中。平台提示音和播报代码在播放时读取内嵌 OGG 字节，并将其解码到 PSRAM 中
的 PCM 缓冲区。压缩后的资源字节会成为 AP 固件镜像的一部分，因此会占用 Flash 空间。

新增或替换提示音时，使用
[`examples/bk725x/scripts`](../../examples/bk725x/scripts/) 中的转换和生成脚本，然后重新
构建 BK 固件。生成的资源源码属于具体产品数据，不应脱离 BK 产品配置单独加入 SDK 核心。

## 配置说明

- 当前 BK 示例默认关闭 `CONFIG_USBD_MSC`。USB MSC 和 SD 卡共享访问属于可选产品功能，
  不是 SDK 平台接口的必需项。
- BK 专用的内存分配、RTOS、网络和显示头文件均限制在本目录内。
- 应用代码应使用 SDK 公开 API（包括 `mybot_get_state()` 和语义化会话按键事件），不要
  直接使用 SDK 私有 lifecycle 接口。

完整的工程目录、板级配置、分区表、固件构建命令和设备验证流程，请参阅上方配套仓库。
