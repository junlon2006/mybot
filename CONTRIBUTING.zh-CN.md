# 参与贡献

欢迎对 0.1 release-candidate 系列贡献代码。公开 API 仍可能发生变化。

> [English](CONTRIBUTING.md) | 简体中文

## 工作流程

1. 涉及 API、平台、依赖或协议的大型改动，请先在 issue 中讨论。
2. 平台代码保持在 `src/` 之外，并通过公共平台 ops 集成。
3. 绝不提交凭据、设备 token、客户数据或专有 SDK 包。
4. 自维护的 C 源码遵循仓库根目录 `.clang-format`，并为每个文件标注 SPDX 许可证头
   （`/* SPDX-License-Identifier: Apache-2.0 */`；cJSON 派生的 `mybot_json` 源码使用 `MIT`）。
   机械重命名或批量编辑后，提交前先执行
   `find include src platforms examples tests -type f \( -name '*.c' -o -name '*.h' \) -exec
   clang-format -i {} +`。
5. 提交 pull request 前先构建并测试（先初始化 AOSL submodule）：

       git submodule update --init --recursive
       cmake -S . -B build -DCONFIG_PLATFORM=linux -DMYBOT_ENABLE_ASAN=ON
       cmake --build build -j
       ctest --test-dir build --output-on-failure

平台贡献必须通过 `docs/PORTING.md` 中的验收清单，并描述真实设备验证情况。涉及
用户可见行为时，请同步更新公共文档与 `CHANGELOG.md`。

Pull request 应说明问题、设计、兼容性影响、测试与使用的硬件。通过提交，你同意你的
贡献按仓库 `LICENSE` 许可发布，除非该文件明确带有自身兼容的许可证。

## 提交信息

使用 [Conventional Commits](https://www.conventionalcommits.org/) 以便发布与变更日志可以机械生成：

    <type>[optional scope][!]: <subject>

    <optional body>

    <optional footer>

类型：`feat`、`fix`、`docs`、`style`、`refactor`、`perf`、`test`、`build`、`ci`、`chore`、
`revert`。破坏性变更在类型或 scope 后加 `!`，并在正文用 `BREAKING CHANGE:` footer 说明。
主题行不超过 72 个字符、使用祈使句、除标识符外小写。

仓库通过本地 `commit-msg` hook 与 CI 强制执行该格式。每个克隆安装一次 hook：

    ./scripts/setup-githooks.sh
