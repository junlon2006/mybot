# Contributing

Contributions are welcome for the 1.0 release series. Public APIs and ABI follow Semantic Versioning.

> [English](CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md)

## Workflow

1. Discuss large API, platform, dependency, or protocol changes in an issue first.
2. Keep platform code outside `src/` and integrate it through public platform ops.
3. Never commit credentials, device tokens, customer data, or proprietary SDK packages.
4. Format self-maintained C sources with the repository `.clang-format` and tag every file with an
   SPDX license header (`/* SPDX-License-Identifier: Apache-2.0 */`; the cJSON-derived `mybot_json`
   sources use `MIT`). After mechanical renames or bulk edits, run
   `find include src platforms/linux examples/linux tests -type f \( -name '*.c' -o -name '*.h'
   \) -exec clang-format -i {} +` before committing. BK725x Armino sources use their firmware
   toolchain's formatting rules and are not part of the host format check.
5. Build and test before opening a pull request (initialize the AOSL submodule first):

       git submodule update --init --recursive
       cmake -S . -B build -DCONFIG_PLATFORM=linux -DMYBOT_ENABLE_ASAN=ON
       cmake --build build -j
       ctest --test-dir build --output-on-failure

Platform contributions must complete the checklist in `docs/PORTING.md` and describe real-device
validation. Update public documentation and `CHANGELOG.md` for user-visible behavior.

Pull requests should explain the problem, design, compatibility impact, tests, and hardware used.
By contributing, you agree that your contribution is licensed under the repository `LICENSE`
unless the file clearly carries its own compatible license.

## Commit messages

Use [Conventional Commits](https://www.conventionalcommits.org/) so releases and the changelog can
be derived mechanically:

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
