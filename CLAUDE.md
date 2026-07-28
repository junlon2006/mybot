# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This repository builds an IoT bot (C, C99) that uses the Agora RTSA SDK for real-time audio/video streaming over SD-RTN, with AOSL providing cross-platform system primitives (threads, message queues, networking, memory, logging). Application code lives under `main/` with component modules under `components/`. The project is in early development — only the LICENSE is committed; `main/` and `components/http_client/` are empty placeholders.

## Build System

There is no root-level build system yet. Two vendored sub-projects use CMake:

### AOSL (`components/aosl/`)
- CMake-based, builds `libaosl.a` (static library) and optionally `aosl_test`
- Configurable platform via `CONFIG_PLATFORM` (default: linux)
- C99 standard
- Use as standalone or subproject via `add_subdirectory()`

```bash
# Standalone build
mkdir -p build && cd build
cmake .. -DCONFIG_PLATFORM=linux -DAOSL_DIR=..
make
```

### Agora RTSA SDK (`components/agora_rtsa_sdk/`)
- CMake-based, demos built via `example/build.sh`
- Links `libagora-rtc-sdk.a` (pre-built static library)
- All public APIs in `agora_sdk/include/agora_rtc_api.h`

```bash
cd components/agora_rtsa_sdk/example
./build.sh -a x86_64              # build all demos
./build.sh -a x86_64 -t debug      # debug build
./build.sh -a x86_64 -b rebuild    # full rebuild
```

## Architecture

```
mybot/
├── main/                          # Application entry point (TODO)
├── components/
│   ├── http_client/               # HTTP client component (TODO)
│   ├── aosl/                      # Cross-platform system library
│   │   ├── include/api/           # Public API headers (~30 modules)
│   │   ├── kernel/                # Core kernel sources
│   │   ├── lib/                   # Library sources
│   │   ├── mm/                    # Memory management
│   │   ├── net/                   # Networking stack
│   │   ├── platform/src/          # HAL implementations per platform
│   │   │   ├── linux/             # Linux
│   │   │   ├── darwin/            # macOS
│   │   │   ├── windows/           # Windows
│   │   │   ├── esp32/             # ESP32
│   │   │   └── harmony/           # HarmonyOS
│   │   │   └── ...                # Various RTOS/chip ports
│   │   └── test/                  # Test sources
│   └── agora_rtsa_sdk/            # Agora RTSA SDK v1.10.1
│       ├── agora_sdk/
│       │   ├── include/agora_rtc_api.h  # Single public API header
│       │   └── lib/x86_64/              # Pre-built libraries
│       └── example/
│           ├── hello_rtsa/              # Audio/video streaming demo
│           ├── hello_rtm/               # RTM messaging demo
│           ├── hello_rdt/               # Reliable data transfer demo
│           ├── hello_rtcm/              # Media control message demo
│           └── hello_stream_message/    # Data stream demo
```

### AOSL Key Modules

AOSL provides these subsystems (all under `components/aosl/include/api/`):

| Module | Header | Purpose |
|--------|--------|---------|
| MPQ | `aosl_mpq.h` | Message queue — core event loop primitive |
| MPQ Pool | `aosl_mpqp.h` | Thread pool for dispatching MPQ work |
| MPQ Timer | `aosl_mpq_timer.h` | Timer scheduling within MPQ loop |
| MPQ Net | `aosl_mpq_net.h` | Async socket I/O integrated with MPQ |
| MPQ FD | `aosl_mpq_fd.h` | File descriptor event monitoring |
| Thread | `aosl_thread.h` | Locks, condition variables, events |
| Memory | `aosl_mm.h` | Allocation with optional statistics |
| Log | `aosl_log.h` | Leveled logging (EMERG..DEBUG) |
| Atomic | `aosl_atomic.h` | Atomic ops and memory barriers |
| List | `aosl_list.h` | Doubly linked list |
| RBTree | `aosl_rbtree.h` | Red-black tree |
| PSB | `aosl_psb.h` | Network packet buffer |
| Socket | `aosl_socket.h` | Socket address and byte-order utils |
| Ref | `aosl_ref.h` | Reference counting + rwlock |

### RTSA SDK Lifecycle

All RTSA integration follows this sequence (single public header `agora_rtc_api.h`):

```
agora_rtc_init()                    # Once per process
  └─► agora_rtc_create_connection() # Create a connection
        └─► agora_rtc_join_channel() # Join a channel
              ├─► send/recv loop    # Via callbacks and app state
              └─► agora_rtc_leave_channel()
        └─► agora_rtc_destroy_connection()
  └─► agora_rtc_fini()              # Release all resources
```

Capabilities: audio/video send/receive (H.264, H.265, JPEG, PCM, Opus, AAC, G.711, G.722), RTM peer messaging, RDT reliable file transfer, RTCM channel control messages, data streams.

## Key Patterns

- **AOSL-driven event loop**: Use `aosl_mpq_create()` for the main message queue, `aosl_mpq_timer_create()` for periodic tasks (e.g., sending audio chunks), and `aosl_mpq_net_create()` for async network I/O.
- **RTSA SDK does not use AOSL**: The SDK has its own internal threading and networking. Application code bridges the two — SDK callbacks deliver media data, application logic processes it via AOSL primitives.
- **Single SDK header**: Always `#include "agora_rtc_api.h"` — no internal SDK headers.
- **Follow demo patterns**: Before writing new RTSA integration code, consult the closest shipped demo in `example/` and reuse its callback structure.
- **Build as separate CMake targets**: Each application component should be its own CMake target, linking `aosl` (from `components/aosl/`) and `agora-rtc-sdk` (from `components/agora_rtsa_sdk/agora_sdk/`).

## Testing

- AOSL has a test binary (`aosl_test`) enabled via `AOSL_COMPILE_TEST=ON`
- No project-level testing infrastructure exists yet

## RTSA Skill

The RTSA SDK ships a Claude Code skill at `components/agora_rtsa_sdk/skills/rtsa-sdk-integration/SKILL.md` — use `/rtsa-sdk-integration` when working on SDK integration tasks. It loads focused reference docs from the `references/` subdirectory relevant to the specific feature (core lifecycle, media, RTM, RDT, advanced, samples).
