# Embedded integration notes

> [English](EMBEDDED.md) | [简体中文](EMBEDDED.zh-CN.md)

Resource budgets and measurement guidance for MCU / RTOS integrators: footprint, memory, threads,
timing, power, and logging. Platform contracts live in the
[public headers](../include/mybot/platform/); integration and lifecycle procedures live in
[PORTING.md](PORTING.md). Measure with the target toolchain, product feature flags, and matching
Agora RTSA package; host figures are not MCU limits.

## Footprint

The following is a historical x86_64 Linux snapshot recorded with GCC 13 and default optimization.
The exact revision and complete build flags were not recorded; these values have not been
remeasured for the current revision and are not release size guarantees.

| Artifact | Previously recorded size |
| --- | --- |
| `libmybot_sdk.a` | ~490 KB |
| `libaosl.a` | ~545 KB |
| Minimal consumer binary (SDK core + AOSL + Agora RTSA, no Linux reference implementations) | text ~890 KB, data ~74 KB, bss ~19 KB |

Measure on the target build, recording the revision, compiler, flags, feature configuration, and
RTSA package alongside the results:

    size <firmware.elf>
    ls -l <build>/libmybot_sdk.a

Feature flags directly change the code footprint — `MYBOT_CLOUD_AEC`, `MYBOT_WAKE_WORDS`,
`MYBOT_ENABLE_VIDEO`, and `MYBOT_ENABLE_HTTPS` are the main ones; disable what the product does not
need. Measure the final linked firmware and its map file to attribute flash use; archive file size
is not linked flash consumption. The bundled RTSA shared library is x86_64 Linux only.

## Memory model

The following capacities are calculated from
[the pipeline storage](../src/internal/mybot_media_pipeline.h) and
[its implementation](../src/media/mybot_media_pipeline.c), not measured heap peaks. At 60 ms ptime,
one 16 kHz mono s16 frame is `F = 16000 × 60 / 1000 × 2 = 1920` bytes.

| Storage | Source-derived capacity at 60 ms |
| --- | --- |
| Fixed PCM arrays in app state: capture, pending playback, prompt scratch, uplink send | `4F = 7680` bytes |
| Additional fixed arrays with cloud AEC: reference and two-channel interleave | `3F = 5760` bytes |
| Capture and playback rings, allocated at startup | `2 × 64000 = 128000` usable bytes |
| Additional cloud AEC reference ring | `64000` usable bytes |

Each ring holds 2 s of mono PCM (`16000 × 2 × 2 = 64000` bytes, or 62.5 KiB).
[Ring allocation](../src/support/mybot_ringbuf.c) also adds a guard byte and metadata per ring;
allocator overhead is additional. These figures exclude the rest of the app state, queues, stacks,
RTSA, and platform buffers. Fixed PCM arrays scale with ptime; the 2 s ring capacities do not.

Preallocated PCM arrays do **not** make the end-to-end audio path allocation-free:

- **RTC downlink, per frame:** [the RTC adapter](../src/rtc/mybot_agora_rtc.c) allocates an
  `rtc_event_t` and a PCM payload copy, then queues the event for `rtc_mpq`.
  [AOSL](../third_party/aosl)'s `kernel/mpq.c` allocates a queued function object and copies its
  name. The callback copies PCM into the playback ring before these temporary objects are freed.
- **RTC uplink, per frame:** the send worker reuses its PCM arrays, but its synchronous call to
  `rtc_mpq` still allocates an AOSL function object and name and initializes a mutex/condition pair
  for waiting; their resource cost depends on the HAL. It borrows the PCM until the call returns.
  Video submission uses the same synchronous MPQ path; having no SDK video ring does not
  imply zero per-frame allocation.
- **Session flush:** consumer workers temporarily allocate one frame of drain scratch per ring
  being drained. The normal capture/playback timers reuse their fixed arrays.
- **Control and RTM:** service responses, request headers, JSON nodes, and RTM message copies have
  additional transient allocations. [HTTP](../src/support/mybot_http_client.c) allocates a 2 KiB
  request buffer and a receive buffer growing from 4 KiB to 32 KiB. A separate response body is
  allocated before the receive buffer is freed; later JSON parsing adds nodes alongside that body.
  The 32 KiB receive limit is not a total request heap ceiling.
- **Platform and RTSA:** prompt asset storage, camera/encoder buffers, TLS, and RTSA internal
  memory require separate target measurements. Prompt `open()` does not require a whole asset to
  be loaded into RAM; account for the actual adapter implementation.

Measure peak heap, minimum free heap, largest free block, and allocation rate during sustained
audio/video, prompts, HTTP requests, and repeated start/stop. Include queued payloads, AOSL
allocations, and allocator overhead, not only `aosl_hal_malloc()` calls made directly by MyBot.
Use [PORTING.md](PORTING.md) for connectivity, pairing, and conversation lifecycle behavior.

## Threads and stacks

| MPQ thread | Responsibility | Requested stack |
| --- | --- | --- |
| `control_mpq` | App state, device lifecycle, blocking HTTP control, UI/volume, resource transitions | 16 KiB |
| `rtc_mpq` | Agora RTSA lifecycle, serialized state, and vendor callback dispatch | 8 KiB |
| `mybot_mpq` | Uplink audio send at the ptime cadence | 16 KiB |
| `cap_mpq` | Microphone capture | 16 KiB |
| `pb_mpq` | Playback and AEC reference | 16 KiB |
| `key_stdin_mpq` (Linux reference only) | Stdin key events | 4 KiB |

Core requested stacks total 4 × 16 KiB + 8 KiB = 72 KiB when all five workers exist. This is a
source-derived budget, not measured stack use; HAL minimums and alignment can change actual
allocation. Stack sizes are compile-time constants: the control worker uses `CONTROL_MPQ_STACK_SIZE` in
`src/core/mybot_app.c`, while audio workers use `MEDIA_MPQ_STACK_SIZE` in
`src/media/mybot_media_pipeline.c`. Measure each worker's stack high-water mark on the target,
including error and teardown paths, before tuning. Add platform encoder/driver tasks and RTSA's
internal threads separately. Threading and borrowed-buffer contracts are defined in the
[audio](../include/mybot/platform/mybot_audio.h) and
[video](../include/mybot/platform/mybot_video.h) headers.

## Timing and real-time behavior

- Audio timers use the configured ptime (20 / 40 / 60 ms; the bundled Linux RTSA uses 60 ms).
  Package compatibility and I/O shutdown requirements are covered in [PORTING.md](PORTING.md).
- The control timer requests a 100 ms interval; blocking control work can delay ticks.
  Device-service polling is server-driven, with each `poll_after_seconds` hint clamped to 3..60 s.
  Runtime polling starts at a 30 s default until the
  first binding-status response is received.
- The HTTP client checks a shared 5 s deadline for connect/send/receive. This is not an absolute
  wall-clock bound: the plain TCP path calls `aosl_hal_gethostbyname()` without a timeout argument
  before checking the remaining deadline. Include target DNS/transport blocking in measurements.
- Device-service HTTP runs synchronously on `control_mpq`, delaying queued UI and video-control
  actions. Audio workers are separate, but media sending and downlink dispatch share `rtc_mpq`;
  they also compete for CPU and allocation services. Measure scheduling delay, audio underruns,
  encoder-handler latency, and shutdown duration under the product workload.

## Power management

Current status: the SDK has **no standby / low-power mode**. While `mybot_is_running()` is
true, worker threads and timers keep running. The power levers belong to the integrator:

- **Sleep and radio**: measure idle and stopped-device current separately. The platform owns
  provisioning and radio power; use the network and start/stop procedure in
  [PORTING.md](PORTING.md) when entering low power or provisioning again.
- **Audio path**: gate the codec/amplifier in the audio implementations; the SDK owns volume control — a
  registered device-volume implementation (hardware hook) is the primary path, with a software media
  gain as fallback.
- **Polling**: intervals are server-driven and clamped to 3..60 s (30 s initial runtime default);
  agree on relaxed intervals with the server if idle power matters.

When `MYBOT_WAKE_WORDS=ON`, include the platform local-ASR workload on the capture MPQ in idle
power and timing measurements; enabling it does not itself stop the playback or send workers.

## Logging

- `mybot_start()` raises AOSL logging to at least `AOSL_LOG_NOTICE`; an existing INFO or DEBUG
  level is preserved. A previously selected stricter threshold is therefore not preserved at start.
- RTSA is initialized with `RTC_LOG_ERROR`. Its initialization changes global AOSL logging, so
  the adapter saves and restores the AOSL level on both success and failure.
- Routine RTM message/forwarding logs use `AOSL_LOG_DBG`; failures remain visible at warning/error
  levels. Measure log formatting and transport cost when enabling DEBUG, especially for payloads.
- Pairing codes currently appear at NOTICE in the core and announcement logs. Account for that
  output in production log handling; do not add device tokens or other credentials to logs.

These policies are implemented in [app startup](../src/core/mybot_app.c) and
[the RTC adapter](../src/rtc/mybot_agora_rtc.c).
