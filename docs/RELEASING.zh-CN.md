# 发布检查清单

> [English](RELEASING.md) | 简体中文

## 准备

- [ ] 选择语义化预发布版本，并更新 `CMakeLists.txt` 顶部的版本变量——这是唯一来源。
      `mybot_version.h` 由 `mybot_version.h.in` 在配置阶段生成，请勿直接编辑；同步更新
      `CHANGELOG.md` 与示例输出。
- [ ] 确认 README 与 `docs/PORTING.md` 与实际公开 API 和 CMake 选项一致。
- [ ] 审查所有第三方变更与 `THIRD_PARTY_NOTICES.md`。
- [ ] 取得随附 Agora 制品可再分发的书面确认，或将其从发布制品中移除并说明用户如何自行
      提供。
- [ ] 确保源码制品包含 AOSL submodule 内容，或说明用户需执行
      `git submodule update --init --recursive`（GitHub 源码归档不包含 submodule）。
- [ ] 确认没有跟踪任何密钥、token、私有端点、客户数据或生成的 KV 数据。

## 验证

    cmake -S . -B build-release -DCONFIG_PLATFORM=linux -DMYBOT_ENABLE_ASAN=ON
    cmake --build build-release -j
    ctest --test-dir build-release --output-on-failure
    git diff --check

- [ ] 测试 20、40 与 60 ms 音频包长。
- [ ] 在真实硬件上运行配网、配对、双向音频、挂断、关闭与重启。
- [ ] 测试网络丢失、音频设备丢失、存储失败与部分启动失败。
- [ ] 确认日志与发布压缩包不含任何凭据。
- [ ] 在硬件上确认 HTTPS 证书链、主机名、SNI、超时与信任库行为。
- [ ] 确认所有发布配置中 `MYBOT_ALLOW_INSECURE_HTTP=OFF`。

## 发布

- [ ] 创建与 `MYBOT_VERSION_STRING` 匹配的带注释的预发布 tag。
- [ ] 仅在第三方授权审查后附加源码与二进制制品。
- [ ] 包含 `LICENSE`、`THIRD_PARTY_NOTICES.md`、changelog 与校验和。
- [ ] 将 GitHub release 标记为预发布并列出已知限制。
