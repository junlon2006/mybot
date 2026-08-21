# Changelog

This project follows Semantic Versioning.

## [Unreleased]

### Added

- Add the BK725x platform port under `platforms/bk725x` and synchronize the complete BK7258
  reference project under `examples/bk725x`, with the full firmware implementation maintained at
  <https://github.com/junlon2006/mybot-bk7258>.
- Add `MYBOT_STATE_IN_CONVERSATION = 7` to the public `mybot_state_t` enum. After the device service
  accepts a conversation, `mybot_get_state()` reports this state until normal teardown returns to
  `MYBOT_STATE_READY`; `MYBOT_STATE_WIFI_DISCONNECTED` takes precedence during runtime connectivity
  loss. Existing state values remain unchanged.

### Changed

- Limit host clang-format and clang-tidy checks to the SDK core, Linux platform, Linux example, and
  tests; BK725x Armino sources are validated by the BK firmware build environment instead.
- Update the AOSL lifecycle integration for the reference-counted `aosl_ctor()` / `aosl_dtor()`
  API: mybot and Agora RTC now hold independent runtime references, and mybot releases its
  application reference only after RTC callbacks, workers and buffers have been torn down.
- Promote application informational logs to the AOSL NOTICE level, set the Linux example's
  runtime log threshold accordingly, and initialize the Agora RTSA SDK at its default NOTICE
  threshold, keeping application lifecycle messages visible while suppressing lower-priority
  dependency logs.
- Simplify the Wi-Fi platform boundary to connectivity events: remove the redundant
  `mybot_wifi_state_t`, `mybot_wifi_state_handler_t`, and internal state query, and
  require connected events to represent usable IP networking.

### Fixed

- Clamp server-provided device-service polling intervals to 3..60 seconds and saturate oversized JSON
  integers before conversion, preventing timer overflow and request storms.
- Drop RTC downlink audio while a pairing announcement is active, preserving the playback ring
  buffer's single-producer/single-consumer access and giving announcements priority.
- Remove the HTTP response parser's POSIX `strncasecmp()` dependency by using
  an ASCII-only case-insensitive comparison, preserving mixed-case header support on non-POSIX platforms.

## [1.0.0] - 2026-08-10

### Added

- Pairing-code voice announcement: when a pair code is obtained the SDK plays
  the fixed prompt then each code digit as short local sounds, once per pair
  code, through the normal playback path. The platform supplies raw 16 kHz
  mono s16 PCM assets per locale (Linux reference: `./assets/locales/<locale>/prompt.pcm`
  and `0.pcm`..`9.pcm`); the SDK core contains no audio decoder.
- HTTPS-by-default device-service transport with a platform TLS contract and Linux OpenSSL implementation.
- Cross-platform porting guide, release checklist, security and contribution policies.
- CI coverage for Linux builds, tests, public headers, and external CMake host integration.
- Bilingual (English / Simplified Chinese) README with an AI-conversation product overview and
  layered architecture diagrams.
- Bilingual (English / Simplified Chinese) community docs: contributing and support.
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
- Deterministic fuzz loops for the HTTP response parser and JSON parser, allocator fault injection
  for the HTTP client (linker-wrapped `aosl_hal_malloc`/`realloc`), and an RTC session state-machine
  unit test that drives the wrapper through stubbed Agora SDK callbacks.
- Embedded integration notes (`docs/EMBEDDED.md` / `docs/EMBEDDED.zh-CN.md`) with measured
  footprint, memory model, thread/stack budget, timing, power and logging guidance for MCU
  integrators.
- Bilingual porting and release guides under `docs/` (PORTING / RELEASING).
- SPDX license identifiers on all self-maintained C sources (Apache-2.0; MIT for the cJSON-derived
  `mybot_json` sources).
- Media volume control: the SDK applies a 0..100 digital software gain to playback PCM, so media
  volume works on every platform without an implementation.
- Real-device volume control: optional `mybot_audio_volume_ops_t` implementation contract routed through
  `mybot_audio_device_set_volume()` / `mybot_audio_device_get_volume()`, with an ALSA mixer
  reference implementation on Linux (Master / PCM / Digital controls).
- Volume-up / volume-down key events, mapped to `u` / `d` on the Linux example; each event steps
  the media volume by 10.
- Installable CMake package: `find_package(mybot)` with exported `mybot::sdk` / `mybot::aosl`
  targets, a package version file, bundled AOSL headers and library, and a pkg-config file. The
  Agora RTSA library is supplied by the consumer via `MYBOT_AGORA_SDK_DIR` /
  `MYBOT_AGORA_RTC_LIBRARY` and is covered by an install-and-consume integration test.

### Fixed

- Continue binding-status polling during active conversations and end RTC locally when the device
  becomes unbound or its credential is rejected.
- Harden the Linux file KV implementation against symlink traversal and persist atomic replacements and
  deletions with file and directory `fsync`.
- Preserve runtime Wi-Fi disconnect/reconnect events, pause device-service traffic while offline,
  and end active RTC conversations locally without reinitializing services after reconnect.
- Percent-encode device IDs in URL path segments and reject control characters in dynamic HTTP
  header values and request targets.
- Reject conversation-start responses without a valid conversation ID before entering the active
  conversation state.
- Reject `MYBOT_BUILD_LINUX_PLATFORM=ON` with a non-Linux `CONFIG_PLATFORM` at configure time, and
  document the two independent platform-selection variables (`CONFIG_PLATFORM` selects the AOSL
  HAL port; `MYBOT_BUILD_LINUX_PLATFORM` builds the Linux reference implementations).
- Fix a NULL dereference in `mybot_json_create_*_array()` when an allocation fails mid-array,
  replace unbounded `strcpy` in JSON printing with bounded copies, and stop calling side-effecting
  functions inside test `assert()` (found by cppcheck / clang-tidy).
- Fix a heap buffer overflow in `mybot_json` string parsing: a malformed `\u` escape followed by a
  quote could swallow the closing quote and overflow the output buffer (found by the new
  deterministic JSON parser fuzz).
- Guard `mybot_rtc_session_join()` and `mybot_rtc_session_send_audio()` against calls before
  initialization, which previously dereferenced a NULL mutex (found by the new RTC session test).
- Release partially initialized services immediately when `start_services()` fails, instead of
  relying on a later `mybot_stop()` call; service teardown is shared, idempotent, and safe to
  run twice after a failed startup.

### Changed

- AOSL is now a git submodule at `third_party/aosl` tracking the latest upstream master commit
  `39c3fb7b` instead of a vendored copy; no local AOSL changes are carried.
- Volume control is now fully SDK-internal: when a device volume implementation is registered, volume
  changes drive real hardware volume and the playback pipeline skips the software gain; otherwise
  the SDK falls back to the media-volume software gain. The volume control functions moved from
  `include/mybot/platform/mybot_audio.h` to the internal `src/internal/mybot_audio_internal.h`.
- Reduce `include/mybot/platform/*.h` to the platform implementation contract only (ops tables, enums
  and the `mybot_*_register()` entry points, plus the audio volume scale constants): the SDK
  internal lifecycle/query functions (`init` / `deinit` / `is_registered`, LCD rendering,
  wake-word PCM feed, KV-store access, capture/playback accessors and volume control) moved to
  `src/internal/mybot_*_internal.h` and are no longer part of the public API surface.
- Move conversation-control requests (`mybot_app_start_conversation()`,
  `mybot_app_stop_conversation()`, `mybot_app_pair()`) out of the public header
  `include/mybot/mybot.h` into the internal `src/internal/mybot_app.h`: host applications now
  trigger them only through platform key / wake-word events, and the symbols are no longer
  exported from the library.
- Rename the public application entry points from the `mybot_app_*` prefix to the root
  `mybot_*` namespace (`mybot_start()`, `mybot_stop()`, `mybot_is_running()`,
  `mybot_get_state()`, `mybot_request_exit()`; types `mybot_config_t` / `mybot_state_t`; state
  enumerators `MYBOT_STATE_*`). `mybot_app_*` is now reserved for internal app-shell control in
  `src/`.
- Unify public API naming around `mybot_<module>_register / init / deinit`: drop redundant middle
  words (`mybot_audio_register_capture()`, `mybot_audio_register_playback()`, `mybot_key_register()`,
  `mybot_wifi_register()`, `mybot_https_register()`), rename the HTTPS transport header to
  `mybot_https.h`, and keep the device-volume family (`mybot_audio_device_volume_*`) distinct from
  media volume.
- Unify result codes: add `mybot_errors.h` (0 success, positive payload, negative failure).
  `mybot_kv_store_get()` now returns `MYBOT_ERR_NOT_FOUND` instead of the positive
  `MYBOT_KV_STORE_NOT_FOUND`.
- `mybot_json` now defaults to the AOSL HAL allocator (`aosl_hal_malloc` / `aosl_hal_free`)
  instead of libc `malloc` / `free`, so JSON follows platform allocator redirects like the rest of
  the SDK; `mybot_json_init_hooks(NULL)` resets to the same defaults.
- The `.clang-format` language standard is `Auto` (inferred per file) instead of the misleading
  `Cpp03`, and the installed `mybot.pc` is relocatable: its prefix is derived from the pkg-config
  directory at any `CMAKE_INSTALL_LIBDIR` depth rather than baked in at configure time.

## [0.1.0-rc.1]

### Added

- Cross-platform SDK target and public platform ops for audio, Wi-Fi, KV, keys, LCD, and wake words.
- Linux reference implementations and CLI example.
- Device pairing, lifecycle, Agora RTC audio, and optional local wake-word flow.
- Version API and validated CMake feature configuration.
- Unit, Linux platform, public-header, and external-host integration tests.

### Known limitations

- MCU ports must provide a certificate-validating TLS implementation and CA trust store.
- API/ABI is not stable; the runtime uses singleton registries and process-global dependencies.
- Bundled Agora RTSA artifacts are x86_64 Linux only and require separate license verification.
