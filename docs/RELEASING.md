# Release checklist

> [English](RELEASING.md) | [简体中文](RELEASING.zh-CN.md)

## Prepare

- [ ] Choose the semantic version and update the version variables at the top of
      `CMakeLists.txt` — the single source of truth. `mybot_version.h` is generated from
      `mybot_version.h.in` at configure time; do not edit it directly. Update `CHANGELOG.md` and
      example output together.
- [ ] Confirm README and `docs/PORTING.md` match the actual public APIs and CMake options.
- [ ] Review all third-party changes and `THIRD_PARTY_NOTICES.md`.
- [ ] Obtain written redistribution authorization for the bundled Agora RTSA binary from Agora
      (声网) sales/business before including it in a release artifact; the in-repo x86_64 Linux
      binary stays for the Linux demo. If authorization is unavailable, exclude the binary and
      document how users supply it via `AGORA_SDK_DIR` / `AGORA_RTC_LIBRARY`.
- [ ] Include the pinned AOSL sources in the release source asset. GitHub's automatic **Source code**
      archives omit submodules and have no `.git` metadata, so `git submodule update` cannot populate
      them. Users should download the release source asset or clone the repository with
      `git clone --recurse-submodules https://github.com/junlon2006/mybot.git`.
- [ ] Confirm no secret, token, private endpoint, customer data, or generated KV data is tracked.

## Verify

    cmake -S . -B build-release -DCONFIG_PLATFORM=linux -DMYBOT_ENABLE_ASAN=ON
    cmake --build build-release -j
    ctest --test-dir build-release --output-on-failure
    git diff --check

- [ ] Test the bundled RTSA package at 60 ms; test 20/40 ms only with matching RTSA packages.
- [ ] Verify RTM login, conversation-channel subscription, and the
      `VP_REGISTER_SUCCESS` LCD indicator with a compatible service.
- [ ] For BK device validation, use the independent
      [BK7258](https://github.com/junlon2006/mybot-bk7258) or
      [BK7259](https://github.com/junlon2006/mybot-bk7259) firmware project. Verify the pending/success
      voiceprint markers and the provisioning-success prompt gate before MyBot startup.
- [ ] Run provisioning, pairing, bidirectional audio, hangup, shutdown, and reboot on real hardware.
- [ ] Test network loss, audio-device loss, storage failure, and partial startup failure.
- [ ] Confirm logs and release archives contain no credentials.
- [ ] Confirm HTTPS certificate-chain, hostname, SNI, timeout and trust-store behavior on hardware.
- [ ] Confirm `MYBOT_ALLOW_INSECURE_HTTP=OFF` in every release configuration.

## Package the tagged source

Use [scripts/release.sh](../scripts/release.sh) from a Git clone. Packaging needs Bash, Git, GNU tar,
gzip, and sha256sum. Fetch the release tag and current `origin/main`, and initialize AOSL:

```sh
git fetch origin main --tags
git submodule update --init --recursive
./scripts/release.sh package v1.2.0
```

For a shallow clone, fetch the complete history first with `git fetch --unshallow origin`.
The script requires an explicit annotated tag whose version matches that tag's `CMakeLists.txt`
and whose commit is in `origin/main` history. It packages the tag and the AOSL commit recorded in
that tag's gitlink; local edits, the current branch, and the checked-out AOSL revision do not enter
the archive. If the recorded AOSL commit is missing locally, fetch that commit explicitly with
`git -C third_party/aosl fetch origin <aosl-commit>`. The script does not fetch or change checkouts.

`package TAG [OUTPUT_DIR]` defaults to `build-release/TAG` and produces:

- `mybot-VERSION-source.tar.gz`, rooted at `mybot-VERSION/`, including the pinned AOSL sources,
  licenses, third-party notices, and changelog.
- `mybot-VERSION-SHA256SUMS`, containing the source archive's SHA-256 checksum.

The source package excludes prebuilt RTSA binaries while retaining its headers and build metadata.
Supply the matching RTSA binary through `AGORA_RTC_LIBRARY`; when changing RTSA packages or target
platforms, also set `AGORA_SDK_DIR` to the matching headers and metadata. Packaging sorts archive
paths, uses UID/GID 0 and the tag commit's timestamp, and omits the gzip timestamp so repeated
packaging of the same tag is reproducible.

## Publish

- [ ] Confirm the existing `v1.0.0` tag still points to the historical 1.0.0 release; never move
      or overwrite an existing release tag.
- [ ] Create an annotated release tag matching `MYBOT_VERSION_STRING` (e.g. `v1.2.0`) on `main`.
- [ ] Push the release commit on `main` and its annotated tag, then package that tag as above.
- [ ] Attach source and binary artifacts only after third-party authorization review.
- [ ] Attach the generated source archive and checksum file. Keep the local files for verification.
- [ ] Publish the GitHub release as a stable release (not marked as a prerelease) and list
      known limitations.
- [ ] Verify the published assets with GitHub CLI (`gh`) installed:

      `./scripts/release.sh verify v1.2.0`

`verify TAG [OUTPUT_DIR]` uses the same output directory convention as `package`. It downloads the
two named GitHub Release assets and compares the checksum file and actual archive hash with the
local files. Other release assets are allowed; the checksum file must contain only the source
archive's entry. For an older manually packaged release with that checksum format, use its original
local files; a newly generated archive can have different metadata and a different hash.
Neither command creates tags, pushes commits, uploads assets, or publishes a release.
