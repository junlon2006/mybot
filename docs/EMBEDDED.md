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
| Minimal consumer binary (SDK core + AOSL + Agora RTSA, no Linux backends) | text ~890 KB, data ~74 KB, bss ~19 KB |

How to measure on your target:

    size <firmware.elf>
    ls -l <build>/libmybot_sdk.a

Feature flags directly change the code footprint — `MYBOT_CLOUD_AEC`, `MYBOT_WAKE_WORDS`,
`MYBOT_SHOW_TRANSCRIPT`, and `MYBOT_ENABLE_HTTPS` are the main ones; disable what the product does
not need. On MCU targets the Agora RTSA library dominates the flash budget, and it must be the
target-architecture package (the bundled archive is x86_64 Linux only).

## Memory model

- **Static per-frame audio buffers** (inside the app state): one 16 kHz mono 16-bit frame per
  worker — capture, pending playback, uplink send, and the AEC reference are each ~1.9 KB
  (960 samples at 60 ms ptime); the AEC interleave buffer is ~3.8 KB.
- **Ring buffers**: each holds 2 s of audio = 64 KB. Capture and playback always exist; a third
  (AEC reference) is created with `MYBOT_CLOUD_AEC=ON`, totaling 192 KB.
- **Heap**: HTTP responses allocate 4 KB initially and grow to at most 32 KB per request (freed
  after use); JSON parsing and platform backends (ALSA, OpenSSL, file KV) allocate transiently.
  All core allocations go through `aosl_hal_malloc`, which each platform can re-point.
- Control-plane state (app, device lifecycle, RTC session) is statically allocated; there are no
  per-conversation heap allocations in the core besides the HTTP/JSON temporaries above.

## Threads and stacks

| MPQ thread | Responsibility | Stack |
| --- | --- | --- |
| `startup_mpq` | Serializes Wi-Fi event handling and startup transitions | 16 KB |
| `state_mpq` | Device state machine, blocking HTTP polling | 16 KB |
| `mybot_mpq` | Uplink audio send at the ptime cadence | 16 KB |
| `cap_mpq` | Microphone capture | 16 KB |
| `pb_mpq` | Playback and AEC reference | 16 KB |
| `key_stdin_mpq` (Linux reference only) | Stdin key events | 4 KB |

Core stack budget is therefore 5 × 16 KB = 80 KB. Stack sizes are compile-time constants
(`MPQ_STACK_SIZE` in `src/core/mybot_app.c`); profile on the target before tuning. The real-time
audio timers live on separate MPQs so a single blocking backend cannot stall the whole audio path,
and the state machine runs on its own thread because HTTP polling blocks. The Agora RTSA SDK owns
additional internal threads whose stacks are vendor-managed.

## Timing and real-time behavior

- Audio format is fixed at 16 kHz, mono, signed 16-bit; ptime is 20 / 40 / 60 ms (default 60 ms,
  i.e. 960 samples / 1920 bytes per frame).
- Platform `read` / `write` calls must bound their blocking (the Linux ALSA backend polls with a
  50 ms timeout) so workers can observe shutdown and exit promptly.
- The state machine ticks every 100 ms; device-service polling is server-driven with a 3 s minimum
  during pairing and a 30 s default in runtime.
- HTTP requests have a 5 s total deadline.

## Power management

Current status: the SDK has **no standby / low-power mode**. While `mybot_is_running()` is
true, worker threads and timers keep running. The power levers belong to the integrator:

- **Sleep**: call `mybot_stop()` before entering low power and `mybot_start()` on wake;
  this releases workers, audio devices, TLS, and RTC resources.
- **Radio**: the Wi-Fi provisioning backend owns the radio; implement the platform's low-power
  policy there.
- **Audio path**: gate the codec/amplifier in the audio backends; the SDK owns volume control — a
  registered device-volume backend (hardware hook) is the primary path, with a software media
  gain as fallback.
- **Polling**: intervals are server-driven (3 s minimum pairing, 30 s default runtime); agree on
  relaxed intervals with the server if idle power matters.

When `MYBOT_WAKE_WORDS=ON`, the platform local-ASR backend runs on the capture MPQ and must be
power-aware (it is the natural place to keep only the microphone path alive while idle).

## Logging

- Logging comes from AOSL; set the runtime level with `aosl_set_log_level()` (debug through
  error). The Linux reference prints to stdout.
- Keep hot-path logging (audio timers) minimal — formatting happens per call.
- Never log device tokens. The reference app logs the pairing code at INFO level; production
  builds should redact it.

See [PORTING.md](PORTING.md) for the full platform contract and acceptance checklist.
