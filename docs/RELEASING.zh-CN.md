# 发布检查清单

> [English](RELEASING.md) | 简体中文

## 准备

- [ ] 确定语义化版本号，并更新 `CMakeLists.txt` 顶部的版本变量——这是唯一来源。
      `mybot_version.h` 由 `mybot_version.h.in` 在配置阶段生成，请勿直接编辑；同步更新
      `CHANGELOG.md` 与示例输出。
- [ ] 确认 README 与 `docs/PORTING.md` 与实际公开 API 和 CMake 选项一致。
- [ ] 审查所有第三方变更与 `THIRD_PARTY_NOTICES.md`。
- [ ] 发布制品若包含内置 Agora RTSA 二进制，需先通过声网（Agora）销售/商务渠道取得书面
      再分发授权；仓库内 x86_64 Linux 二进制保留用于 Linux 演示。若未取得授权，请从发布
      制品中排除该二进制，并说明用户如何通过 `AGORA_SDK_DIR` / `AGORA_RTC_LIBRARY` 自行提供。
- [ ] 在发布源码制品中展开固定版本的 AOSL 源码。GitHub 自动生成的 **Source code** 归档
      不包含 submodule，也没有 `.git` 元数据，无法通过 `git submodule update` 补齐。
      用户应下载发布页的源码制品，或执行
      `git clone --recurse-submodules https://github.com/junlon2006/mybot.git`。
- [ ] 确认没有跟踪任何密钥、token、私有端点、客户数据或生成的 KV 数据。

## 验证

    cmake -S . -B build-release -DCONFIG_PLATFORM=linux -DMYBOT_ENABLE_ASAN=ON
    cmake --build build-release -j
    ctest --test-dir build-release --output-on-failure
    git diff --check

- [ ] 使用随附 RTSA 软件包测试 60 ms；20/40 ms 仅在提供匹配 RTSA 软件包时测试。
- [ ] 使用兼容服务验证 RTM 登录、会话 channel 订阅以及
      `VP_REGISTER_SUCCESS` LCD 指示器。
- [ ] BK 设备验证使用独立的 [BK7258](https://github.com/junlon2006/mybot-bk7258) 或
      [BK7259](https://github.com/junlon2006/mybot-bk7259) 固件工程，验证声纹注册中/成功标记，
      以及 MyBot 启动前完整播报配网成功提示音。
- [ ] 在真实硬件上运行配网、配对、双向音频、挂断、关闭与重启。
- [ ] 测试网络丢失、音频设备丢失、存储失败与部分启动失败。
- [ ] 确认日志与发布压缩包不含任何凭据。
- [ ] 在硬件上确认 HTTPS 证书链、主机名、SNI、超时与信任库行为。
- [ ] 确认所有发布配置中 `MYBOT_ALLOW_INSECURE_HTTP=OFF`。

## 打包已标记版本

在 Git clone 中使用 [scripts/release.sh](../scripts/release.sh)，依赖 Bash、Git、GNU tar、
gzip 和 sha256sum。先获取发布 tag 与最新的 `origin/main`，并初始化 AOSL：

```sh
git fetch origin main --tags
git submodule update --init --recursive
./scripts/release.sh package v1.2.0
```

浅克隆需先执行 `git fetch --unshallow origin` 获取完整历史。脚本要求显式指定带注释 tag，
版本与该 tag 中的 `CMakeLists.txt` 一致，且 tag 提交位于 `origin/main` 历史中。
打包内容来自 tag 及其中 gitlink 固定的 AOSL 提交；本地改动、当前分支和当前检出的 AOSL
版本不会进入制品。若本地缺少指定的 AOSL 提交，需手动执行
`git -C third_party/aosl fetch origin <aosl-commit>`；脚本不会自动 fetch 或切换工作区版本。

`package TAG [OUTPUT_DIR]` 默认输出到 `build-release/TAG`，生成：

- `mybot-VERSION-source.tar.gz`：根目录为 `mybot-VERSION/`，包含固定版本的 AOSL 源码、
  许可证、第三方声明与 changelog。
- `mybot-VERSION-SHA256SUMS`：源码归档的 SHA-256 校验和。

源码包排除 RTSA 预编译二进制，保留其头文件和构建元数据。通过 `AGORA_RTC_LIBRARY` 提供
匹配版本的 RTSA 库；更换 RTSA 软件包或目标平台时，还需通过 `AGORA_SDK_DIR` 指向匹配的
头文件和元数据。脚本对归档路径排序，将 UID/GID 设为 0、时间设为 tag 提交时间，并去掉
gzip 时间戳，使同一 tag 的重复打包结果一致。

## 发布

- [ ] 确认现有 `v1.0.0` tag 仍指向历史 1.0.0 发布提交；绝不移动或覆盖已有发布 tag。
- [ ] 在 `main` 分支创建与 `MYBOT_VERSION_STRING` 匹配的带注释发布 tag（例如 `v1.2.0`）。
- [ ] 推送 `main` 上的发布提交和对应带注释 tag，再按上面的步骤打包该 tag。
- [ ] 仅在第三方授权审查后附加源码与二进制制品。
- [ ] 附加生成的源码归档和校验和文件，并保留本地文件以便核验。
- [ ] 将 GitHub release 发布为正式版（不标记为预发布）并列出已知限制。
- [ ] 安装 GitHub CLI（`gh`）后核验已发布的资产：

      `./scripts/release.sh verify v1.2.0`

`verify TAG [OUTPUT_DIR]` 与 `package` 使用相同的默认输出目录。脚本下载 GitHub Release
中对应名称的两个资产，将校验和文件及实际归档哈希与本地文件比较；允许发布页包含其他资产，
但校验和文件必须仅含源码归档这一项。验证采用此校验格式的旧手工打包版本时，应使用当时
保留的原始本地文件；重新打包可能因归档元数据不同而产生不同哈希。
两个命令都不会创建 tag、推送提交、上传资产或发布 Release。
