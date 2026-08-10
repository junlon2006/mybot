# Third-party notices

The root Apache-2.0 license applies to mybot-maintained code unless a file states otherwise. It does
not replace third-party terms. This file is informational, not legal advice.

## AOSL

Location: `third_party/aosl` — a git submodule pinned to upstream commit
`39c3fb7b331b52b706f4f1bc9a803913bc0d82ff` of https://github.com/AgoraIO-Community/aosl.

AOSL includes `third_party/aosl/LICENSE`, which is based on Apache-2.0 and adds restrictive
conditions. Read that file before using, modifying, deploying, or redistributing AOSL. Do not label
the combined repository or binary as uniformly Apache-2.0.

The previous vendored tree carried one local change (ESP-IDF IPv6 traffic-class support in
`aosl_hal_sk_set_dscp`). It is preserved as `third_party/patches/aosl/esp32-ipv6-tclass.patch` and
is **not** applied to the pristine submodule by default; see
`third_party/patches/aosl/README.md` before applying it to a build.

## Agora RTSA SDK

Location: `third_party/agora_rtsa_sdk`. The bundled package identifies itself as Agora RTSA Lite
v1.10.1 for x86_64 Linux.

No standalone license or NOTICE for the bundled RTSA binary was found in the package during the
0.1.0-rc.1 audit. Possession of the files is not evidence of redistribution rights. Before a public
source archive, binary release, container image, firmware image, or mirror is published, the
distributor must obtain and retain the applicable Agora terms and confirm redistribution rights.

Files inside the Agora example tree may carry their own copyright or license headers; those terms
also remain in force.

## cJSON-derived implementation

`src/support/mybot_json.c` and `src/internal/mybot_json.h` are namespaced derivatives of cJSON.
They retain the MIT license and Dave Gamble copyright notice in the source.

## Release blocker

Do not publish a release artifact containing the bundled Agora binary until its license and
redistribution authorization have been verified. If authorization is unavailable, exclude the
binary and require users to supply `AGORA_SDK_DIR` and `AGORA_RTC_LIBRARY` locally.

