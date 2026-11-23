# Contributing

Contributions are welcome. The public source API follows Semantic Versioning. Binary ABI
compatibility is not currently guaranteed, so SDK consumers and platform adapters must be rebuilt
against the selected headers. Extending a public structure is a source-compatible change only after
those components are rebuilt.

> [English](CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md)

## Workflow

1. Discuss large API, platform, dependency, or protocol changes in an issue first.
2. Keep platform code outside `src/` and integrate it through public platform ops. Maintain MCU
   adapters and firmware in their independent projects; this repository keeps the Linux reference.
3. Never commit credentials, device tokens, customer data, or unapproved proprietary SDK packages.
   Changes to the existing Agora RTSA package under `third_party/agora_rtsa_sdk` require license
   and redistribution review; see `THIRD_PARTY_NOTICES.md`.
4. Format first-party C and C++ files with clang-format 15 or newer using the repository
   `.clang-format`. Separate function definitions with one blank line, including around
   conditional blocks that group whole functions. Use braces for control statement bodies.
   Tag every file with its SPDX license header (`/* SPDX-License-Identifier: Apache-2.0 */`).
   The cJSON-derived `mybot_json` sources use `MIT`. After mechanical renames or bulk edits,
   run the following command before committing.

   ```sh
   find include src platforms/linux examples/linux tests -type f \
     \( -name '*.c' -o -name '*.cpp' -o -name '*.h' \) -exec clang-format -i {} +
   ```

5. Build and test before opening a pull request using the commands in
   [README: Development and verification](README.md#development-and-verification).

Platform contributions must complete the checklist in `docs/PORTING.md` and describe real-device
validation. Update public documentation and `CHANGELOG.md` for user-visible behavior.

Pull requests should explain the problem, design, compatibility impact, tests, and hardware used.
By contributing, you agree that your contribution is licensed under the repository `LICENSE`
unless the file clearly carries its own compatible license.

## Commit messages

Use [Conventional Commits](https://www.conventionalcommits.org/) so release notes and changelog
entries stay consistent:

    <type>[optional scope][!]: <subject>

    <optional body>

    <optional footer>

Types: `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`, `build`, `ci`, `chore`,
`revert`. Add `!` after the type or scope for breaking changes and describe the break in a
`BREAKING CHANGE:` footer. Keep the subject under 72 characters, in the imperative mood, lowercase
except identifiers.

The repository enforces this format with a local `commit-msg` hook and in CI. Install the hook once
per clone:

    ./scripts/setup-githooks.sh
