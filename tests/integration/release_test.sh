#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/mybot-release-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT
repo="$test_root/repo"
out="$test_root/package"
again="$test_root/package-again"
remote="$test_root/remote"
tag=v1.2.3
archive=mybot-1.2.3-source.tar.gz
sums=mybot-1.2.3-SHA256SUMS

init_repo() {
    git init -q "$1"
    git -C "$1" config user.name "Release test"
    git -C "$1" config user.email "release-test@example.invalid"
    git -C "$1" config commit.gpgsign false
    git -C "$1" config tag.gpgsign false
}

expect_failure() {
    if "$@" >"$test_root/failure.log" 2>&1; then
        printf 'Expected failure: %s\n' "$*" >&2
        exit 1
    fi
}

init_repo "$test_root/aosl-source"
printf 'pinned AOSL\n' >"$test_root/aosl-source/aosl.c"
printf 'AOSL license\n' >"$test_root/aosl-source/LICENSE"
git -C "$test_root/aosl-source" add .
git -C "$test_root/aosl-source" commit -qm "Pin AOSL"

init_repo "$repo"
mkdir -p "$repo/scripts" "$repo/src"
cp "$source_root/scripts/release.sh" "$repo/scripts/release.sh"
printf '%s\n' 'set(MYBOT_VERSION_MAJOR 1)' 'set(MYBOT_VERSION_MINOR 2)' \
    'set(MYBOT_VERSION_PATCH 3)' 'set(MYBOT_VERSION_PRERELEASE "")' >"$repo/CMakeLists.txt"
printf 'tagged SDK\n' >"$repo/src/sdk.c"
printf 'SDK license\n' >"$repo/LICENSE"
printf 'Third-party notices\n' >"$repo/THIRD_PARTY_NOTICES.md"
rtsa="$repo/third_party/agora_rtsa_sdk"
mkdir -p "$rtsa/agora_sdk/include" "$rtsa/agora_sdk/lib/x86_64" "$rtsa/agora_sdk/bin" "$rtsa/example/out" \
    "$rtsa/example/third-party/file_parser/lib"
printf 'RTSA header\n' >"$rtsa/agora_sdk/include/agora_rtc_api.h"
printf 'CONFIG_MINIMAL_TIMER_INTERVAL_MS=60\n' >"$rtsa/agora_sdk/.config"
for binary in agora_sdk/lib/x86_64/libagora-rtc-sdk.so agora_sdk/bin/demo example/out/demo \
    example/third-party/file_parser/lib/libparser.a; do
    printf 'excluded RTSA binary\n' >"$rtsa/$binary"
done
git -C "$repo" -c protocol.file.allow=always submodule add -q "$test_root/aosl-source" third_party/aosl
git -C "$repo" add .
git -C "$repo" commit -qm "Release fixture"
release_commit=$(git -C "$repo" rev-parse HEAD)
git -C "$repo" tag -a "$tag" -m "Release $tag"
tag_object=$(git -C "$repo" rev-parse "refs/tags/$tag")
git -C "$repo" update-ref refs/remotes/origin/main "$release_commit"

# Dirty trees and a newer submodule HEAD must not alter the tagged package.
git -C "$repo/third_party/aosl" config user.name "Release test"
git -C "$repo/third_party/aosl" config user.email "release-test@example.invalid"
git -C "$repo/third_party/aosl" config commit.gpgsign false
printf 'new AOSL\n' >"$repo/third_party/aosl/aosl.c"
git -C "$repo/third_party/aosl" commit -qam "New AOSL"
printf 'dirty AOSL\n' >"$repo/third_party/aosl/aosl.c"
printf 'dirty SDK\n' >"$repo/src/sdk.c"
printf 'invalid working-tree version\n' >"$repo/CMakeLists.txt"
printf 'untracked secret\n' >"$repo/untracked.txt"

bash "$repo/scripts/release.sh" package "$tag" "$out"
# Cross a timestamp boundary so non-normalized archive mtimes cannot pass by chance.
sleep 1
bash "$repo/scripts/release.sh" package "$tag" "$again"
cmp "$out/$archive" "$again/$archive"
cmp "$out/$sums" "$again/$sums"
(cd "$out" && sha256sum --check "$sums")
[[ $(wc -l <"$out/$sums") -eq 1 ]]
[[ $(tar -xOf "$out/$archive" mybot-1.2.3/src/sdk.c) == 'tagged SDK' ]]
[[ $(tar -xOf "$out/$archive" mybot-1.2.3/third_party/aosl/aosl.c) == 'pinned AOSL' ]]
[[ $(tar -xOf "$out/$archive" mybot-1.2.3/third_party/aosl/LICENSE) == 'AOSL license' ]]
[[ $(tar -xOf "$out/$archive" mybot-1.2.3/third_party/agora_rtsa_sdk/agora_sdk/.config) == \
    'CONFIG_MINIMAL_TIMER_INTERVAL_MS=60' ]]
tar -tf "$out/$archive" >"$test_root/members"
grep -qx 'mybot-1.2.3/third_party/agora_rtsa_sdk/agora_sdk/include/agora_rtc_api.h' "$test_root/members"
if grep -Eq '/(\.git(/|$)|untracked\.txt$)|agora_rtsa_sdk/(agora_sdk/(lib/|bin/)|example/out/|example/third-party/file_parser/lib/)' \
    "$test_root/members"; then
    printf 'Package contains working-tree data, Git internals, or RTSA binaries\n' >&2
    exit 1
fi

# Each rejection retains the same valid package version, except the explicit mismatch.
git -C "$repo" update-ref "refs/tags/$tag" "$release_commit"
expect_failure bash "$repo/scripts/release.sh" package "$tag" "$test_root/lightweight"
git -C "$repo" update-ref "refs/tags/$tag" "$tag_object"
unrelated=$(git -C "$repo" commit-tree "$release_commit^{tree}" -m "Unrelated main")
git -C "$repo" update-ref refs/remotes/origin/main "$unrelated"
expect_failure bash "$repo/scripts/release.sh" package "$tag" "$test_root/off-main"
git -C "$repo" update-ref refs/remotes/origin/main "$release_commit"
git -C "$repo" tag -a v1.2.4 -m "Wrong version"
expect_failure bash "$repo/scripts/release.sh" package v1.2.4 "$test_root/wrong-version"
mv "$repo/.git/modules/third_party/aosl/objects" "$test_root/aosl-objects"
mkdir "$repo/.git/modules/third_party/aosl/objects"
expect_failure bash "$repo/scripts/release.sh" package "$tag" "$test_root/missing-objects"
rmdir "$repo/.git/modules/third_party/aosl/objects"
mv "$test_root/aosl-objects" "$repo/.git/modules/third_party/aosl/objects"

# A prerelease uses its complete version in both the default directory and assets.
printf '%s\n' 'set(MYBOT_VERSION_MAJOR 1)' 'set(MYBOT_VERSION_MINOR 2)' \
    'set(MYBOT_VERSION_PATCH 3)' 'set(MYBOT_VERSION_PRERELEASE "rc.1")' >"$repo/CMakeLists.txt"
git -C "$repo" add CMakeLists.txt
git -C "$repo" commit -qm "Prerelease fixture"
git -C "$repo" tag -a v1.2.3-rc.1 -m "Prerelease"
git -C "$repo" update-ref refs/remotes/origin/main HEAD
bash "$repo/scripts/release.sh" package v1.2.3-rc.1
test -f "$repo/build-release/v1.2.3-rc.1/mybot-1.2.3-rc.1-source.tar.gz"

# The gh stub permits downloads only, so verification cannot mutate a release.
mkdir -p "$test_root/bin" "$remote"
printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail' \
    '[[ $1 == release && $2 == download && $3 == v1.2.3 ]]' 'shift 3' \
    'assets=(); destination=""' 'while (($#)); do' '    case "$1" in' \
    '        --pattern) assets+=("$2"); shift 2 ;;' \
    '        --dir) destination=$2; shift 2 ;;' '        *) exit 2 ;;' \
    '    esac' 'done' '[[ -n $destination && ${#assets[@]} -gt 0 ]]' \
    'mkdir -p "$destination"' 'for asset in "${assets[@]}"; do' \
    '    cp -- "$RELEASE_TEST_REMOTE/$asset" "$destination/$asset"' 'done' >"$test_root/bin/gh"
chmod +x "$test_root/bin/gh"
export PATH="$test_root/bin:$PATH"
export RELEASE_TEST_REMOTE="$remote"
cp "$out/$archive" "$out/$sums" "$remote/"
bash "$repo/scripts/release.sh" verify "$tag" "$out"
rm -- "$remote/$sums"
expect_failure bash "$repo/scripts/release.sh" verify "$tag" "$out"
cp "$out/$sums" "$remote/$sums"
printf 'tampered remote checksum\n' >>"$remote/$sums"
expect_failure bash "$repo/scripts/release.sh" verify "$tag" "$out"
cp "$out/$sums" "$remote/$sums"
printf 'tampered remote archive\n' >>"$remote/$archive"
expect_failure bash "$repo/scripts/release.sh" verify "$tag" "$out"
cp "$out/$archive" "$remote/$archive"
printf 'tampered local archive\n' >>"$out/$archive"
expect_failure bash "$repo/scripts/release.sh" verify "$tag" "$out"
printf 'release_test: ok\n'
