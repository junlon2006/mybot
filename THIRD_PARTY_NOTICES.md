# Third-party notices

The root Apache-2.0 license applies to mybot-maintained code unless a file states otherwise. It does
not replace third-party terms. This file is informational, not legal advice.

## AOSL

Location: `third_party/aosl` — a git submodule pinned to upstream commit
`84e086084ebcd0ae2455a0ce5721950c5fe2e656` of https://github.com/AgoraIO-Community/aosl.

AOSL includes `third_party/aosl/LICENSE`, which is based on Apache-2.0 and adds restrictive
conditions. Read that file before using, modifying, deploying, or redistributing AOSL. Do not label
the combined repository or binary as uniformly Apache-2.0.

Because AOSL is a git submodule, source archives of this repository do not include it
automatically; initialize submodules after cloning with `git submodule update --init --recursive`.

## Agora RTSA SDK

Location: `third_party/agora_rtsa_sdk`. The bundled shared-library package identifies itself as
`Agora-RTSALite-RmRcAcAj-x86_64-linux-gnu-v1.10.1-20260828_180234-1270765`. It is provided for
Linux **development and demo** use only and is intentionally retained in this repository for the
Linux reference demo (it is not removed).

Package provenance recorded for this update:

- Source archive: `Agora-RTSALite-RmRcAcAj-x86_64-linux-gnu-v1.10.1-20260828_180234-1270765.tgz`
- Archive SHA-256: `739901d09a4161a33f36e06281fe30dc11629f1f3271e55a5662501593985fbf`
- `libagora-rtc-sdk.so` SHA-256: `269d985f2fcb86ce259591853e59bc5a5d86f5f8fa82a1408bc7ac3795c3f999`

No standalone license or NOTICE for the bundled RTSA binary was found in the package during the
1.0.0 audit. Possession of the files is not evidence of redistribution rights. Commercial or
production use and redistribution (source archives, binaries, container/firmware images, mirrors)
require a separate license from Agora (声网): contact Agora's sales/business channel to obtain the
applicable terms and written redistribution authorization before publishing any release artifact
that contains the binary.

Files inside the Agora example tree may carry their own copyright or license headers; those terms
also remain in force.

## cJSON-derived implementation

`src/support/mybot_json.c` and `src/internal/mybot_json.h` are namespaced derivatives of cJSON.
They retain the MIT license and Dave Gamble copyright notice in the source.

## xiaozhi-esp32 announcement assets

`assets/locales/` contains raw 16 kHz mono s16 PCM files decoded from the xiaozhi-esp32 project
(https://github.com/78/xiaozhi-esp32), which is distributed under the MIT License
(c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd. and Project Contributors. The pairing-code
prompt and digit sounds are derived from that project's `main/assets/locales/<locale>/*.ogg`
assets; retain the MIT copyright notice when redistributing them.

## Release blocker

Do not publish a release artifact containing the bundled Agora binary until its license and
redistribution authorization have been verified. If authorization is unavailable, exclude the
binary and require users to supply `AGORA_SDK_DIR` and `AGORA_RTC_LIBRARY` locally.
