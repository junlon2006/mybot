# 参与贡献

欢迎贡献代码。公开源码 API 遵循语义化版本。当前不保证二进制 ABI 兼容，因此 SDK 使用方与
平台适配层必须基于选定头文件重新构建。公开结构体增加字段只有在相关组件重新构建后才属于
源码兼容变更。

> [English](CONTRIBUTING.md) | 简体中文

## 工作流程

1. 涉及 API、平台、依赖或协议的大型改动，请先在 issue 中讨论。
2. 平台代码保持在 `src/` 之外，并通过公共平台 ops 集成。MCU 适配与固件在独立工程中维护；
   本仓库保留 Linux 参考实现。
3. 绝不提交凭据、设备 token、客户数据或未经批准的专有 SDK 包。修改
   `third_party/agora_rtsa_sdk` 中已有的 Agora RTSA 软件包必须经过许可证与再分发审查；
   参见 `THIRD_PARTY_NOTICES.md`。
4. 使用 clang-format 15 或更高版本，按仓库根目录的 `.clang-format` 格式化自维护的
   C 和 C++ 文件。函数定义之间留一个空行，整组函数的条件编译块前后也留空行。控制语句的
   语句体使用花括号。每个文件标注 SPDX 许可证头（`/* SPDX-License-Identifier: Apache-2.0 */`）。
   cJSON 派生的 `mybot_json` 源码使用 `MIT`。机械重命名或批量编辑后，提交前先执行以下命令。

   ```sh
   find include src platforms/linux examples/linux tests -type f \
     \( -name '*.c' -o -name '*.cpp' -o -name '*.h' \) -exec clang-format -i {} +
   ```

5. 提交 pull request 前按
   [README：开发与验证](README.zh-CN.md#开发与验证)中的命令构建并测试。

平台贡献必须通过 `docs/PORTING.md` 中的验收清单，并描述真实设备验证情况。涉及
用户可见行为时，请同步更新公共文档与 `CHANGELOG.md`。

Pull request 应说明问题、设计、兼容性影响、测试与使用的硬件。通过提交，你同意你的
贡献按仓库 `LICENSE` 许可发布，除非该文件明确带有自身兼容的许可证。

## 提交信息

使用 [Conventional Commits](https://www.conventionalcommits.org/) 以保持发布说明和变更日志一致：

    <type>[optional scope][!]: <subject>

    <optional body>

    <optional footer>

类型：`feat`、`fix`、`docs`、`style`、`refactor`、`perf`、`test`、`build`、`ci`、`chore`、
`revert`。破坏性变更在类型或 scope 后加 `!`，并在正文用 `BREAKING CHANGE:` footer 说明。
主题行不超过 72 个字符、使用祈使句、除标识符外小写。

仓库通过本地 `commit-msg` hook 与 CI 强制执行该格式。每个克隆安装一次 hook：

    ./scripts/setup-githooks.sh
