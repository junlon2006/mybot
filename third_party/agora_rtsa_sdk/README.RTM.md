# Agora RTSA-Lite SDK 介绍 (RTM 功能)

*简体中文| [English](README.RTM.en.md)*

声网实时码流加速（Real-time Streaming Acceleration, RTSA）SDK，依托声网自建的底层实时传输网络 Agora SD-RTN™ (Software Defined Real-time Network)，为所有支持网络功能的 Linux/RTOS 设备提供音视频码流在互联网实时传输的能力。与此同时，SDK 整合了Agora 实时消息（Real-time Messaging，RTM）SDK 部分功能，提供了稳定可靠、低延时、高并发的全球消息云服务，帮助你快速构建实时场景。

# 五分钟快速入门

我们提供了一个简单的示例项目，帮助开发者轻松掌握RTM API的使用，从而更高效的集成到自己的应用程序中。该示例项目 hello_rtm.c 位于 example/hello_rtm/ 目录，编译后可以演示实时消息收发的功能。

### 1. 创建 Agora 账号并获取 App ID

在编译和运行示例项目前，你首先需要通过以下步骤获取 Agora App ID:
1. 创建一个有效的 [Agora 账号](https://console.agora.io/)。
2. 登录 [Agora 控制台](https://console.agora.io/)，点击左侧导航栏项目管理按钮进入项目管理页面。
3. 在项目管理页面，点击创建按钮。在弹出的对话框内输入项目名称，选择鉴权机制为 App ID。点击提交，新建的项目就会显示在项目管理页中。Agora 会给每个项目自动分配一个 App ID 作为项目唯一标识。复制并保存此项目的 **App ID** ，稍后运行示例项目时会用到 App ID。

### 2. 编译 hello_rtm

在Linux x86环境下（比如Ubuntu 18.04），通过以下命令编译 hello_rtm。
```
$ cd example
$ ./build-x86_84.sh
```
编译完成后，会在当前目录里创建out/x86_64/目录，并在该目录下生成 hello_rtm 可执行文件。

### 3. 运行 hello_rtm

#### 参数

* **-h, --help ：** 打印帮助信息。
* **-i, --appId ：** 用于指定用户的 appId。无默认值，必填。
* **-t, --token ：** 用于指定用户的 RTM token。默认值为空字符串。RTM Token 的生成方式参考[生成 RTM Token](https://docs.agora.io/cn/Real-time-Messaging/token_server_rtm)。*注意：用于登录云信令系统的 `agora_rtc_login_rtm` 方法使用 RTM Token。用于加入 RTC 频道的  `agora_rtc_join_channel` 方法使用 RTC Token (参考[生成 Token](https://docs.agora.io/cn/Interactive%20Broadcast/token_server))。两种 Token 的生成方式不同且不可混用。*
* **-l, --sdkLogDir ：** 用于指定存放SDK Log的位置。 参数为目录，默认值为当前目录。
* **-p, --peerUid ：** 用于指定接收消息端用户ID。默认值为 **peer**。
* **-R, --rtmUid ：** 用于指定发送消息端用户ID。 默认值为 **user**。
* **-o, --role ：** 用于指定运行模式，演示时选择3即可。默认值为3。

#### 示例

可以打开两个终端，分别在两个终端输入如下命令行，通过从终端输入消息，按Enter键发送给对方，实现两个终端之间互发消息。
```
$ cd out/x86_64
$ ./hello_rtm --appId YOUR_APPID --rtmUid user1 --peerUid user2
$ ./hello_rtm --appId YOUR_APPID --rtmUid user2 --peerUid user1
```

请注意：
- 参数 `YOUR_APPID` 需要替换为你创建的 App ID。
- 消息只能发送给指定用户(peerUid)
- 如果在创建项目时，鉴权机制未设置为 App ID，而是设置为 Token，运行时将出现 `event code[110].` 错误提示，请重新创建项目选择 App ID 鉴权，或者参考官网文档生成临时 Token 并使用 `-t YOUR_TOKEN`设置 Token 参数，再次尝试运行。
- 再次强调，RTM的token与传输音视频流所用的token不同，如何生成toke请可参考官网链接[生成 RTM Token](https://docs.agora.io/cn/Real-time-Messaging/token_server_rtm)

# 移植

为了能让示例项目（hello_rtm）运行在嵌入式设备端（通常是ARM Linux系统），请参考 [移植指南](./docs/PORTING.md)，hello_rtm 和 hello_rtsa 移植过程相同。

# 关于 License
为了让你可以流畅体验我们的示例项目，并快速开始集成和测试你自己的项目，我们的 SDK 提供了一定时长的免费试用期。免费期到期之后，则无法继续使用 RTSA SDK，示例项目（hello_rtsa）也将无法运行！

所以，在免费期到期或正式上线之前，请务必联系声网销售(iot@agora.io)，购买商用的License，并且在项目中集成 License 的激活机制。详细流程请参考 [License集成指南]()

# 联系我们

- 如果发现了示例代码的 bug，欢迎[提交工单](https://agora-ticket.agora.io/)

