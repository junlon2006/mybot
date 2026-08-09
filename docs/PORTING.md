# Porting mybot to a new platform

> [English](PORTING.md) | [简体中文](PORTING.zh-CN.md)

This document defines the platform contract for mybot 0.1.0-rc.1. Public APIs may change before
1.0. Platform code must include only headers under `include/mybot` and link `mybot::sdk`.

mybot is designed to be portable to virtually any platform — Linux, an RTOS, or bare metal — as
long as AOSL has a `CONFIG_PLATFORM` port for it and an Agora RTSA library exists for the target
ABI. The contract below is identical regardless of the host operating system or silicon vendor.

## Step 1: Verify prerequisites

Provide a C99 compiler, CMake 3.16+, an AOSL `CONFIG_PLATFORM` port, and an Agora RTSA header and
library built for the exact target ABI. The device also needs TLS-capable connectivity to the compatible
device service, persistent credential storage, and 16 kHz mono signed 16-bit PCM I/O. The bundled
Agora library is x86_64 Linux only and cannot be reused for another architecture.

## Step 2: Create the platform layout

Keep platform code outside `src/`:

```text
platforms/my_mcu/
  CMakeLists.txt
  my_mcu_platform.c
  my_mcu_platform.h
  my_mcu_audio.c
  my_mcu_wifi.c
  my_mcu_https.c
  my_mcu_kv_store.c
  my_mcu_key.c
  my_mcu_lcd.c          # optional
  my_mcu_wake_words.c   # optional
```

An out-of-tree firmware project may use the same layout without changing this repository.

## Step 3: Implement required backends

Register every required backend exactly once before `mybot_start()`.

### Audio

Implement complete capture and playback tables from `mybot_audio.h`. The lifecycle is
`init -> start -> repeated read/write -> stop -> destroy`.

- The format is 16000 Hz, one channel, 16 bits per sample.
- Counts passed to and returned from `read` and `write` are frames, not bytes.
- Return a short positive count for progress, 0 for no progress, or a negative value on error.
  Never return more frames than requested.
- PCM pointers are borrowed only for the callback duration.
- I/O runs on dedicated AOSL MPQ workers. Blocking must be bounded so shutdown can finish.
- `stop` should unblock in-flight I/O; `destroy` releases the context after workers stop.

Register with `mybot_audio_register_capture()` and
`mybot_audio_register_playback()`.

#### Device volume (optional)

Volume control has two independent layers:

- **Media volume** is managed entirely by the SDK. `mybot_audio_set_media_volume()` stores a
  0..100 software gain that the playback pipeline applies digitally to downlink PCM before it
  reaches the device (linear amplitude, 100 = unity gain). No platform code is required.
- **Device volume** is an optional platform hook. Implement `mybot_audio_volume_ops_t` and call
  `mybot_audio_device_register_volume()` to route `mybot_audio_device_set_volume()` /
  `mybot_audio_device_get_volume()` to real hardware (codec register, amplifier, or mixer).
  `init`, `set_volume`, and `destroy` are required; `get_volume` is optional. The SDK
  initializes the backend during startup and releases it on shutdown; an init failure only
  disables device volume control — playback and media volume keep working.

### Wi-Fi provisioning

Implement `mybot_wifi_ops_t`. `init` starts APSTA without waiting for the user.
Emit connected, disconnected and failed events as state changes. Events may come from a platform
thread, but none may run after `destroy` returns. Destroy must stop the transport and wait for
in-flight callbacks. The backend must keep monitoring the STA link after the first successful
connection and report runtime disconnect and reconnect events; the SDK pauses device-service
traffic while offline and resumes it after reconnect. Register with
`mybot_wifi_register()`.

The Linux backend reports connected immediately and is not a real APSTA reference.

### Persistent key-value storage

Implement all callbacks in `mybot_kv_store_ops_t`.

- `get` returns 0 on success, `MYBOT_ERR_NOT_FOUND` when absent, and negative on failure.
  It must respect `capacity` and set `out_len` only on success.
- `set` should survive power loss without exposing a partial replacement.
- `erase` is idempotent.
- Protect the device token with appropriate access controls or encryption.

Register with `mybot_kv_store_register()`.

### HTTPS transport

Production builds keep `MYBOT_ENABLE_HTTPS=ON` and register one
`mybot_https_ops_t` before `mybot_start()`. Wrap mbedTLS, BearSSL, or the chipset TLS
socket API; the SDK core does not link OpenSSL. The backend must:

- establish TCP and TLS within the supplied timeout;
- send DNS hosts as SNI, validate the certificate chain against a maintained trust store, and
  verify the certificate hostname;
- return a positive byte count from `send` and `recv` on progress, 0 from `recv` only for a clean
  peer close, and -1 on error or timeout;
- release the entire TLS connection from `close`.

Register with `mybot_https_register()`. Do not disable certificate or hostname
verification for development certificates; install the required CA in the device trust store.
The Linux reference backend uses OpenSSL and the system CA store. Plain HTTP exists only for an
isolated development build configured with
`MYBOT_ENABLE_HTTPS=OFF -DMYBOT_ALLOW_INSECURE_HTTP=ON`.

### Keys

Implement `mybot_key_ops_t` and translate hardware input into conversation start, stop,
pair, exit, volume-up and volume-down events. The SDK adjusts media volume by 10 on volume
events; emitting them is optional. Events may be asynchronous. Destroy must stop the source and
wait for all handlers. Register with `mybot_key_register()`.

### LCD (optional)

Implement `mybot_lcd_ops_t` when a display exists. Render receives semantic content and can be
called from different SDK threads; the SDK serializes calls. Content is borrowed. Register with
`mybot_lcd_register()`.

### Wake words (optional)

Required only with `MYBOT_WAKE_WORDS=ON`. Process receives borrowed PCM. An asynchronous backend
must copy retained data, and destroy must wait for all detection handlers. Register with
`mybot_wake_words_register()`.

## Step 4: Add one registration entry point

```c
int my_mcu_platform_register(void) {
    if (my_mcu_wifi_register() < 0 || my_mcu_https_register() < 0 ||
        my_mcu_kv_register() < 0 ||
        my_mcu_key_register() < 0 || my_mcu_audio_capture_register() < 0 ||
        my_mcu_audio_playback_register() < 0) {
        return -1;
    }
    return 0;
}
```

Propagate every failure. Add LCD and wake-word registration when enabled.

## Step 5: Integrate with CMake

```cmake
set(CONFIG_PLATFORM my_mcu CACHE STRING "" FORCE)
set(AGORA_SDK_DIR /opt/agora-rtsa-target CACHE PATH "" FORCE)
set(AGORA_RTC_LIBRARY /opt/agora-rtsa-target/lib/libagora-rtc-sdk.a CACHE FILEPATH "" FORCE)
set(MYBOT_BUILD_LINUX_PLATFORM OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MYBOT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MYBOT_ENABLE_HTTPS ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/mybot)
add_library(my_mcu_platform STATIC ${MY_MCU_PLATFORM_SOURCES})
target_link_libraries(my_mcu_platform PUBLIC mybot::sdk)
target_link_libraries(device_firmware PRIVATE mybot::sdk my_mcu_platform)
```

Two independent variables select platform code: `CONFIG_PLATFORM` chooses the AOSL HAL port
consumed by `third_party/aosl` (e.g. `linux`, `esp32`); `MYBOT_BUILD_LINUX_PLATFORM` builds the
bundled Linux reference backends (`platforms/linux/`) and requires `CONFIG_PLATFORM=linux`. An MCU
port keeps `MYBOT_BUILD_LINUX_PLATFORM=OFF`. A host may predefine an imported target named
`agora-rtc-sdk`. Both source integration with `add_subdirectory()` and installed-package
consumption via `find_package(mybot)` are supported.

## Step 6: Start and stop

```c
if (my_mcu_platform_register() < 0) fail_startup();

mybot_config_t config = {0};
copy_checked(config.server_base, sizeof(config.server_base), server_url);
copy_checked(config.device_id, sizeof(config.device_id), device_id);
if (mybot_start(&config) < 0) fail_startup();

while (mybot_is_running()) platform_sleep_ms(100);
mybot_stop();
```

`server_base` must be an HTTPS URL and both fields must be non-empty NUL-terminated strings. Start
fails before global initialization when no TLS transport is registered. Start is non-blocking;
services continue after Wi-Fi reports connected. Do not call stop from a platform callback because
it waits for workers and callbacks. mybot currently owns process-wide AOSL and Agora lifecycles.

## Step 7: Cross-compile

```bash
cmake -S firmware -B build-target \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
  -DCONFIG_PLATFORM=my_mcu \
  -DAGORA_SDK_DIR=/opt/agora-rtsa-target \
  -DAGORA_RTC_LIBRARY=/opt/agora-rtsa-target/lib/libagora-rtc-sdk.a
cmake --build build-target -j
```

Verify endianness, pointer width, libc, compiler and floating-point ABI against the Agora library.

## Step 8: Acceptance checklist

- Public headers compile with warnings as errors and the host links only documented targets.
- HTTPS rejects an untrusted CA, expired certificate, wrong hostname, missing SNI and handshake timeout.
- Wi-Fi connected, disconnected and failure paths complete without deadlock.
- KV survives reset, handles not-found and overflow, and protects credentials.
- Capture/playback pass 16 kHz mono S16 tests at 20, 40 and 60 ms.
- Short I/O makes progress and stop unblocks device loss.
- No key or wake-word callback runs after destroy returns; LCD does not retain borrowed content.
- Partial startup failure and repeated start/stop release all resources.
- A real device completes provisioning, pairing, RTC join, bidirectional audio, hangup and reboot.
- Logs and storage do not expose tokens.

## Current RC limitations

- Public API and ABI may change before 1.0.
- HTTPS requires a platform TLS backend and maintained CA trust store. Linux supplies an OpenSSL
  reference; MCU ports must integrate their TLS stack.
- RTC is Agora-specific, registries are singleton, and one application instance is supported.
- Linux backends are development references, not production provisioning or secure storage.
- Third-party redistribution rights must be verified separately; see `THIRD_PARTY_NOTICES.md`.
