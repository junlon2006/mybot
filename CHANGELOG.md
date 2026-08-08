# Changelog

This project follows Semantic Versioning after 1.0. Before 1.0, public API and ABI may change
between release candidates.

## [Unreleased]

### Fixed

- Continue binding-status polling during active conversations and end RTC locally when the device
  becomes unbound or its credential is rejected.
- Harden the Linux file KV backend against symlink traversal and persist atomic replacements and
  deletions with file and directory `fsync`.
- Preserve runtime Wi-Fi disconnect/reconnect events, pause device-service traffic while offline,
  and end active RTC conversations locally without reinitializing services after reconnect.
- Percent-encode device IDs in URL path segments and reject control characters in dynamic HTTP
  header values and request targets.
- Reject conversation-start responses without a valid conversation ID before entering the active
  conversation state.
- Reject `MYBOT_BUILD_LINUX_PLATFORM=ON` with a non-Linux `CONFIG_PLATFORM` at configure time, and
  document the two independent platform-selection variables (`CONFIG_PLATFORM` selects the AOSL
  HAL port; `MYBOT_BUILD_LINUX_PLATFORM` builds the Linux reference backends).
- Fix a NULL dereference in `mybot_json_create_*_array()` when an allocation fails mid-array,
  replace unbounded `strcpy` in JSON printing with bounded copies, and stop calling side-effecting
  functions inside test `assert()` (found by cppcheck / clang-tidy).
- Release partially initialized services immediately when `start_services()` fails, instead of
  relying on a later `mybot_app_stop()` call; service teardown is shared, idempotent, and safe to
  run twice after a failed startup.

### Changed

- Unify public API naming around `mybot_<module>_register / init / deinit`: drop redundant middle
  words (`mybot_audio_register_capture()`, `mybot_audio_register_playback()`, `mybot_key_register()`,
  `mybot_wifi_register()`, `mybot_https_register()`), rename the HTTPS transport header to
  `mybot_https.h`, and keep the device-volume family (`mybot_audio_device_volume_*`) distinct from
  media volume.
- Unify result codes: add `mybot_errors.h` (0 success, positive payload, negative failure).
  `mybot_kv_store_get()` now returns `MYBOT_ERR_NOT_FOUND` instead of the positive
  `MYBOT_KV_STORE_NOT_FOUND`.

### Added

- HTTPS-by-default device-service transport with a platform TLS contract and Linux OpenSSL backend.
- Cross-platform porting guide, release checklist, security and contribution policies.
- CI coverage for Linux builds, tests, public headers, and external CMake host integration.
- Bilingual (English / Simplified Chinese) README with an AI-conversation product overview and
  layered architecture diagrams.
- Bilingual (English / Simplified Chinese) community docs: contributing, support, security, and
  code of conduct.
- Dedicated `mybot_ringbuf_test` unit test covering lifecycle, full/empty boundaries,
  wrap-around reads and writes, argument validation, and a single-producer/single-consumer
  concurrency run.
- Enforce Conventional Commits with a repository-local `commit-msg` hook
  (`githooks/commit-msg`), a one-command installer (`scripts/setup-githooks.sh`), a commit
  template (`.gitmessage`), and a CI step that validates pushed / PR commit subjects.
- CI matrix across GCC and Clang with ASan and UBSan, cppcheck and clang-tidy static analysis,
  gcov/lcov coverage collection with Codecov upload, and new `MYBOT_ENABLE_UBSAN` /
  `MYBOT_ENABLE_COVERAGE` CMake options.
- Add a `MYBOT_API` symbol-visibility macro (`mybot_export.h`) to every public header, preparing
  the SDK for shared-library and Windows DLL builds; the SDK target builds with
  `-fvisibility=hidden` and defines `MYBOT_BUILDING_LIBRARY` while compiling.
- Doxygen API reference generated from the public headers (`docs/Doxyfile.in`, version
  single-sourced through CMake, warnings treated as errors) and built as a CI artifact on every
  push / PR.
- Bilingual porting and release guides under `docs/` (PORTING / RELEASING).
- SPDX license identifiers on all self-maintained C sources (Apache-2.0; MIT for the cJSON-derived
  `mybot_json` sources).
- Media volume control: the SDK applies a 0..100 digital software gain to playback PCM, so media
  volume works on every platform without a backend.
- Real-device volume control: optional `mybot_audio_volume_ops_t` backend contract routed through
  `mybot_audio_device_set_volume()` / `mybot_audio_device_get_volume()`, with an ALSA mixer
  reference backend on Linux (Master / PCM / Digital controls).
- Volume-up / volume-down key events, mapped to `u` / `d` on the Linux example; each event steps
  the media volume by 10.
- Installable CMake package: `find_package(mybot)` with exported `mybot::sdk` / `mybot::aosl`
  targets, a package version file, bundled AOSL headers and library, and a pkg-config file. The
  Agora RTSA library is supplied by the consumer via `MYBOT_AGORA_SDK_DIR` /
  `MYBOT_AGORA_RTC_LIBRARY` and is covered by an install-and-consume integration test.

## [0.1.0-rc.1] - Unreleased

### Added

- Cross-platform SDK target and public platform ops for audio, Wi-Fi, KV, keys, LCD, and wake words.
- Linux reference backends and CLI example.
- Device pairing, lifecycle, Agora RTC audio, and optional local wake-word flow.
- Version API and validated CMake feature configuration.
- Unit, Linux platform, public-header, and external-host integration tests.

### Known limitations

- MCU ports must provide a certificate-validating TLS backend and CA trust store.
- API/ABI is not stable; the runtime uses singleton registries and process-global dependencies.
- Bundled Agora RTSA artifacts are x86_64 Linux only and require separate license verification.
