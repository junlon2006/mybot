# Embedded integration notes

> [English](EMBEDDED.md) | [简体中文](EMBEDDED.zh-CN.md)

Resource and timing facts for MCU / RTOS integrators: footprint, memory, threads, power, and
logging. Numbers below are measured on the x86_64 Linux reference build (GCC 13, default
optimization) and are **indicative only** — always measure with the target toolchain, real
configuration (`-Os`, enabled feature flags), and the target-architecture Agora RTSA package.

## Footprint

| Artifact | Size (x86_64 reference) |
| --- | --- |
| `libmybot_sdk.a` | ~490 KB |
| `libaosl.a` | ~545 KB |
| Minimal consumer binary (SDK core + AOSL + Agora RTSA, no Linux reference implementations) | text ~890 KB, data ~74 KB, bss ~19 KB |

How to measure on your target:

    size <firmware.elf>
    ls -l <build>/libmybot_sdk.a

Feature flags directly change the code footprint — `MYBOT_CLOUD_AEC`, `MYBOT_WAKE_WORDS`, and
`MYBOT_ENABLE_HTTPS` are the main ones; disable what the product does not need. On MCU targets the
Agora RTSA library dominates the flash budget, and it must be the
target-architecture package (the bundled shared library is x86_64 Linux only).

## Memory model

- **Static per-frame audio buffers** (inside the app state): one 16 kHz mono 16-bit frame per
  worker — capture, pending playback, uplink send, and the AEC reference are each ~1.9 KB
  (960 samples at 60 ms ptime); the AEC interleave buffer is ~3.8 KB.
- **Ring buffers**: each holds 2 s of audio = 64 KB. Capture and playback always exist; a third
  (AEC reference) is created with `MYBOT_CLOUD_AEC=ON`, totaling 192 KB.
- **Pairing announcement (optional)**: when a pair code is obtained, the platform streams raw
  16 kHz mono s16 PCM into the playback ring buffer (the SDK contains no decoder) and the prompt
  plays once through the normal speaker path. On the Linux reference the prompt (~150 KB) and
  each digit (~30 KB) are loaded into RAM while playing.
- **Heap**: HTTP responses allocate 4 KB initially and grow to at most 32 KB per request (freed
  after use); JSON parsing and platform implementations (ALSA, OpenSSL, file KV) allocate transiently.
  All core allocations go through `aosl_hal_malloc`, which each platform can re-point.
- Control-plane state (app, device lifecycle, Agora RTC) is statically allocated; there are no
  per-conversation heap allocations in the core besides the HTTP/JSON temporaries above.

The thread-safe application state model stores runtime phase, connectivity, and the device-lifecycle
projection in one atomic snapshot. `mybot_get_state()` derives the public state from that snapshot:
`MYBOT_STATE_WIFI_DISCONNECTED` takes precedence while offline, otherwise an accepted conversation
reports `MYBOT_STATE_IN_CONVERSATION` until normal teardown returns to `MYBOT_STATE_READY`.

## Threads and stacks

| MPQ thread | Responsibility | Stack |
| --- | --- | --- |
| `control_mpq` | App state, device lifecycle, blocking HTTP/RTC control, UI/volume, resource transitions | 16 KB |
| `mybot_mpq` | Uplink audio send at the ptime cadence | 16 KB |
| `cap_mpq` | Microphone capture | 16 KB |
| `pb_mpq` | Playback and AEC reference | 16 KB |
| `key_stdin_mpq` (Linux reference only) | Stdin key events | 4 KB |

Core stack budget is therefore 4 × 16 KB = 64 KB. Stack sizes are compile-time
constants: the control worker uses `CONTROL_MPQ_STACK_SIZE` in
`src/core/mybot_app.c`, while audio workers use `MEDIA_MPQ_STACK_SIZE` in
`src/media/mybot_media_pipeline.c`; profile on the target before tuning. The real-time
audio timers live on separate MPQs, and PCM stays on the direct data path rather than passing
through `control_mpq`, so blocking HTTP or control work cannot stall audio. Control callbacks,
including wake-word callbacks, only enqueue short events or publish atomic mailboxes.
The Agora RTSA SDK owns
additional internal threads whose stacks are vendor-managed.

## Timing and real-time behavior

- Audio format is fixed at 16 kHz, mono, signed 16-bit; ptime is 20 / 40 / 60 ms (default 60 ms,
  i.e. 960 samples / 1920 bytes per frame).
- During shutdown the SDK calls both platform `stop` hooks before waiting for audio workers.
  Each hook must safely interrupt an in-flight `read` / `write`; bounded I/O timeouts remain a
  fallback against driver failures (the Linux ALSA implementation polls with a 50 ms timeout).
- The state machine ticks every 100 ms; device-service polling is server-driven, with each
  `poll_after_seconds` hint clamped to 3..60 s. Runtime polling starts at a 30 s default until the
  first binding-status response is received.
- HTTP requests have a 5 s total deadline.
- Device-service HTTP runs synchronously on `control_mpq`; a queued UI or control action may wait
  up to that deadline behind an in-flight request, while the PCM data path continues independently.
- `mybot_start()` and `mybot_stop()` are thread-safe. Because stop waits for workers and callbacks,
  never call it from a platform or SDK callback.

## Power management

Current status: the SDK has **no standby / low-power mode**. While `mybot_is_running()` is
true, worker threads and timers keep running. The power levers belong to the integrator:

- **Sleep**: call `mybot_stop()` before entering low power and `mybot_start()` on wake;
  this releases workers, audio devices, TLS, RTC resources, and mybot's AOSL runtime reference.
  RTSA finalization completes before mybot releases its application reference.
- **Radio**: the Wi-Fi provisioning implementation owns the radio; implement the platform's low-power
  policy there.
- **Audio path**: gate the codec/amplifier in the audio implementations; the SDK owns volume control — a
  registered device-volume implementation (hardware hook) is the primary path, with a software media
  gain as fallback.
- **Polling**: intervals are server-driven and clamped to 3..60 s (30 s initial runtime default);
  agree on relaxed intervals with the server if idle power matters.

When `MYBOT_WAKE_WORDS=ON`, the platform local-ASR implementation runs on the capture MPQ and must be
power-aware (it is the natural place to keep only the microphone path alive while idle).

## Logging

- Logging comes from AOSL; set the runtime level with `aosl_set_log_level()` (debug through
  error). The Linux reference prints to stdout.
- The Agora RTC module initializes the RTSA SDK at its default NOTICE threshold; lower-priority
  SDK informational logs are suppressed unless a platform port overrides the level.
- Keep hot-path logging (audio timers) minimal — formatting happens per call.
- Never log device tokens. The reference app logs the pairing code at INFO level; production
  builds should redact it.

See [PORTING.md](PORTING.md) for the full platform integration specification and acceptance
checklist.
