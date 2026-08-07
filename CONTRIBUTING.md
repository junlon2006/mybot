# Contributing

Contributions are welcome for the 0.1 release-candidate series. Public APIs may still change.

> [English](CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md)

## Workflow

1. Discuss large API, platform, dependency, or protocol changes in an issue first.
2. Keep platform code outside `src/` and integrate it through public platform ops.
3. Never commit credentials, device tokens, customer data, or proprietary SDK packages.
4. Format self-maintained C sources with the repository `.clang-format`.
5. Build and test before opening a pull request:

       cmake -S . -B build -DCONFIG_PLATFORM=linux -DMYBOT_ENABLE_ASAN=ON
       cmake --build build -j
       ctest --test-dir build --output-on-failure

Platform contributions must complete the checklist in `docs/PORTING.md` and describe real-device
validation. Update public documentation and `CHANGELOG.md` for user-visible behavior.

Pull requests should explain the problem, design, compatibility impact, tests, and hardware used.
By contributing, you agree that your contribution is licensed under the repository `LICENSE`
unless the file clearly carries its own compatible license.
