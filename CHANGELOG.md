# Changelog

This project follows Semantic Versioning.

## [Unreleased]

### Added

- Expand deterministic unit coverage for JSON allocation failures, HTTP and device-service
  protocol boundaries, device lifecycle recovery, RTC errors, and application cleanup.
- Add one-shot `mybot_platform_descriptor_t` registration with complete validation and
  synchronous startup checks for configuration-required ops.
- Add the BK725x platform port under `platforms/bk725x` and synchronize the complete BK7258
  reference project under `examples/bk725x`, with the full firmware implementation maintained at
  <https://github.com/junlon2006/mybot-bk7258>.
- Add `MYBOT_STATE_IN_CONVERSATION = 7` to the public `mybot_state_t` enum. After the device service
  accepts a conversation, `mybot_get_state()` reports this state until normal teardown returns to
  `MYBOT_STATE_READY`; `MYBOT_STATE_WIFI_DISCONNECTED` takes precedence during runtime connectivity
  loss. Existing state values remain unchanged.

### Changed

- Sync the complete BK725x platform adaptation with the BK7258 reference project, including
  descriptor-based registration and embedded TLS root verification.
- Restrict the SDK-internal LCD facade to the application control owner and remove its redundant
  render mutex.
- **Breaking:** remove `mybot_request_exit()`; host applications stop directly after their own exit
  signal or condition is observed.
- Align the Agora RTSA wrapper with the single-instance, one-call-at-a-time product model: initialize
  RTSA once per mybot run, use one AOSL CAS lifecycle gate for callback, audio-send, and teardown
  ordering, and create one connection per conversation, with explicit lifecycle and error logging.
- **Breaking:** remove legacy per-capability registration; the descriptor's version, capability, and
  name fields; and the name field from every ops table. Platform integrations must submit one complete
  descriptor whose non-NULL ops pointers are the sole declarations of supported platform functions.
- **Breaking:** narrow the public error-code header to the only SDK-consumed `MYBOT_ERR_NOT_FOUND`
  result; other APIs continue to use `0` for success and negative values for failure.
- Remove the internal conversation forwarding layer; application orchestration now calls the
  process-wide Agora RTC module directly without copying RTC credentials into a second context.
- Consolidate application control into one `control_mpq` owner for state, device lifecycle, RTC
  control, UI and volume actions, and resource startup and shutdown. Control callbacks only publish
  short events or atomic mailboxes, while PCM remains on the direct real-time data path.
- Remove the unused platform-registry startup lock and the unconsumed `MYBOT_SHOW_TRANSCRIPT`
  configuration surface.
- Trim internal dead surface: unused lifecycle/RTC response fields, empty RTC stats callback,
  duplicate platform/audio/wake-word accessors, redundant state-model CAS retries, and unused HTTP
  and JSON convenience APIs.
- Align the English and Chinese README lifecycle and architecture guidance, document the descriptor
  registration contract in the public headers, match local format commands to CI scope, and refresh
  stale BK725x ownership and generator comments.
- Refactor application, lifecycle, RTC, audio, storage, connectivity, input, display, announcement,
  and wake-word runtime state into caller-owned internal contexts. Public APIs retain the default
  process-wide compatibility facade, while active implementation state no longer lives in module
  globals.
- Split `mybot_app.c` orchestration into dedicated media-pipeline, Agora RTC, and LCD presenter
  modules without changing the public API.
- Derive public application state and LCD presentation from one atomic state-model snapshot fed by
  runtime-phase, connectivity, and device-lifecycle events.
- Limit host clang-format and clang-tidy checks to the SDK core, Linux platform, Linux example, and
  tests; BK725x Armino sources are validated by the BK firmware build environment instead.
- Update the pinned AOSL submodule to upstream commit `84e0860`, which adds reference-counted
  `aosl_ctor()` / `aosl_dtor()` lifecycle management. mybot and Agora RTC now hold independent
  runtime references, and mybot releases its application reference only after RTC callbacks,
  workers and buffers have been torn down.
- Promote application informational logs to the AOSL NOTICE level, set the Linux example's
  runtime log threshold accordingly, and initialize the Agora RTSA SDK at its default NOTICE
  threshold, keeping application lifecycle messages visible while suppressing lower-priority
  dependency logs.
- Simplify the Wi-Fi platform boundary to connectivity events: remove the redundant
  `mybot_wifi_state_t`, `mybot_wifi_state_handler_t`, and internal state query, and
  require connected events to represent usable IP networking.

### Fixed

- Keep asynchronous exit notifications side-effect free while the control owner destroys the media
  pipeline.
- Use a unique temporary directory in the Linux announcement test so stale files or PID reuse do not
  make repeated CI runs fail during setup.
- Make the application shutdown test wait for the playback worker to apply a pending announcement
  buffer clear before using a downlink frame to block playback I/O.
- Install the project `LICENSE` and `THIRD_PARTY_NOTICES.md` with the CMake package alongside the
  bundled AOSL license, and verify all three documents in the install-consumer integration test.
- Call both platform audio `stop` hooks before waiting for media workers, allowing a thread-safe
  stop implementation to unblock in-flight capture and playback I/O without shutdown deadlocks.
- Simplify device-service URL scheme validation into direct branches, eliminating a duplicate-condition
  cppcheck warning across HTTPS and development HTTP build configurations.
- Clamp server-provided device-service polling intervals to 3..60 seconds and saturate oversized JSON
  integers before conversion, preventing timer overflow and request storms.
- Drop RTC downlink audio while a pairing announcement is active, preserving the playback ring
  buffer's single-producer/single-consumer access and giving announcements priority.
- Remove the HTTP response parser's POSIX `strncasecmp()` dependency by using
  an ASCII-only case-insensitive comparison, preserving mixed-case header support on non-POSIX platforms.
- Serialize Agora RTC callbacks, audio sends, and connection teardown, reject stale connection IDs,
  and cover concurrent teardown ordering.
- Destroy a failed Agora RTC connection without reinitializing the process-wide RTSA service.
- Serialize application start and stop across threads so runtime initialization, failure cleanup, and
  teardown cannot concurrently mutate the process-wide runtime.
- Correct the public key-input contract to document the forwarded `user_data` context and its
  callback lifetime.

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
- Remove the conversation-control helper symbols and internal `mybot_app.h`; platform key and
  wake-word callbacks now submit short commands directly to the application control owner.
- Rename the public application entry points from the `mybot_app_*` prefix to the root
  `mybot_*` namespace (`mybot_start()`, `mybot_stop()`, `mybot_is_running()`,
  `mybot_get_state()`; types `mybot_config_t` / `mybot_state_t`; state
  enumerators `MYBOT_STATE_*`).
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
