# mybot

[![CI](https://github.com/junlon2006/mybot/actions/workflows/ci.yml/badge.svg)](https://github.com/junlon2006/mybot/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/junlon2006/mybot)](LICENSE)

**[English](README.md) | [简体中文](README.zh-CN.md)**

`mybot` is a cross-platform **AI multimodal interaction SDK** for edge devices: it lets smart devices
send voice and optional device-camera video to cloud AI agents over Agora RTC for real-time
conversation and visual recognition. The platform/application owns
APSTA provisioning and Wi-Fi credentials; the SDK consumes connectivity events and handles device
pairing and authentication, a conversation state machine, full-duplex voice interaction (Agora RTSA
with Agora AI capabilities), optional device-video uplink, button/LCD workflows, and optional local
wake-word recognition.
Platform-specific capabilities are injected through a small set of `ops` interfaces;
the core depends only on C99 and AOSL and can be ported to virtually any platform — Linux, an
RTOS, or a bare-metal MCU.

> Current version: **1.2.0**. The bundled Agora RTSA
> binary and AOSL have separate licensing and usage terms; read
> [License and third-party dependencies](#license-and-third-party-dependencies) before using the
> SDK in a product.

## Table of Contents

- [Features](#features)
- [Conversation flow](#conversation-flow)
- [Boundaries and limitations](#boundaries-and-limitations)
- [Quick start](#quick-start)
- [Integrating into a host project](#integrating-into-a-host-project)
- [Build configuration](#build-configuration)
- [Architecture](#architecture)
- [Repository layout](#repository-layout)
- [Documentation](#documentation)
- [Development and verification](#development-and-verification)
- [Contributing and support](#contributing-and-support)
- [License and third-party dependencies](#license-and-third-party-dependencies)

## Features

- **Real-time AI conversation**: Hold live voice chats with a cloud AI agent; speech recognition,
  language-model reasoning, and speech synthesis (ASR / LLM / TTS) are orchestrated in the cloud.
- **Multimodal device input**: Optionally upload platform-encoded camera frames so the cloud AI agent
  can perform visual recognition alongside voice. JPEG, H.264, and H.265 are supported; video is
  uplink-only and the server does not send video back to the device.
- **Portable to virtually any platform**: The core depends only on C99 and AOSL, and device
  capabilities are injected through the `ops` contract, so it never touches any OS or peripheral
  API directly — Linux, an RTOS, or a bare-metal MCU.
- **APSTA integration**: Non-blocking startup; the platform owns provisioning and Wi-Fi events drive
  the application state machine.
- **Pairing and authentication**: Pair code → device claim → persisted long-lived credential, with
  automatic re-pairing when authentication is rejected.
- **Conversation state machine**: Five device-service lifecycle states — `unprovisioned / pairing /
  awaiting_claim / runtime / in_conversation` — drive the device-server interaction.
- **Application lifecycle state**: `mybot_get_state()` exposes startup, pairing, connectivity,
  shutdown, and conversation state. During device-service provisioning (`unprovisioned`, `pairing`,
  or `awaiting_claim`) it returns `MYBOT_STATE_PAIRING`; only an authenticated runtime is
  `MYBOT_STATE_READY`. After the device service accepts a conversation it returns
  `MYBOT_STATE_IN_CONVERSATION`; normal teardown returns to `MYBOT_STATE_READY`, while
  `MYBOT_STATE_WIFI_DISCONNECTED` takes precedence when connectivity is lost.
- **Full-duplex voice · barge-in**: Uplink and downlink run simultaneously; the user can interrupt
  the AI mid-reply at any time, and the microphone keeps streaming so the cloud agent hears and
  responds to new input.
- **Full-duplex voice interaction · Agora AI capabilities**: Built on Agora RTSA, with cloud AEC and
  AI QoS.
- **Volume control**: The SDK owns volume. When the platform registers a real-device volume
  implementation (codec / amplifier / mixer), volume changes drive hardware volume directly; otherwise
  the SDK falls back to a digital software gain applied to playback PCM. There is no
  application-facing volume API.
- **Optional local wake words**: Off by default; wake behavior is identical to starting a
  conversation with a physical button.
- **Button and LCD workflows**: Semantic screen states (provisioning / pairing / pair code / ready /
  in conversation); how each is displayed is up to the platform.
- **Voiceprint registration status**: During an active conversation, the SDK listens on the
  conversation RTM channel for the server's `message.sal_status` / `VP_REGISTER_SUCCESS` message
  and exposes `MYBOT_LCD_INDICATOR_VP_REGISTERED` as an in-conversation LCD overlay.
- **Pairing-code voice prompt**: Once per pair code, plays a fixed prompt ("Please enter the
  pairing code in the console") followed by one sound per digit through the normal speaker path. Assets are raw
  16 kHz mono s16 PCM files under `./assets/locales/<locale>/` (`prompt.pcm`, `0.pcm`..`9.pcm`);
  the platform owns them and the SDK core contains no audio decoder.
- **HTTPS transport**: The device service accepts HTTPS only by default. Linux uses OpenSSL; MCU
  platforms may integrate mbedTLS or a vendor TLS and must validate the certificate chain and host
  name.

## Boundaries and limitations

- Audio is fixed at 16 kHz, mono, 16-bit PCM; `ptime` is configurable to 20/40/60 ms (default
  60 ms).
- Video uplink is optional and uses platform-owned JPEG, H.264, or H.265 encoding. The SDK does not
  encode, decode, buffer, or receive video; it sends one primary encoded stream to the cloud agent.
- The RTC implementation is specific to Agora RTSA; no other RTC protocol adapter is provided.
- Local ASR wake words are an optional platform implementation, off by default; enabling them
  requires the platform to register an implementation.
- The platform/application owns APSTA provisioning and credential storage; the SDK consumes only
  connectivity events through the Wi-Fi interface.
- The device server is not part of this repository; running the examples requires a compatible
  server endpoint.

## Conversation flow

The SDK establishes a real-time audio channel and optional encoded video uplink to a cloud AI agent
over Agora RTC, forming a multimodal interaction loop:

```mermaid
flowchart LR
    user["User speaks"] --> mic["Microphone · capture"]
    mic --> up["Agora RTC uplink"]
    camera["Camera"] --> encode["Platform encoder<br/>JPEG · H.264 · H.265"]
    encode --> up
    up --> agent["Cloud AI agent<br/>ASR · LLM · TTS · vision"]
    agent --> down["Agora RTC downlink"]
    down --> spk["Speaker · playback"]
    spk --> reply["User hears the AI reply"]
```

- **Uplink**: the device captures 16 kHz PCM from the microphone and sends it to the cloud AI agent
  over Agora RTC.
- **Visual uplink (optional)**: the platform captures and encodes camera frames; the SDK forwards
  the encoded frames over the same RTC channel for cloud-side visual recognition. The server does
  not send video back to the device.
- **Cloud orchestration**: the AI agent performs speech recognition (ASR), language-model reasoning
  and reply generation (LLM), and speech synthesis (TTS).
- **Downlink**: the AI reply audio returns over Agora RTC and plays out on the device speaker.
- **Session scheduling**: the device server handles pairing / claim and allocates the RTC channel for
  each conversation.

The loop is **full-duplex**: uplink and downlink run at the same time, with no turn-taking. The user
can **interrupt** the AI at any point mid-reply — the device keeps the microphone streaming, and the
cloud agent detects the new input, stops its reply, and listens for the new command.

For the device-side audio pipeline and state machine, see [Architecture](#architecture).

## Quick start

The Linux reference platform lets you run the full workflow on a development machine. Requirements:
Linux x86_64, CMake 3.16+, a C99 compiler, and ALSA and OpenSSL development packages. The bundled
Agora RTSA shared library is also the x86_64 Linux build. AOSL is pulled in as a pinned git
submodule: initialize it before the first build (or clone with `--recurse-submodules`).

```bash
git submodule update --init --recursive
sudo apt-get update
sudo apt-get install -y build-essential cmake libasound2-dev libssl-dev
cmake -S . -B build -DCONFIG_PLATFORM=linux -DMYBOT_ENABLE_ASAN=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Run the example:

```bash
./build/examples/linux/mybot \
  --server https://api.example.com \
  --device-id AG-DEMO-001 \
  --fw-ver 1.2.0 \
  --hw-model linux-reference
```

The example also plays an optional pairing-code voice prompt ("Please enter the pairing code in the console...") from
`./assets/locales/<locale>/` (raw 16 kHz mono s16 PCM: `prompt.pcm`, `0.pcm`..`9.pcm`).
The default locale is `zh-CN`; set `MYBOT_LOCALE` and `MYBOT_ASSETS_DIR` to override.

Once ready, press `s` to start a conversation, `q` to stop it, `p` to re-pair, `u` / `d` to raise /
lower the volume, `e` to exit.

The Linux reference implementation is a **development stand-in**: it reuses the host network and
reports STA as connected immediately; it does not implement real APSTA provisioning. Audio uses the
ALSA `default` device. KV data is written to `.mybot-kv-store/` in the current directory by default;
override the location with the `MYBOT_KV_STORE_DIR` environment variable.

## Integrating into a host project

We recommend vendoring the repository as a source submodule, and mybot itself depends on AOSL
through a nested submodule — initialize submodules after adding it with
`git submodule update --init --recursive`.
The host must provide an Agora RTSA header and shared or static library matching the target
architecture and ensure AOSL supports the target platform.

An installed package is also supported: `cmake --install` exports `mybot::sdk` (and the bundled
`mybot::aosl`), and a consumer project can use `find_package(mybot CONFIG REQUIRED)` after pointing
`MYBOT_AGORA_SDK_DIR` / `MYBOT_AGORA_RTC_LIBRARY` at a target-architecture Agora RTSA package.

```cmake
set(CONFIG_PLATFORM my_mcu CACHE STRING "" FORCE)
set(AGORA_SDK_DIR /opt/agora-rtsa CACHE PATH "" FORCE)
set(AGORA_RTC_LIBRARY /opt/agora-rtsa/lib/libagora-rtc-sdk.so CACHE FILEPATH "" FORCE)

set(MYBOT_BUILD_LINUX_PLATFORM OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MYBOT_AUDIO_PTIME_MS 60 CACHE STRING "" FORCE)
set(MYBOT_WAKE_WORDS OFF CACHE BOOL "" FORCE)
set(MYBOT_ENABLE_HTTPS ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/mybot)
target_link_libraries(device_firmware PRIVATE mybot::sdk)
```

Register one `mybot_platform_descriptor_t` before `mybot_start()`. A non-NULL ops pointer is the sole
declaration that the platform supports that function. Registration validates the complete descriptor,
including the required Wi-Fi, KV, key, capture, and playback tables, before committing the complete
descriptor.
`mybot_start()` then checks the ops required by the active build and runtime configuration before
creating any platform resources. Every platform is submitted through this one descriptor. For the
full implementation order, minimal code, threading constraints, and acceptance checklist, see
[docs/PORTING.md](docs/PORTING.md).

Minimal application lifecycle:

```c
/* Host-defined function that fills and registers one complete descriptor. */
my_mcu_platform_register();
mybot_config_t config = {0};
mybot_start(&config);
while (mybot_is_running()) {
    platform_sleep_ms(100);
}
mybot_stop();
```

`mybot_start()` is non-blocking: it starts provisioning first, then initializes storage,
buttons, audio, and the device service asynchronously once usable network connectivity is
reported. RTC is initialized on demand when a conversation starts.
`mybot_start()` and `mybot_stop()` are thread-safe and serialize their work through the application
lifecycle gate and control owner. `mybot_stop()` waits for worker shutdown and must not be called
from inside a platform or SDK callback. If a worker cannot be joined, the SDK retains its resources
for a later stop attempt; callers should treat shutdown as incomplete until the runtime reports
stopped. The application acquires one reference to the process-wide AOSL runtime inside
`mybot_start()` and releases it at the end of `mybot_stop()`.
The RTSA lifecycle is initialized and finalized through `agora_rtc_init()` / `agora_rtc_fini()`.
A host that uses AOSL directly must keep its own `aosl_ctor()` / `aosl_dtor()` pair balanced.

The bundled Linux RTSA package is a shared library. CMake supplies a build-tree runtime path for the
reference executable and tests. Installed-package consumers must deploy `libagora-rtc-sdk.so` and
make it discoverable through their install RPATH or runtime loader configuration; the mybot CMake
install target does not install or redistribute that dependency.

## Build configuration

The following options can be set via the CMake command line or cache variables before the host's
`add_subdirectory()` call:

| Option | Default | Description |
| --- | --- | --- |
| `MYBOT_AUDIO_PTIME_MS` | `60` | Audio packet duration; accepts only 20, 40, 60 ms |
| `MYBOT_CLOUD_AEC` | `ON` | Server-side AEC; the uplink carries mic and reference channels |
| `MYBOT_WAKE_WORDS` | `OFF` | Enable the platform local-ASR wake-word implementation |
| `MYBOT_ENABLE_VIDEO` | `OFF` | Enable platform-encoded JPEG/H.264/H.265 video uplink |
| `MYBOT_VIDEO_MAX_FRAME_BYTES` | `524288` | Maximum encoded video frame accepted by the SDK |
| `MYBOT_AI_QOS` | `ON` | Agora AI QoS |
| `MYBOT_FAST_SEND_MULTIPLIER` | `3` | Fast-send multiplier; accepts only 1–5 |
| `MYBOT_ENABLE_HTTPS` | `ON` | Enable the platform HTTPS transport; keep ON for production builds |
| `MYBOT_ALLOW_INSECURE_HTTP` | `OFF` | Local development only: explicitly allow plaintext HTTP |
| `MYBOT_ENABLE_ASAN` | `OFF` | GCC/Clang AddressSanitizer; recommended for host tests |
| `MYBOT_ENABLE_UBSAN` | `OFF` | GCC/Clang UndefinedBehaviorSanitizer; recommended for host tests |
| `MYBOT_ENABLE_COVERAGE` | `OFF` | Instrument mybot targets for gcov; used by the CI coverage job |

Two independent variables select platform code: `CONFIG_PLATFORM` chooses the AOSL HAL port
consumed by `third_party/aosl` (e.g. `linux`, `esp32`), while `MYBOT_BUILD_LINUX_PLATFORM` builds
the bundled Linux reference implementations (`platforms/linux/`: ALSA, stdin, file KV, console LCD,
OpenSSL) and requires `CONFIG_PLATFORM=linux`. An MCU port sets `CONFIG_PLATFORM=my_mcu` and keeps
`MYBOT_BUILD_LINUX_PLATFORM=OFF`.

For example:

```bash
cmake -S . -B build-wake \
  -DCONFIG_PLATFORM=linux \
  -DMYBOT_AUDIO_PTIME_MS=60 \
  -DMYBOT_WAKE_WORDS=ON
```

`MYBOT_AUDIO_PTIME_MS` must match the RTSA package's
`CONFIG_MINIMAL_TIMER_INTERVAL_MS` setting. The bundled x86_64 Linux package is fixed at 60 ms;
for 20 or 40 ms, provide a matching external package with `AGORA_SDK_DIR` and
`AGORA_RTC_LIBRARY`. CMake checks the package's `.config` or `include/global_config.cmake` and
rejects a mismatch; packages without this build metadata are rejected. When a parent project
predefines the Agora imported target, set `AGORA_SDK_DIR` to that same package so the check still
has a verifiable source of truth. CMake cannot inspect an imported target's ABI; the parent project
must ensure its include and library paths refer to this same package.

For a 20 ms build, add the paths to the matching RTSA package, for example:

```bash
cmake -S . -B build-20 \
  -DMYBOT_AUDIO_PTIME_MS=20 \
  -DAGORA_SDK_DIR=/opt/agora-rtsa-20 \
  -DAGORA_RTC_LIBRARY=/opt/agora-rtsa-20/lib/libagora-rtc-sdk.so
```

The Linux reference platform has no local ASR implementation, so enabling `MYBOT_WAKE_WORDS`
requires the host to register an additional implementation; otherwise the app fails to start with a
clear error.

Plaintext HTTP never falls back automatically. Only in an isolated local development environment may
you configure `-DMYBOT_ENABLE_HTTPS=OFF -DMYBOT_ALLOW_INSECURE_HTTP=ON`. This combination transmits
device credentials and RTC parameters in cleartext and must not be used on devices, shared
networks, or release builds.

## Architecture

The SDK uses a layered architecture: the host application drives the core through the public API,
the core modules sit on top of the AOSL portability layer and the platform `ops` contract, and all
platform differences are absorbed by the platform implementations. The device server, the Agora RTC cloud,
and the cloud AI agent are runtime external dependencies and are not part of this repository.

```mermaid
flowchart TB
    subgraph host["Host application"]
        host_app["Device firmware / Linux example"]
    end

    subgraph api["Public API · include/mybot"]
        api_h["mybot_start / mybot_is_running / mybot_stop<br/>mybot_get_state"]
    end

    subgraph core["SDK core · src/"]
        app_c["mybot_app<br/>startup orchestration · event dispatch · threads"]
        app_state["Application state model<br/>phase · connectivity · device projection"]
        presenter["LCD presenter<br/>state projection · semantic screens"]
        state_m["Device state machine<br/>pairing · claim · conversation lifecycle"]
        svc_c["Device-service client<br/>pair / claim / conversation polling"]
        rtc_c["Agora RTC<br/>RTSA wrapper"]
        media_c["Audio pipeline<br/>ring buffers · AEC reference · wake words"]
        video_c["Video uplink<br/>platform encoder bridge"]
    end

    subgraph infra["Foundation layer"]
        aosl["AOSL<br/>MPQ threads · timers · atomics · logging"]
        ops["Platform ops contract<br/>wifi · kv_store · key · lcd<br/>audio · video · https · announce · asr"]
    end

    subgraph plat["Platform implementations"]
        linux_b["Linux reference<br/>ALSA · stdin · file · console · OpenSSL"]
        mcu_b["MCU implementation · host-provided"]
    end

    subgraph ext["External services · cloud"]
        svc_e["Device server<br/>pairing · claim · session scheduling (HTTPS)"]
        agora_e["Agora RTC cloud"]
        agent_e["AI agent<br/>ASR · LLM · TTS"]
    end

    host_app --> api_h
    api_h --> app_c
    api_h --> app_state
    app_c --> state_m
    app_c --> app_state
    state_m --> app_state
    app_state --> presenter
    app_c --> media_c
    app_c --> video_c
    state_m --> svc_c
    svc_c --> rtc_c
    rtc_c <--> media_c
    app_c --> aosl
    app_c --> ops
    presenter --> ops
    svc_c --> aosl
    rtc_c --> aosl
    media_c --> aosl
    video_c --> aosl
    video_c --> ops
    ops --> linux_b
    ops --> mcu_b
    svc_c -->|HTTPS polling| svc_e
    rtc_c -->|real-time audio| agora_e
    video_c -->|encoded video uplink| agora_e
    agora_e <--> agent_e
    svc_e -->|schedules session| agent_e
```

Layer notes:

- **Public API** ([include/mybot/mybot.h](include/mybot/mybot.h)): application lifecycle and
  state queries (`mybot_start` / `mybot_is_running` / `mybot_get_state` / `mybot_stop`);
  non-blocking startup. Use `mybot_get_state()` for key or UI decisions:
  `MYBOT_STATE_READY` can start a conversation and `MYBOT_STATE_IN_CONVERSATION` can stop one;
  `MYBOT_STATE_PAIRING` is not conversation-ready. LCD output is only a rendering result, not a
  source of lifecycle state. Conversation and pairing actions are triggered by platform key /
  wake-word events and handled inside the SDK core. Wi-Fi and device-lifecycle events update one
  atomic state-model snapshot; `mybot_get_state()` and the LCD presenter derive their views from
  that same snapshot.
- **SDK core** ([src/](src/)): one control owner serializes application state, the device lifecycle,
  UI and volume actions, and resource startup and shutdown. RTC commands are forwarded synchronously
  to the dedicated `rtc_mpq`, which serializes RTSA lifecycle, vendor calls, and callbacks. Control
  callbacks only publish short events or atomic mailboxes to their owner. The core also contains the
  device-service HTTP client, the Agora RTSA session wrapper, audio ring buffers, the optional video
  uplink bridge, and the optional local wake-word engine. Core code never touches any OS or
  peripheral API directly.
- **Foundation layer**: AOSL provides portable threads / MPQ queues / timers / logging; the
  platform `ops` contract defines the device capabilities the SDK requires. Both are implementable
  per platform.
- **Platform implementations**: the Linux reference implementation and each MCU platform register
  against the same contract.
- **External services**: the device server (pairing / claim / session scheduling, HTTPS only), the
  Agora RTC cloud (real-time audio and optional video transport), and the cloud AI agent (speech
  recognition / understanding / synthesis / visual recognition).

### Threading model

`mybot_start()` creates four core worker threads (AOSL MPQ queues) with strictly separated
responsibilities. RTC creates a fifth, on-demand `rtc_mpq` worker when a conversation starts; all
RTSA lifecycle calls and vendor callbacks are serialized there:

| Thread (MPQ) | Driven by | Responsibility |
| --- | --- | --- |
| `control_mpq` | Events and 100 ms timer | Owns application state, device lifecycle, blocking HTTP/RTC control, UI/volume actions, and resource transitions |
| `mybot_mpq` | ptime timer | Sends uplink audio at the packetization cadence (Agora RTSA) |
| `cap_mpq` | ptime timer | Mic capture → capture ring buffer → (optional) wake words |
| `pb_mpq` | ptime timer | Playback ring buffer → speaker; also feeds the AEC reference channel |
| `rtc_mpq` | RTC/RTM callbacks and synchronous RTC commands | Serializes RTSA lifecycle, vendor calls, and application callbacks; created on demand |

Callbacks keep their work bounded: they enqueue a short control event or publish an atomic mailbox.
PCM capture, RTC uplink/downlink, and playback stay on the direct data path and never pass through
`control_mpq`. The real-time audio timers (cap / pb / send) are independent, so blocking control or
device-service work cannot stall the audio cadence.
When video is enabled, the platform encoder owns frame capture and timing; the SDK adds no video
worker or frame queue. Each encoded frame is borrowed only for the synchronous RTC send call.

### Workflows

#### Device state machine

```mermaid
stateDiagram-v2
    [*] --> unprovisioned
    unprovisioned --> pairing: start pairing
    pairing --> awaiting_claim: pair code received
    awaiting_claim --> runtime: device claimed
    runtime --> in_conversation: conversation starts
    in_conversation --> runtime: conversation ends
    runtime --> unprovisioned: auth rejected
    in_conversation --> unprovisioned: auth rejected
```

When device authentication is rejected, the device returns to `unprovisioned` and automatically
restarts pairing on the next state-machine tick.

#### Audio data flow

```mermaid
flowchart LR
    mic["Microphone"] -->|capture ops| cap["Capture worker (cap_mpq)"]
    cap --> capbuf["Capture ring buffer"]
    cap --> wake["Local wake words · when idle"]
    capbuf --> send["Send worker (mybot_mpq)"]
    send -->|ptime frames| rtc_u["Agora RTC uplink"]

    rtc_d["Agora RTC downlink"] --> pbbuf["Playback ring buffer"]
    pbbuf --> pb["Playback worker (pb_mpq)"]
    pb -->|playback ops| spk["Speaker"]
    pb -.->|AEC reference| send
```

With `MYBOT_CLOUD_AEC=ON`, the downlink audio is interleaved with the microphone signal as a
reference channel and sent uplink together, letting the server cancel echo. The uplink and downlink
run concurrently (**full-duplex**): the microphone keeps streaming during AI replies, which is what
lets the cloud agent support user interruption.

#### Multimodal video flow

```mermaid
flowchart LR
    camera["Camera"] --> encoder["Platform encoder"]
    encoder -->|JPEG · H.264 · H.265| video["MyBot video handler"]
    video --> rtc_v["Agora RTC video uplink"]
    rtc_v --> vision["Cloud AI visual recognition"]
```

The video handler uses RTSA's target-bitrate callback to let the platform encoder adapt to current
uplink capacity. The device sends only the primary video stream; remote video subscription is off.

## Repository layout

```text
mybot/
├── include/mybot/          # public headers and platform interface specifications
├── src/                    # cross-platform implementation; internal/ is not public API
├── platforms/linux/        # Linux reference implementations (ALSA/stdin/file/console)
├── examples/linux/         # Linux example application entry
├── tests/                  # unit, platform, and host integration tests
├── docs/                   # porting and release guides
├── cmake/                  # toolchain helpers
└── third_party/            # AOSL submodule and the Agora RTSA SDK
```

Key CMake targets:

- `mybot::sdk` — the cross-platform SDK core (AOSL + Agora RTSA).
- `mybot::platform_linux` — the Linux reference implementation; not part of the cross-platform core.
- `mybot::linux_example` — the Linux CLI example application.

## Documentation

- [docs/PORTING.md](docs/PORTING.md) ([简体中文](docs/PORTING.zh-CN.md)) — porting guide and
  acceptance checklist
- [mybot-bk7258](https://github.com/junlon2006/mybot-bk7258) — BK7258 reference firmware project
- [mybot-bk7259](https://github.com/junlon2006/mybot-bk7259) — BK7259 reference firmware project
- [mybot-esp32](https://github.com/junlon2006/mybot-esp32) — ESP32 cross-platform reference project
- [docs/EMBEDDED.md](docs/EMBEDDED.md) ([简体中文](docs/EMBEDDED.zh-CN.md)) — footprint, memory,
  thread/stack, timing, power and logging guidance for MCU integrators
- [docs/RELEASING.md](docs/RELEASING.md) ([简体中文](docs/RELEASING.zh-CN.md)) — release process
- [CHANGELOG.md](CHANGELOG.md) — version history
- API reference — generated by Doxygen from the public headers with
  `doxygen build/docs/Doxyfile`; CI builds it on every push / PR and publishes it as an artifact

## Development and verification

```bash
cmake -S . -B build -DCONFIG_PLATFORM=linux -DMYBOT_ENABLE_ASAN=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
find include src platforms/linux examples/linux tests -type f \
  \( -name '*.c' -o -name '*.h' \) \
  -exec clang-format --dry-run --Werror {} +
```

- Host-checked C code follows the root `.clang-format`; `third_party/` keeps upstream content,
  and BK725x Armino sources use their firmware toolchain's formatting rules.
- CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) runs the build, tests, and format check
  on every push / PR; make sure your local commands match CI before merging.
- CI builds with both GCC and Clang under ASan and UBSan, runs cppcheck and clang-tidy static
  analysis, and publishes gcov/lcov coverage to Codecov.
- Commit messages follow Conventional Commits (see `CONTRIBUTING.md`). Install the local
  `commit-msg` hook once per clone with `./scripts/setup-githooks.sh`; CI validates every pushed /
  PR commit subject.

## Contributing and support

We welcome issues, discussions, and pull requests. Before you start, please read (each document is
available in English and Simplified Chinese):

- [CONTRIBUTING](CONTRIBUTING.md) ([简体中文](CONTRIBUTING.zh-CN.md)) — development workflow and contribution guidelines
- [SUPPORT](SUPPORT.md) ([简体中文](SUPPORT.zh-CN.md)) — how to get help

## License and third-party dependencies

Our own code is released under the Apache License 2.0 in the root [LICENSE](LICENSE). This does not
change the licensing of third-party components:

- AOSL carries additional conditions listed in `third_party/aosl/LICENSE`.
- The Agora RTSA SDK binary is subject to its software license, trial period, and commercial
  licensing requirements. The bundled x86_64 Linux binary is for development/demo use only;
  commercial or production use and redistribution require authorization from Agora (声网) — contact
  Agora's sales channel before shipping or redistributing it.
- `mybot_json` is derived from cJSON and retains the MIT license notice.

Verify these terms independently before shipping or redistributing a product. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.
