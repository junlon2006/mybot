# Release checklist

> [English](RELEASING.md) | [简体中文](RELEASING.zh-CN.md)

## Prepare

- [ ] Choose a semantic prerelease version and update the version variables at the top of
      `CMakeLists.txt` — the single source of truth. `mybot_version.h` is generated from
      `mybot_version.h.in` at configure time; do not edit it directly. Update `CHANGELOG.md` and
      example output together.
- [ ] Confirm README and `docs/PORTING.md` match the actual public APIs and CMake options.
- [ ] Review all third-party changes and `THIRD_PARTY_NOTICES.md`.
- [ ] Obtain written confirmation that bundled Agora artifacts may be redistributed, or remove them
      from the release artifact and document how users supply them.
- [ ] Confirm no secret, token, private endpoint, customer data, or generated KV data is tracked.

## Verify

    cmake -S . -B build-release -DCONFIG_PLATFORM=linux -DMYBOT_ENABLE_ASAN=ON
    cmake --build build-release -j
    ctest --test-dir build-release --output-on-failure
    git diff --check

- [ ] Test 20, 40, and 60 ms packet durations.
- [ ] Run provisioning, pairing, bidirectional audio, hangup, shutdown, and reboot on real hardware.
- [ ] Test network loss, audio-device loss, storage failure, and partial startup failure.
- [ ] Confirm logs and release archives contain no credentials.
- [ ] Confirm HTTPS certificate-chain, hostname, SNI, timeout and trust-store behavior on hardware.
- [ ] Confirm `MYBOT_ALLOW_INSECURE_HTTP=OFF` in every release configuration.

## Publish

- [ ] Create an annotated prerelease tag matching `MYBOT_VERSION_STRING`.
- [ ] Attach source and binary artifacts only after third-party authorization review.
- [ ] Include `LICENSE`, `THIRD_PARTY_NOTICES.md`, changelog, and checksums.
- [ ] Mark the GitHub release as a prerelease and list known limitations.
