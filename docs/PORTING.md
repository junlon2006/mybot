# Porting mybot to a new platform

> [English](PORTING.md) | [简体中文](PORTING.zh-CN.md)

This guide describes how to integrate the current mybot release into a product. Public API contracts,
including ownership and callback threading, are defined in
[mybot.h](../include/mybot/mybot.h) and the
[platform headers](../include/mybot/platform/). The public source API follows Semantic Versioning.
Binary ABI compatibility is not guaranteed across releases, including when public structures gain
fields. Build the SDK and platform adapters against the same selected headers; do not mix
precompiled components built against different mybot header versions.
Platform code must include only headers under `include/mybot` and link `mybot::sdk`.

mybot is designed to be portable to virtually any platform — Linux, an RTOS, or bare metal — as
long as AOSL has a `CONFIG_PLATFORM` port for it and an Agora RTSA library exists for the target
ABI. The integration flow below applies regardless of the host operating system or silicon vendor.

## Step 1: Verify prerequisites

Provide a C99 compiler, CMake 3.16+, an AOSL `CONFIG_PLATFORM` port, and an Agora RTSA header and
library built for the exact target ABI. When building from a git checkout, initialize the AOSL
submodule first with `git submodule update --init --recursive`. The device also needs
TLS-capable connectivity to the compatible device service, persistent credential storage, and
16 kHz mono signed 16-bit PCM I/O. The bundled
Agora library is x86_64 Linux only and cannot be reused for another architecture.

## Step 2: Create the platform layout

Implement MCU platform code in its independent firmware repository. A suggested layout there is:

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
  my_mcu_video.c        # optional (encoded video uplink)
  my_mcu_wake_words.c   # optional
```

Integrate the SDK through its public platform operations without copying board-specific sources
or resources into this repository.

## Reference port projects

The following independent repositories maintain the MCU adapters, board configuration, assets,
and firmware builds. Use them directly for device integration; this SDK repository keeps the Linux
reference implementation without duplicate MCU source trees.

- [mybot-bk7258](https://github.com/junlon2006/mybot-bk7258) for BK7258;
- [mybot-bk7259](https://github.com/junlon2006/mybot-bk7259) for BK7259;
- [mybot-esp32](https://github.com/junlon2006/mybot-esp32) for ESP32.

Use them to study target build integration, AOSL platform wiring, descriptor registration, and
firmware lifecycle. When a reference project differs, use the SDK public headers as the authority
for interface contracts and ownership boundaries.

## Step 3: Implement the required platform operations

Implement the operations tables below and expose them through the platform descriptor in Step 4.
Use each public header for the complete callback, return-value, and lifetime contract.

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
  `get_volume` is optional. When provided, the SDK calls it after initialization to sync its
  volume state and before every relative adjustment, including volume-key events and
  `audio.adjust_volume` commands. If it fails or reports an out-of-range value, that adjustment
  fails. The SDK initializes the implementation during startup and releases it on shutdown.
- **Media volume** is the fallback used when no device volume implementation is active. The SDK keeps a
  0..100 software gain and the playback pipeline applies it digitally to downlink PCM before it
  reaches the device (linear amplitude, 100 = unity gain). No platform code is required.

An init failure only disables device volume control; the SDK falls back to the software gain and
playback keeps working.

### Network connectivity

Implement [mybot_wifi_ops_t](../include/mybot/platform/mybot_wifi.h) as a listener on the
product's existing network manager and add it to the platform descriptor. Product provisioning,
credential storage, and connection establishment are independent of MyBot. `init` attaches the SDK
listener and reports the current usable connection; `destroy` detaches that listener without
shutting down the product's network. Follow the header for event ordering and callback lifetime.

Keep reporting runtime link loss and recovery while MyBot is running. The SDK pauses device-service
requests while offline and resumes them after reconnection; a lost conversation is ended locally.
Entering provisioning is a separate product action and follows the stop/start sequence in Step 6.
The Linux adapter assumes the host network is already available and reports connected immediately;
it does not implement provisioning or monitor host link changes.

### Persistent key-value storage

Implement all callbacks in `mybot_kv_store_ops_t`.

- `get` returns 0 on success, `MYBOT_ERR_NOT_FOUND` when absent, and negative on failure.
  It must respect `capacity` and set `out_len` only on success.
  A read failure during credential restoration fails service startup and preserves the record.
  Once storage recovers, stop and restart MyBot to restore the binding.
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

Control requests and background device-service requests can use two connections concurrently.
Give each `connect()` an independent context and support calls on different connections from
different workers. The SDK uses each handle sequentially; it does not issue concurrent operations
on the same connection. Include any shared TLS state in the adapter's thread-safety design.
The Linux reference creates a separate SSL context, TLS session, and socket for each connection;
verify the equivalent isolation in the target TLS implementation.

Add the operations table to the platform descriptor. Do not disable certificate or hostname
verification for development certificates; install the required CA in the device trust store.

The SDK's HTTP receive buffer grows to at most 32 KiB and reserves one byte for its terminating
NUL, leaving 32767 bytes for the raw response, including HTTP headers and any chunk framing.
The client completes a chunked response when it receives the zero-size chunk and the empty line
ending its trailers. Only a close-delimited body requires a clean peer close.
The 2 KiB free-space threshold only triggers buffer growth; remaining space at the allocation
limit is still usable regardless of how the response is split across receive calls. A full buffer
that still needs an end-of-stream check uses a one-byte probe under the existing request deadline
to detect closure or overflow; receive calls never use zero capacity.

The Linux reference implementation uses OpenSSL and the system CA store. Plain HTTP exists only for an
isolated development build configured with
`MYBOT_ENABLE_HTTPS=OFF -DMYBOT_ALLOW_INSECURE_HTTP=ON`.
OpenSSL support is compiled into the Linux reference platform exactly when `MYBOT_ENABLE_HTTPS=ON`.

### Keys

Implement `mybot_key_ops_t` and translate hardware input into conversation start, stop,
pair, exit, volume-up and volume-down events. The SDK adjusts volume by 10 on volume events —
real hardware volume when a device volume implementation is active, otherwise the media-volume software
gain; emitting volume events is optional. Events may be asynchronous. Destroy must stop the
source and wait for all handlers. Add the operations table to the platform descriptor.

For a single hardware toggle button, query the thread-safe `mybot_get_state()` when handling the
button: emit `MYBOT_KEY_EVENT_CONVERSATION_START` only from `MYBOT_STATE_READY`, and emit
`MYBOT_KEY_EVENT_CONVERSATION_STOP` only from `MYBOT_STATE_IN_CONVERSATION`. Ignore the toggle in
pairing, initial network confirmation (`MYBOT_STATE_WIFI_PROVISIONING`), startup, disconnected,
failed, and stopping states. `MYBOT_STATE_PAIRING`
covers the device-service `unprovisioned`, `pairing`, and `awaiting_claim` phases. Do not infer
conversation state from the LCD or maintain a second platform-side state; runtime connectivity
loss is reported as `MYBOT_STATE_WIFI_DISCONNECTED` and the SDK ends the conversation locally.

### LCD (optional)

If Wi-Fi reconnects while the device is waiting to be claimed, the SDK renders the current pairing
code again. The LCD adapter can draw each screen from the supplied content.

Implement `mybot_lcd_ops_t` when a display exists. Render receives semantic content from the
application control owner; the SDK does not invoke it concurrently. Content is borrowed. Add the
operations table to the platform descriptor.

`mybot_lcd_content_t.indicators` is a bitmask for conversation overlays. During
`MYBOT_LCD_SCREEN_IN_CONVERSATION`, `MYBOT_LCD_INDICATOR_VP_REGISTERED` means the server completed
voice-print registration for the active conversation. Render this as an in-conversation indicator;
ignore bits the platform does not recognize and do not derive lifecycle state from the indicator.
A platform may show its own pending/unregistered marker while this bit is clear, but the marker is
only presentation state and must not change the SDK lifecycle. The
`MYBOT_LCD_INDICATOR_LISTENING`, `MYBOT_LCD_INDICATOR_THINKING`, and
`MYBOT_LCD_INDICATOR_SPEAKING` bits expose the server's current conversation phase; these three bits
are mutually exclusive.
Copy content needed by asynchronous rendering before returning, and wait for that work before
`destroy()` releases the context. See
[mybot_lcd.h](../include/mybot/platform/mybot_lcd.h) for the interface contract.

### Wake words (optional)

Required only with `MYBOT_WAKE_WORDS=ON`. Process receives borrowed PCM. An asynchronous implementation
must copy retained data, and destroy must wait for all detection handlers. Add the operations table
to the platform descriptor.

### Pairing-code voice announcement (optional)

Implement `mybot_announce_ops_t` to play the pairing-code prompt on the speaker ("Please enter the
pairing code xxx in the console"). All PCM exchanged with the SDK is raw 16 kHz mono signed 16-bit — the SDK contains
no audio decoder, so the platform must decode/resample its own assets to that format.

When a pair code is obtained, the SDK queues the fixed prompt sound followed by one sound per
digit and plays the queue **once** through the normal playback path. Prompt PCM is selected by the
playback worker and kept separate from the RTC playback ring; a final partial prompt frame is
zero-padded. RTC downlink data is discarded while the announcement is active. The announcement stops when
the device leaves `awaiting_claim` (claimed, re-pairing, or offline). A missing prompt sound
skips the new announcement; a missing digit sound skips just that digit — pairing never blocks
on the audio.

A replacement becomes current only after its assets finish loading and the SDK swaps the sound
source; the previous prompt can continue during loading and remains current if loading fails.
Replacement and stop discard stale prompt PCM buffered in the SDK, including any remainder from a
short playback write. A device write authorized before the replacement or stop may still finish;
PCM already accepted by the device cannot be withdrawn.

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

### Video uplink (optional)

When `MYBOT_ENABLE_VIDEO=ON`, implement `mybot_video_ops_t` and add it to the platform descriptor.
The platform owns camera capture and JPEG, H.264, or H.265 encoding; the SDK does not encode,
decode, or receive server video. Set `min_bps` and `max_bps` in the operations table to the
encoder's supported video bitrate range. The SDK validates that range and applies it with
`agora_rtc_set_bwe_param()` after creating the RTC connection; `start_bps` is the midpoint.

The encoder pushes complete encoded frames through the handler registered by `init()`. Frame data is
borrowed until the handler returns and the handler must run from a task context, never an ISR.
`stop()` must stop the encoder and wait for all in-flight handlers before `destroy()` is called.
The SDK serializes all video operations on `control_mpq`, including bitrate and key-frame requests.
RTC callbacks enqueue these control notifications; notifications from ended conversations are discarded.
Encoded frame submission remains on the encoder task.

Video starts only after RTC reaches `CONNECTED` and stops before the session leaves RTC. The SDK has
no video ring buffer; a failed send drops that frame. RTSA packetizes and copies the input before
`agora_rtc_send_video_data()` returns, while the platform must keep each frame within
`MYBOT_VIDEO_MAX_FRAME_BYTES`.
H.264/H.265 frames must be complete Annex-B access units accepted by the RTSA packetizer; JPEG
frames must be complete JPEG images.

`on_target_bitrate_changed()` receives RTSA's current uplink target in bits per second; the encoder
should adjust its bitrate within its supported range and return promptly. `on_key_frame_request()` is
optional and handles RTSA key-frame requests. MyBot sends one primary stream and explicitly disables
remote video subscription.

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
such as HTTPS for an HTTPS server URL, wake words when enabled, and video (including its bitrate
callback) when enabled.

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
The installed package provides CMake package files only; pkg-config metadata is not installed.

## Step 6: Start and stop

Run provisioning and SDK lifecycle control as separate product responsibilities:

1. The product connects using saved credentials or completes its provisioning flow, then signals
   that the network is usable to the product control task.
2. The product control task calls `mybot_start()`. The Wi-Fi adapter reports the existing connection,
   allowing MyBot to start device-service pairing, authentication, and conversations.
3. Before entering provisioning again, the product control task calls `mybot_stop()` and waits
   for it to finish before handing shared devices to the provisioning flow. Start MyBot again
   after usable connectivity returns. Provisioning prompts belong to that product flow;
   SDK pairing-code announcements belong to MyBot.

For the lifecycle API contract, see [mybot.h](../include/mybot/mybot.h). The following example runs
on the product control task after network connectivity is ready:

```c
if (my_mcu_platform_register() < 0) fail_startup();

mybot_config_t config = {0};
copy_checked(config.server_base, sizeof(config.server_base), server_url);
copy_checked(config.device_id, sizeof(config.device_id), device_id);
if (mybot_start(&config) < 0) fail_startup();

while (mybot_is_running()) platform_sleep_ms(100);
mybot_stop();
```

Use the deployment-supplied device-service base URL as `server_base`. The SDK validates it and
appends the request paths listed in [Device-service compatibility](#device-service-compatibility).
Generate `device_id` in the product layer according to
[Device ID generation](#device-id-generation), keeping both fields NUL-terminated.
`mybot_start()` sets up the SDK control worker and network listener; device services
start after the listener's first connected event. The historical `MYBOT_STATE_WIFI_PROVISIONING` and
`MYBOT_LCD_SCREEN_WIFI_PROVISIONING` names identify this initial network-confirmation stage;
they do not mean the SDK is running provisioning. Do not call stop from a platform callback
because it waits for workers and callbacks. `mybot_start()` acquires one application reference to the
process-wide AOSL runtime and `mybot_stop()` releases that reference last, after workers, buffers
and the RTC callback queues have been torn down. MyBot serializes RTSA lifecycle, state, and
vendor callback dispatch on a dedicated `rtc_mpq`; application callbacks must not re-enter the RTC
API. The RTSA lifecycle is managed through
`agora_rtc_init()` / `agora_rtc_fini()`. A host that uses AOSL directly must pair its own
`aosl_ctor()` and `aosl_dtor()` calls and keep that reference until all of its AOSL users have stopped.

If stopping device I/O or joining a worker fails, the SDK retains the resources and reports
`MYBOT_STATE_STOPPING`. Call `mybot_stop()` again from the product control thread to complete
cleanup before restarting.

Use `mybot_get_state()` to decide which product actions are currently available; the returned
states are defined in [mybot.h](../include/mybot/mybot.h). Do not reconstruct the SDK's private
device-service state machine in the platform.

Control-event submission does not wait for free queue capacity. If the 1000-call control queue
is full, RTM LCD indicators and video bitrate/key-frame notifications are logged and discarded.
Failure to queue a key, Wi-Fi, wake-word, or video-start event publishes an exit request:
`mybot_is_running()` becomes false, and the product control task must call `mybot_stop()`.
The callback does not tear down resources.

If allocation or queue admission fails for a critical RTC connection or token notification, the
next control tick ends that conversation through the normal local teardown path.

For an ordinary temporary network outage, keep MyBot running and let the network manager reconnect
with its existing credentials. The adapter reports the disconnect and subsequent usable connection;
do not turn every link loss into a provisioning restart. The SDK ends the interrupted conversation
and resumes device-service work after reconnect; starting a new conversation requires a new trigger.

Re-pairing also discards pending conversation-start requests, including requests received after
the pair request but before the next control tick. Once the new binding is claimed, starting a
conversation requires a fresh trigger, even if the pair-code request needed retries.

The application control worker owns lifecycle state, keys, LCD, RTC, and media operations. A separate
`service_mpq` performs pairing/runtime binding-status polling and HTTPS stop notifications, then
returns polling results to the control worker. At most one poll is outstanding, so slow responses
do not accumulate polling requests. Each request owns a snapshot of its address and credentials;
the control worker discards results from an earlier network or binding generation.

Conversation teardown first stops local video and RTC, then flushes session PCM. An online hangup
returns to `MYBOT_STATE_READY` before queuing one HTTPS stop notification with the original reason.
The control worker does not wait for this notification, so a slow background poll or stop request
does not delay the next conversation trigger. The notification is not retried on failure, so a
compatible device service must independently end the remote conversation when the device leaves
the RTC channel. Offline conversations end locally without an HTTPS stop notification.

Pair-code, conversation-start, and RTC-token renewal requests still run synchronously on the control
worker, as do the RTM login/subscription waits described below. Their own latency can delay later
control work. Shutdown stops accepting background work and results and waits for `service_mpq`
before releasing request state and the AOSL reference; an in-flight HTTPS request can still delay
`mybot_stop()`. There is no transport cancellation API.

Re-pairing uses the same local-first teardown before clearing the device credential and requesting
a new pair code. Pending lifecycle stop and RTC-token renewal flags for the old conversation are
cleared; an already queued HTTPS stop notification retains its original credential snapshot.
A rejected device credential still restarts pairing; pair-code and RTC-token renewal retries are
unaffected by the single-attempt stop policy.

### Device-service compatibility

The device service is external to this repository. The SDK appends these paths to
`mybot_config_t.server_base` and consumes only the fields listed below:

| Request | SDK behavior and consumed response fields |
| --- | --- |
| `POST /devices/pair-codes` | Unauthenticated; sends `device_id`, optional `firmware_version` and `hardware_model`; requires `data.code` and `data.pair_token`, with optional `data.poll_after_seconds`. |
| `GET /devices/{device_id}/binding-status` | Uses `Pair` or `Device` authorization; consumes `data.status` and, when present, `data.device_token`, `data.agent_id`, `data.agent_name`, and `data.poll_after_seconds`. |
| `POST /devices/{device_id}/conversations/start` | Uses `Device` authorization; sends `trigger: "button"`, the configured audio `p_time` and G722 codec, and configured feature flags; requires `data.conversation_id`, top-level `agent_uid`, and `data.rtc.app_id`, `channel`, and `uid`. `rtc.token` may be empty for a no-auth channel. |
| `POST /devices/{device_id}/rtc-token` | Uses `Device` authorization; sends `channel` and `local_uid`; requires `data.rtc.channel`, `uid`, and `token`. |
| `POST /devices/{device_id}/conversations/stop` | Uses `Device` authorization; sends `conversation_id` and `reason`; success requires a 2xx HTTP status. |

For authenticated operations, HTTP 401, 403, and 409 responses are treated as
device-authentication rejection. Other non-2xx responses fail the current request; retry behavior
depends on the operation. The SDK does not parse server error-body codes.

### Device ID generation

`mybot_config_t.device_id` is the globally unique identity of one physical device. Different
devices must have different IDs, and a device must retain its ID across reboots and OTA updates.
The product supplies this ID; the SDK does not read the MAC or derive it.
Use `<chip>-<24 uppercase hexadecimal characters>`.

The chip prefix separates ID namespaces across chip platforms. Assign each platform a distinct,
fixed prefix, such as `ESP32`, `BK7258`, or `BK7259`; different platforms must not share a prefix.
Even when their MAC addresses or digest suffixes match, the complete IDs remain different.

Compute HMAC-SHA256 over a stable device identifier with a fixed platform derivation key, encode
the first 12 digest bytes as 24 uppercase hexadecimal characters, and add the chip prefix and one
`-`. A separate output buffer must hold at least 33 bytes; generation must fail rather than truncate.
Keep the prefix, derivation key, and input identifier unchanged across reboots and OTA updates.
The truncated digest is not a proof of collision-free uniqueness, so the product and registration
system must reject conflicting IDs.
The existing SDK buffer remains `device_id[64]`; its startup validation checks non-empty,
NUL-terminated input and does not enforce this product-side 32-character generation rule.

### RTM account mapping

The device-service conversation response supplies a server-generated `rtc.uid` (also called
`local_uid`) and, for point-to-point messages, a top-level `agent_uid`. The SDK uses `rtc.uid`
unchanged as both the RTC user account and the local RTM UID. RTM token authentication uses a
separate RTM credential; the current service response exposes `rtc.token`, so deployments must
confirm that the service/app authentication mode accepts it for RTM or provide an RTM token through
an extended service contract. The `agent_uid` value identifies the agent allowed to deliver
commands and indicators; the device ID is only the device-service identity and must not be
substituted for either account.

An RTM UID must be non-empty, shorter than 64 bytes, and contain only ASCII letters, digits, space,
or the punctuation characters accepted by Agora. The internal RTC wrapper applies this validation
before login; no public RTM UID-validation API is exported. RTM login is asynchronous, and the SDK
processes inbound messages only after login succeeds. The SDK does not provide an outbound RTM
publish API. Inbound payloads are limited to 31 KiB and custom types to 32 bytes.
The device-service client rejects response strings that exceed their destination buffers; RTC token
storage reserves space for 512 bytes of content plus the terminating NUL.

Conversation startup requests RTM login first and waits up to five seconds for login success. It
then subscribes to the RTM channel whose name matches the RTC channel and waits up to five seconds
for subscription success. The SDK does not create or join the RTC connection until both operations
succeed, so platforms must allow RTM callbacks to run while the control owner is waiting. Voiceprint
status accepts only a channel message whose exact fields are `"object":"message.sal_status"` and
`"status":"VP_REGISTER_SUCCESS"`, from the current agent on the current conversation channel. It
also accepts `state.listening`, `state.thinking`, and `state.speaking` messages with a boolean
`payload.value` and exposes them as mutually exclusive LCD indicators. The SDK resets these
indicators at conversation boundaries and exposes success through
`MYBOT_LCD_INDICATOR_VP_REGISTERED`.

The P2P RTM receive path is internal to the RTC wrapper and is used for supported inbound messages.
The SDK does not expose RTM login, P2P receive, or subscribed-channel callbacks as public
application APIs.

### MCP device commands

MCP tool invocation and cloud authentication belong to the server. The SDK consumes only the
`audio.set_volume` and `audio.adjust_volume` DeviceCommand v1 actions from a single cloud envelope
with exact fields
`object: "message.user"`, `data_type: "message"`, and a string `content` containing the command JSON.
These three envelope fields must be unique; other envelope metadata is ignored. Bare commands
and nested envelopes are rejected. The inner command supplies the IDs used for session validation
and deduplication; the outer `message_id` is transport metadata. Both P2P and subscribed-channel
delivery share one parser; commands execute on `control_mpq`.

The complete RTM message, including any envelope and JSON escaping, is limited to 1024 bytes;
inner `message_id`, `command_id`, and `conversation_id` must each be non-empty and at most 127 bytes.
Unknown or duplicate fields, unsupported commands, and invalid
parameter types in the DeviceCommand are rejected. There is no SDK MCP network client, command ACK,
or execution retry.

The object requires `type: "device_command"`, `schema: "botstation.device-command"`, `version: 1`,
the three IDs, and a `command` object with `type`, `name`, and `params`. The `issued_at` and
`expires_at` fields may be omitted or included; the SDK currently ignores them and does not check
command validity periods or require clock synchronization.

| Command type | Name | Params |
| --- | --- | --- |
| `action` | `audio.set_volume` | Integer `level: 0..100` |
| `action` | `audio.adjust_volume` | Integer `delta: -100..100`; positive raises volume, negative lowers it |

Both commands reuse the key-volume path: active hardware volume takes priority, otherwise the
SDK applies software gain. Adjustments add `delta` to the current volume and clamp the result to
0..100; zero leaves the volume unchanged. For example, `delta: -10` changes 50 to 40 and
`delta: 10` changes 50 to 60. Duplicate command IDs do not apply the adjustment again while they
remain in the cache described below. Other MCP actions and scenes are unsupported.

At execution, the SDK matches the RTM sender, channel when present, and body conversation ID exactly
against the active session. A per-conversation FIFO cache retains the 16 most recently accepted
command IDs. A duplicate still in the cache is ignored without refreshing its position; a new ID
replaces the oldest entry when the cache is full. An ID outside this window may execute again.
The cache is cleared at conversation boundaries.

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

The selected `MYBOT_AUDIO_PTIME_MS` must match the RTSA package's
`CONFIG_MINIMAL_TIMER_INTERVAL_MS`. The bundled x86_64 Linux package is fixed at 60 ms; for 20 or
40 ms, provide a matching package through `AGORA_SDK_DIR` and `AGORA_RTC_LIBRARY`. CMake reads
`.config` or `include/global_config.cmake` and fails configuration when the values differ or the
metadata is absent. If the host predefines the `agora-rtc-sdk` target, `AGORA_SDK_DIR` must still
identify that same package for validation. CMake cannot inspect the imported target's ABI, so the
host must ensure its include and library paths refer to that package.

## Step 8: Acceptance checklist

- Public headers compile with warnings as errors and the host links only documented targets.
- HTTPS rejects an untrusted CA, expired certificate, wrong hostname, missing SNI and handshake timeout.
- Independent HTTPS connections work concurrently across control and background workers; a delayed
  binding-status poll or stop notification does not delay local hangup or a new start request.
- An already-connected network is reported when the SDK listener attaches; runtime disconnect,
  reconnect, and failure events complete without deadlock.
- Entering provisioning waits for `mybot_stop()` to finish; the product restarts MyBot only after
  network connectivity is usable again.
- KV survives reset, handles not-found and overflow, and protects credentials.
- Capture/playback pass 16 kHz mono S16 tests at each selected ptime with its matching RTSA package
  (the bundled Linux package covers 60 ms).
- Short I/O makes progress and stop unblocks device loss.
- No key or wake-word callback runs after destroy returns; LCD does not retain borrowed content.
- Partial startup failure and repeated start/stop release all resources after workers can be joined;
  if a worker join fails, the SDK retains the affected resources for a later stop attempt.
- Conversation teardown flushes capture, playback, and AEC reference buffers before a new session;
  the flush is performed by the corresponding ring-buffer consumer worker.
- AEC reference samples are committed only after playback accepts the matching samples; prompt PCM
  is never included in the reference stream.
- `mybot_get_state()` reports `READY -> IN_CONVERSATION -> READY` for a normal runtime conversation;
  a conversation interrupted by Wi-Fi loss reports `WIFI_DISCONNECTED` and returns to the
  corresponding online state after reconnect, without deadlock during stop or re-pair.
- A real device completes provisioning, pairing, RTC join, bidirectional audio, hangup and reboot.
- Logs and storage do not expose tokens.

## Known limitations

- HTTPS requires a platform TLS implementation and maintained CA trust store. Linux supplies an OpenSSL
  reference; MCU ports must integrate their TLS stack.
- RTC is Agora-specific, platform registration is process-wide, and one application instance is
  supported.
- Linux reference implementations are development references, not production provisioning or secure storage.
- Third-party redistribution rights must be verified separately; see `THIRD_PARTY_NOTICES.md`.
