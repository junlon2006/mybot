# Porting mybot to a new platform

> [English](PORTING.md) | [简体中文](PORTING.zh-CN.md)

This document defines the cross-platform integration specification for mybot 1.0.0.
Public APIs and ABI follow
Semantic Versioning. Platform code must include only headers under `include/mybot` and link
`mybot::sdk`.

mybot is designed to be portable to virtually any platform — Linux, an RTOS, or bare metal — as
long as AOSL has a `CONFIG_PLATFORM` port for it and an Agora RTSA library exists for the target
ABI. The specification below is identical regardless of the host operating system or silicon vendor.

## Step 1: Verify prerequisites

Provide a C99 compiler, CMake 3.16+, an AOSL `CONFIG_PLATFORM` port, and an Agora RTSA header and
library built for the exact target ABI. When building from a git checkout, initialize the AOSL
submodule first with `git submodule update --init --recursive`. The device also needs
TLS-capable connectivity to the compatible device service, persistent credential storage, and
16 kHz mono signed 16-bit PCM I/O. The bundled
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
  my_mcu_announce.c     # optional (pairing-code voice announcement)
  my_mcu_wake_words.c   # optional
```

An out-of-tree firmware project may use the same layout without changing this repository.

## Reference port projects

The [mybot-esp32](https://github.com/junlon2006/mybot-esp32) repository provides an ESP32
cross-platform firmware reference. Use it to study AOSL platform integration, descriptor wiring,
and firmware lifecycle while keeping the SDK public contract and ownership boundaries defined here.

## Step 3: Implement the required platform operations

Implement the operations tables below and expose them through the platform descriptor in Step 4.

### Audio

Implement complete capture and playback tables from `mybot_audio.h`. The lifecycle is
`init -> start -> repeated read/write -> stop -> destroy`.

- The format is 16000 Hz, one channel, 16 bits per sample.
- Counts passed to and returned from `read` and `write` are frames, not bytes.
- Return a short positive count for progress, 0 for no progress, or a negative value on error.
  Never return more frames than requested.
- PCM pointers are borrowed only for the callback duration.
- I/O runs on dedicated AOSL MPQ workers. Blocking must be bounded so shutdown can finish.
- The SDK calls capture and playback `stop` from its shutdown thread before waiting for either
  worker. `stop` must be thread-safe and promptly unblock an in-flight `read` / `write`;
  `destroy` runs only after the corresponding worker has exited.

Add both operations tables to the platform descriptor.

#### Device volume (optional)

The SDK owns volume control and exposes no application-facing volume API; volume changes (for
example from volume key events) take one of two paths:

- **Device volume** is the primary path. Implement `mybot_audio_volume_ops_t` and add it to the
  platform descriptor to route the SDK's volume changes to real hardware (codec register,
  amplifier, or mixer). `init`, `set_volume`, and `destroy` are required;
  `get_volume` is optional and used only to sync the SDK's volume state. The SDK initializes
  the implementation during startup and releases it on shutdown.
- **Media volume** is the fallback used when no device volume implementation is active. The SDK keeps a
  0..100 software gain and the playback pipeline applies it digitally to downlink PCM before it
  reaches the device (linear amplitude, 100 = unity gain). No platform code is required.

An init failure only disables device volume control; the SDK falls back to the software gain and
playback keeps working.

### Wi-Fi provisioning

Implement `mybot_wifi_ops_t`. `init` starts APSTA without waiting for the user.
Emit connected, disconnected and failed events only on connectivity transitions. Emit connected
only after the STA has an IP address and the network is ready for SDK traffic. Events may come from
platform threads, but must be delivered serially and none may run after `destroy` returns. Destroy
must stop the transport and wait for in-flight callbacks. The implementation must keep monitoring
network connectivity after the first successful connection and report runtime disconnect and
reconnect events; the SDK pauses device-service traffic while offline and resumes it after
reconnect. Add the operations table to the platform descriptor.

The Linux reference implementation reports connected immediately and is not a real APSTA reference.

### Persistent key-value storage

Implement all callbacks in `mybot_kv_store_ops_t`.

- `get` returns 0 on success, `MYBOT_ERR_NOT_FOUND` when absent, and negative on failure.
  It must respect `capacity` and set `out_len` only on success.
- `set` should survive power loss without exposing a partial replacement.
- `erase` is idempotent.
- Protect the device token with appropriate access controls or encryption.

Add the operations table to the platform descriptor.

### HTTPS transport

Production builds keep `MYBOT_ENABLE_HTTPS=ON` and provide one `mybot_https_ops_t`. Wrap mbedTLS,
BearSSL, or the chipset TLS
socket API; the SDK core does not link OpenSSL. The implementation must:

- establish TCP and TLS within the supplied timeout;
- send DNS hosts as SNI, validate the certificate chain against a maintained trust store, and
  verify the certificate hostname;
- return a positive byte count from `send` and `recv` on progress, 0 from `recv` only for a clean
  peer close, and -1 on error or timeout;
- release the entire TLS connection from `close`.

Add the operations table to the platform descriptor. Do not disable certificate or hostname
verification for development certificates; install the required CA in the device trust store.
The Linux reference implementation uses OpenSSL and the system CA store. Plain HTTP exists only for an
isolated development build configured with
`MYBOT_ENABLE_HTTPS=OFF -DMYBOT_ALLOW_INSECURE_HTTP=ON`.

### Keys

Implement `mybot_key_ops_t` and translate hardware input into conversation start, stop,
pair, exit, volume-up and volume-down events. The SDK adjusts volume by 10 on volume events —
real hardware volume when a device volume implementation is active, otherwise the media-volume software
gain; emitting volume events is optional. Events may be asynchronous. Destroy must stop the
source and wait for all handlers. Add the operations table to the platform descriptor.

For a single hardware toggle button, query the thread-safe `mybot_get_state()` when handling the
button: emit `MYBOT_KEY_EVENT_CONVERSATION_START` only from `MYBOT_STATE_READY`, and emit
`MYBOT_KEY_EVENT_CONVERSATION_STOP` only from `MYBOT_STATE_IN_CONVERSATION`. Ignore the toggle in
provisioning, startup, disconnected, failed, and stopping states. Do not infer conversation state
from the LCD or maintain a second platform-side state; runtime connectivity loss is reported as
`MYBOT_STATE_WIFI_DISCONNECTED` and the SDK ends the conversation locally.

### LCD (optional)

Implement `mybot_lcd_ops_t` when a display exists. Render receives semantic content from the
application control owner; the SDK does not invoke it concurrently. Content is borrowed. Add the
operations table to the platform descriptor.

`mybot_lcd_content_t.indicators` is a bitmask for non-exclusive overlays. During
`MYBOT_LCD_SCREEN_IN_CONVERSATION`, `MYBOT_LCD_INDICATOR_VP_REGISTERED` means the server completed
voice-print registration for the active conversation. Render this as an in-conversation indicator;
ignore bits the platform does not recognize and do not derive lifecycle state from the indicator.

### Wake words (optional)

Required only with `MYBOT_WAKE_WORDS=ON`. Process receives borrowed PCM. An asynchronous implementation
must copy retained data, and destroy must wait for all detection handlers. Add the operations table
to the platform descriptor.

### Pairing-code voice announcement (optional)

Implement `mybot_announce_ops_t` to play the pairing-code prompt on the speaker ("Please enter the
pairing code xxx in the console"). All PCM exchanged with the SDK is raw 16 kHz mono signed 16-bit — the SDK contains
no audio decoder, so the platform must decode/resample its own assets to that format.

When a pair code is obtained, the SDK queues the fixed prompt sound followed by one sound per
digit and plays the queue **once** through the normal playback path. The announcement stops when
the device leaves `awaiting_claim` (claimed, re-pairing, or offline). A missing prompt sound
skips the whole announcement; a missing digit sound skips just that digit — pairing never blocks
on the audio.

Ops interface: `init` allocates the implementation; `open` opens one logical sound
(`MYBOT_ANNOUNCE_SOUND_PROMPT`, `MYBOT_ANNOUNCE_SOUND_DIGIT_0`..`9`) and may do I/O; `read`
copies up to `max_frames` frames and must stay cheap (it runs on the real-time playback worker);
`close` / `destroy` release handles and the implementation. Add the operations table to the
platform descriptor. The implementation is optional: without one the SDK skips local announcements
and only logs.

The Linux reference implementation reads raw PCM files per locale from
`./assets/locales/<locale>/` (`prompt.pcm`, `0.pcm`..`9.pcm`); override the directory with the
`MYBOT_ASSETS_DIR` environment variable (default `./assets`) and the locale with `MYBOT_LOCALE`
(default `zh-CN`).

## Step 4: Register one platform descriptor

```c
#include <mybot/platform/mybot_platform.h>

static const mybot_platform_descriptor_t my_mcu_platform = {
    .wifi = &my_mcu_wifi_ops,
    .kv_store = &my_mcu_kv_ops,
    .key = &my_mcu_key_ops,
    .audio_capture = &my_mcu_capture_ops,
    .audio_playback = &my_mcu_playback_ops,
    .https = &my_mcu_https_ops,
    .lcd = &my_mcu_lcd_ops,
};

int my_mcu_platform_register(void) {
    return mybot_platform_register(&my_mcu_platform);
}
```

A non-NULL ops pointer is the sole declaration that the platform supports that function; leave an
optional pointer NULL when it is unavailable. `mybot_platform_register()` validates every required
table and every provided optional table before committing the complete descriptor, so a failure
cannot leave a partially registered platform. It is the only platform registration entry point.
`mybot_start()` separately checks the ops required by the active build and runtime configuration,
such as HTTPS for an HTTPS server URL and wake words when enabled.

## Step 5: Integrate with CMake

```cmake
set(CONFIG_PLATFORM my_mcu CACHE STRING "" FORCE)
set(AGORA_SDK_DIR /opt/agora-rtsa-target CACHE PATH "" FORCE)
set(AGORA_RTC_LIBRARY /opt/agora-rtsa-target/lib/libagora-rtc-sdk.so CACHE FILEPATH "" FORCE)
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
bundled Linux reference implementations (`platforms/linux/`) and requires `CONFIG_PLATFORM=linux`. An MCU
port keeps `MYBOT_BUILD_LINUX_PLATFORM=OFF`. A host may predefine an imported target named
`agora-rtc-sdk`. Both source integration with `add_subdirectory()` and installed-package
consumption via `find_package(mybot)` are supported. Source integration must initialize mybot's
nested AOSL submodule (`git submodule update --init --recursive` in the mybot checkout).

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
checks the registered descriptor against the active configuration and fails before global
initialization when a required ops table, such as the TLS transport, is absent. Start is non-blocking;
services continue after Wi-Fi reports usable network connectivity. Do not call stop from a platform callback because
it waits for workers and callbacks. `mybot_start()` acquires one application reference to the
process-wide AOSL runtime and `mybot_stop()` releases that reference last, after workers, buffers
and the RTC callback queue have been torn down. The RTSA lifecycle is managed through
`agora_rtc_init()` / `agora_rtc_fini()`. A host that uses AOSL directly must pair its own
`aosl_ctor()` and `aosl_dtor()` calls and keep that reference until all of its AOSL users have stopped.

`mybot_get_state()` is the thread-safe application-level state query. It reports
`MYBOT_STATE_IN_CONVERSATION` after the device service accepts a conversation and returns to
`MYBOT_STATE_READY` after normal teardown. If runtime connectivity is lost,
`MYBOT_STATE_WIFI_DISCONNECTED` takes precedence until reconnect. The device-service lifecycle
states (`unprovisioned`, `pairing`, `awaiting_claim`, `runtime`, and `in_conversation`) are an
internal state machine and must not be reconstructed by the platform.

### RTM account mapping

The Agora RTM integration follows the xiaozhi reference flow. The device-service conversation
response supplies a server-generated `rtc.uid` (also called `local_uid`) and, for point-to-point
messages, a top-level `agent_uid`. The SDK uses `rtc.uid` unchanged as both the RTC user account
and the local RTM UID, and uses the same server-issued RTC token for the RTM login request. The
`agent_uid` value is the peer passed to `mybot_agora_rtc_send_rtm_data()`; the `AG-*` device ID is
only the device-service identity and must not be substituted for either account.

An RTM UID must be non-empty, shorter than 64 bytes, and contain only ASCII letters, digits, space,
or the punctuation characters accepted by Agora. `mybot_agora_rtc_rtm_uid_is_valid()` exposes the
same validation used before login. RTM login is asynchronous: a send is allowed only after the
`MYBOT_RTM_EVENT_LOGIN` callback reports error code 0. RTM payloads are limited to 31 KiB and
custom types to 32 bytes.

Conversation startup requests RTM login first and waits up to five seconds for a successful
`MYBOT_RTM_EVENT_LOGIN` callback. The SDK does not create or join the RTC connection until RTM login
has succeeded, so platforms must allow the RTM callback to run while the control owner is waiting.

## Step 7: Cross-compile

```bash
cmake -S firmware -B build-target \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
  -DCONFIG_PLATFORM=my_mcu \
  -DAGORA_SDK_DIR=/opt/agora-rtsa-target \
  -DAGORA_RTC_LIBRARY=/opt/agora-rtsa-target/lib/libagora-rtc-sdk.so
cmake --build build-target -j
```

`AGORA_RTC_LIBRARY` accepts a shared or static library. Verify endianness, pointer width, libc,
compiler and floating-point ABI against the selected Agora library. For a shared target package,
also deploy the library and configure the firmware or OS runtime loader to find it.

## Step 8: Acceptance checklist

- Public headers compile with warnings as errors and the host links only documented targets.
- HTTPS rejects an untrusted CA, expired certificate, wrong hostname, missing SNI and handshake timeout.
- Wi-Fi connected, disconnected and failure paths complete without deadlock.
- KV survives reset, handles not-found and overflow, and protects credentials.
- Capture/playback pass 16 kHz mono S16 tests at 20, 40 and 60 ms.
- Short I/O makes progress and stop unblocks device loss.
- No key or wake-word callback runs after destroy returns; LCD does not retain borrowed content.
- Partial startup failure and repeated start/stop release all resources.
- `mybot_get_state()` reports `READY -> IN_CONVERSATION -> READY` for a normal conversation;
  a conversation interrupted by Wi-Fi loss reports `WIFI_DISCONNECTED` and returns to `READY` after
  reconnect, without deadlock during stop or re-pair.
- A real device completes provisioning, pairing, RTC join, bidirectional audio, hangup and reboot.
- Logs and storage do not expose tokens.

## Known limitations

- HTTPS requires a platform TLS implementation and maintained CA trust store. Linux supplies an OpenSSL
  reference; MCU ports must integrate their TLS stack.
- RTC is Agora-specific, platform registration is process-wide, and one application instance is
  supported.
- Linux reference implementations are development references, not production provisioning or secure storage.
- Third-party redistribution rights must be verified separately; see `THIRD_PARTY_NOTICES.md`.
