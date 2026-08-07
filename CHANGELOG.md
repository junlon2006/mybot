# Changelog

This project follows Semantic Versioning after 1.0. Before 1.0, public API and ABI may change
between release candidates.

## [Unreleased]

### Fixed

- Continue binding-status polling during active conversations and end RTC locally when the device
  becomes unbound or its credential is rejected.
- Harden the Linux file KV backend against symlink traversal and persist atomic replacements and
  deletions with file and directory `fsync`.
- Preserve runtime Wi-Fi disconnect/reconnect events, pause device-service traffic while offline,
  and end active RTC conversations locally without reinitializing services after reconnect.
- Percent-encode device IDs in URL path segments and reject control characters in dynamic HTTP
  header values and request targets.
- Reject conversation-start responses without a valid conversation ID before entering the active
  conversation state.

### Added

- HTTPS-by-default device-service transport with a platform TLS contract and Linux OpenSSL backend.
- Cross-platform porting guide, release checklist, security and contribution policies.
- CI coverage for Linux builds, tests, public headers, and external CMake host integration.
- Bilingual (English / Simplified Chinese) README with an AI-conversation product overview and
  layered architecture diagrams.
- Bilingual (English / Simplified Chinese) community docs: contributing, support, security, and
  code of conduct.
- Bilingual porting and release guides under `docs/` (PORTING / RELEASING).
- SPDX license identifiers on all self-maintained C sources (Apache-2.0; MIT for the cJSON-derived
  `mybot_json` sources).

## [0.1.0-rc.1] - Unreleased

### Added

- Cross-platform SDK target and public platform ops for audio, Wi-Fi, KV, keys, LCD, and wake words.
- Linux reference backends and CLI example.
- Device pairing, lifecycle, Agora RTC audio, and optional local wake-word flow.
- Version API and validated CMake feature configuration.
- Unit, Linux platform, public-header, and external-host integration tests.

### Known limitations

- MCU ports must provide a certificate-validating TLS backend and CA trust store.
- API/ABI is not stable; the runtime uses singleton registries and process-global dependencies.
- Bundled Agora RTSA artifacts are x86_64 Linux only and require separate license verification.
