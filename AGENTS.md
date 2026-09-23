# MyBot agent guide

This file guides AI coding assistants working in the MyBot SDK repository. Follow the user's
current task and any more specific instructions in the directory being changed. Keep work scoped
to this repository unless the user includes another repository in the task.

## Start here

1. Run `git status --short --branch` and inspect the relevant diff. Preserve existing user work.
2. Read the relevant public headers and their implementation before proposing a change.
3. Use [README.md](README.md) for orientation, [PORTING.md](docs/PORTING.md) for platform contracts,
   and [EMBEDDED.md](docs/EMBEDDED.md) for resource and threading guidance. Chinese versions are
   available alongside them.
4. Check [CMakeLists.txt](CMakeLists.txt), [tests/CMakeLists.txt](tests/CMakeLists.txt), and
   [.github/workflows/ci.yml](.github/workflows/ci.yml) for current options and validation commands.
5. Distinguish observed behavior from intended contracts. If source, tests, and documentation
   disagree, describe the conflict; do not invent an API behavior to reconcile them.

## Project and source map

MyBot is a C99 SDK for one device and one active AI conversation per process. Agora RTSA provides
RTC/RTM, AOSL provides the portable runtime, and platform operations supply device capabilities.
Audio is bidirectional. Optional JPEG/H.264/H.265 video is sent from the device to the server only;
the platform performs capture and encoding.

| Location | Responsibility |
| --- | --- |
| `include/mybot/mybot.h` | Public application lifecycle and state |
| `include/mybot/platform/` | Platform operation tables and registration contract |
| `src/core/mybot_app.c` | Application orchestration and control worker |
| `src/core/mybot_device_lifecycle.c` | Pairing, authentication, conversation lifecycle |
| `src/core/mybot_state_model.c`, `mybot_presenter.c` | Public state projection and LCD content |
| `src/rtc/mybot_agora_rtc.c` | RTSA lifecycle, RTC/RTM callbacks, media send |
| `src/media/` | Audio pipeline, wake words, platform video bridge |
| `src/platform/` | Platform-neutral wrappers around registered operations |
| `src/service/`, `src/support/` | Device API, HTTP/JSON parsing, SPSC ring buffer |
| `src/internal/` | Private SDK contracts; not platform include surfaces |
| `platforms/linux/`, `examples/linux/` | Linux adapters and example application |
| `tests/unit/`, `tests/platform/`, `tests/integration/` | Unit, adapter, public-header and CMake checks |
| `third_party/` | Pinned AOSL submodule and target-specific Agora RTSA package |

## Design and evidence rules

- Prefer the smallest change satisfying the current requirement. Keep one owner per resource;
  avoid generic providers, duplicate state machines, speculative fallbacks, and multi-instance
  machinery. Agora RTSA is the RTC implementation; do not add an unrelated RTC abstraction.
- Derive every behavioral conclusion from inspected source, tests, or package metadata. For RTSA
  and AOSL, verify return values, synchronous versus asynchronous execution, buffer ownership,
  callback threads, and teardown behavior in implementation source. A prototype or demo alone is
  insufficient evidence.
- The RTSA implementation is maintained separately under `iot-paas-package/rtc/iot-paas-sdk/rtsa/`.
  Ask for its location if unavailable and needed. Check `rtc/rtc_service.c`, the relevant service
  or logging implementation, and `include/api/agora_rtc_api.h`. Compare against the selected
  package's headers and build metadata; do not assume a newer source checkout matches every binary.
- Keep OS and device APIs out of the core. Use AOSL for runtime services and the public platform
  operations for hardware. Identify a concrete AOSL limitation before proposing an OS-specific
  dependency.
- Platform code must not include `src/internal/` or call Agora APIs directly. Keep platform types
  out of SDK public contracts. Use `mybot_audio.h` as the style reference for platform comments:
  module overview, operation description, `@param`, `@return`, and ownership/threading notes.
- Use C99, the repository `.clang-format`, and the existing SPDX license headers. Preserve MIT
  notices in the cJSON-derived JSON implementation and all third-party notices.
- Validate lengths, integer conversions, PCM/frame boundaries, and service fields before use.
  Reject malformed or oversized input instead of silently truncating it.
- Update related English and Chinese documentation for changed contracts, and record user-visible
  behavior changes under `[Unreleased]` in [CHANGELOG.md](CHANGELOG.md).

## Runtime invariants

- Register one complete `mybot_platform_descriptor_t` before starting the SDK. Registration is
  process-wide; referenced operations tables must remain valid for the process lifetime.
- The product/platform owns provisioning and network credentials. The SDK consumes connectivity
  events and owns device-service pairing and conversations. For independent provisioning tasks,
  stop MyBot before entering provisioning and start it after usable network connectivity returns.
  Do not implement a second SDK lifecycle state machine in the platform.
- `control_mpq` owns application lifecycle/control work. `rtc_mpq` serializes RTSA state and calls;
  vendor callbacks copy borrowed payloads before asynchronous dispatch. Never queue a pointer to
  a stack object or expired callback buffer. Check the actual MPQ API for copy and wait semantics.
- Keep platform/RTSA callbacks short. Do not call `mybot_stop()` from a callback that shutdown must
  drain. Avoid RTC re-entry from application callbacks running on `rtc_mpq`.
- Audio capture, playback, and send use separate workers. Keep slow HTTP, parsing, and verbose
  logging out of their hot paths. Use event wakeups for queues without FD monitoring; do not apply
  `AOSL_MPQ_FLAG_SIGP_EVENT` to an FD-monitoring queue such as Linux stdin.
- Preserve the ring buffer's single-producer/single-consumer ownership. `clear()` and destruction
  require quiescent access; discard live data through the consumer. Keep session boundaries from
  sending or playing audio belonging to the previous conversation.
- Local prompts take priority over RTC playback. A new prompt replaces the unfinished prompt;
  discard RTC downlink during prompt playback so playback resumes with live audio. Prompt PCM must
  not enter the cloud AEC reference. Commit reference samples only after playback accepts them,
  including short writes.
- Audio ops count PCM frames, not bytes: 16 kHz, mono, signed 16-bit. The selected 20/40/60 ms ptime
  must match the RTSA package's `CONFIG_MINIMAL_TIMER_INTERVAL_MS`; bundled Linux RTSA uses 60 ms.
- Video bitrate limits come from platform ops `min_bps`/`max_bps`. The SDK sets RTSA BWE only when
  video is enabled, using the range midpoint as the initial value. Preserve
  `on_target_bitrate_changed` and optional `on_key_frame_request`; do not add SDK video encoding
  or an unneeded frame queue. Respect the borrowed-frame lifetime in `mybot_video.h`.
- RTM login and subscription to the conversation channel precede RTC join. Keep voiceprint and
  server-state parsing in the SDK, with the platform rendering LCD indicators.
- Stop device I/O before waiting for workers; destroy resources only when their owners and
  callbacks have stopped. Check queue, timer, allocation, vendor, and platform failures before
  releasing resources. Preserve AOSL ctor/dtor ownership across startup, failure, and teardown.
- Avoid new SDK stack locals larger than 1 KiB. Use bounded storage with explicit ownership and
  allocation-failure handling. Budget target stack/heap/queue usage; host measurements are not MCU
  limits. Avoid new per-frame heap allocations where preallocated storage suffices.
- `mybot_start()` raises AOSL logging to at least NOTICE while preserving INFO/DEBUG. RTSA uses
  `RTC_LOG_ERROR`; save and restore the AOSL level around RTSA initialization, including failure.
  Routine RTM message/forwarding logs use `AOSL_LOG_DBG`; keep failures visible and avoid credentials
  or unbounded payloads in logs.

## Build and validation

Host prerequisites: Linux x86_64 for the bundled RTSA library, CMake 3.16+, a C99 compiler,
ALSA/OpenSSL development packages, and initialized AOSL sources. Run from the repository root:

```sh
git submodule update --init --recursive
cmake -S . -B build -DCONFIG_PLATFORM=linux -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
git diff --check
```

Use distinct build directories for compiler/sanitizer/feature profiles. `CONFIG_PLATFORM` selects
the AOSL HAL; `MYBOT_BUILD_LINUX_PLATFORM` selects Linux adapters. Embedded SDK consumers normally
disable the Linux adapters and examples and supply matching `AGORA_SDK_DIR`/`AGORA_RTC_LIBRARY`.

For SDK video changes, test both OFF and ON. The Linux adapter has no video source; a host SDK test
configuration can enable video with the adapters and example disabled:

```sh
cmake -S . -B build-video -DCONFIG_PLATFORM=linux -DMYBOT_ENABLE_VIDEO=ON \
  -DMYBOT_BUILD_LINUX_PLATFORM=OFF -DMYBOT_BUILD_EXAMPLES=OFF -DMYBOT_BUILD_TESTS=ON
cmake --build build-video -j2
ctest --test-dir build-video --output-on-failure
```

- Use relevant existing tests first, then the required checks in the current CI workflow. Its
  matrix covers GCC/Clang ASan, GCC UBSan, AEC disabled, and wake words enabled. Do not claim a
  variant was tested unless it was configured and run. Test failure/ownership paths when changing
  lifecycle, memory, concurrency, or protocol handling.
- Keep assertions enabled in tests. Public-header and CMake host/install checks matter for public
  API or build changes; inspect their configuration because nested builds may use default features.
- Follow CI for cppcheck, clang-tidy, lcov, and Doxygen commands. Coverage excludes third-party,
  test, and platform code, with SDK thresholds of 80% lines and 65% branches. Inspect skipped
  compile commands and tool errors rather than reporting them as successful analysis.
- If LeakSanitizer cannot run under ptrace, record the failure. A diagnostic retry with
  `ASAN_OPTIONS=detect_leaks=0` checks address errors but does not verify leak detection.
- Documentation-only changes need link/content and whitespace checks; public-header comment
  changes also need Doxygen. Hardware validation is separate from stubbed host tests.

Formatting and API documentation commands:

```sh
find include src platforms/linux examples/linux tests -type f \
  \( -name '*.c' -o -name '*.h' \) -exec clang-format --dry-run --Werror {} +
doxygen build/docs/Doxyfile
```

## Repository and release workflow

- Commit, push, merge, or publish only within the user's requested scope; inspect existing
  authorization before asking again. Do not discard unrelated edits or rewrite published history.
- Follow [CONTRIBUTING.md](CONTRIBUTING.md): Conventional Commits, imperative subject of at most
  72 characters, and no trailing whitespace. Validate the actual subject with
  `./githooks/commit-msg --subject-only` via stdin, and check the staged diff before committing.
- `skills/` is ignored local guidance, not a product dependency. Do not add it to commits. Keep
  generated builds, runtime credentials, caches, and temporary release files out of source commits.
- Read [RELEASING.md](docs/RELEASING.md) for releases. Version variables in `CMakeLists.txt` are
  authoritative; `mybot_version.h` is generated. Merge the release changes into `main` before
  creating an annotated release tag there. Never move or overwrite an existing release tag.
- A pushed tag is not a GitHub Release with notes and downloadable assets. When publication is
  requested, complete and verify the requested release page and assets, not only the Git push.
- Source bundles must account for the AOSL submodule: a standalone tarball needs its pinned
  contents; `git submodule update` cannot populate a tarball without Git metadata. Include license
  notices and checksums. Follow third-party terms and the user's established distribution
  authorization; do not infer authorization solely from the presence of a bundled binary.

## External references and handoff

Reference firmware projects: [BK7258](https://github.com/junlon2006/mybot-bk7258),
[BK7259](https://github.com/junlon2006/mybot-bk7259), and
[ESP32](https://github.com/junlon2006/mybot-esp32). Inspect their actual revision and source when
needed. SDK work does not implicitly authorize edits or synchronization in those repositories.
Maintain MCU adapters and firmware in those independent projects; do not reintroduce copies into
this SDK repository. Keep the Linux reference here for development and host validation.

At handoff, state what changed and why, the checks actually run, and any remaining limitations.
Report commit IDs or release URLs only for actions completed. For reviews, cite source locations
and distinguish confirmed problems from unverified risks.
