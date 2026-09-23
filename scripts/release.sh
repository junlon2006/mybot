#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Package a tagged source snapshot or verify its uploaded GitHub Release assets.
set -euo pipefail
export LC_ALL=C
umask 022

usage() {
    printf 'Usage: %s {package|verify} vMAJOR.MINOR.PATCH[-PRERELEASE] [OUTPUT_DIR]\n' "$0"
    printf 'Default output: build-release/TAG. verify requires gh and local release artifacts.\n'
}

die() {
    printf 'release: %s\n' "$*" >&2
    exit 1
}

if [[ $# == 1 && ( $1 == --help || $1 == -h ) ]]; then
    usage
    exit 0
fi
[[ $# == 2 || $# == 3 ]] || { usage >&2; exit 1; }
mode=$1
tag=$2
[[ $mode == package || $mode == verify ]] || die 'expected package or verify'
[[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[[:alnum:].-]+)?$ ]] || die 'supply an explicit version tag'

repo_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tag_ref="refs/tags/$tag"
[[ $(git -C "$repo_root" cat-file -t "$tag_ref") == tag ]] || die "$tag must be an annotated tag"
commit=$(git -C "$repo_root" rev-parse --verify "$tag_ref^{commit}")
main_commit=$(git -C "$repo_root" rev-parse --verify refs/remotes/origin/main^{commit}) ||
    die 'fetch origin main before packaging or verification'
git -C "$repo_root" merge-base --is-ancestor "$commit" "$main_commit" ||
    die "$tag is not proven to belong to origin/main; fetch the complete main history"

version=$(git -C "$repo_root" show "$commit:CMakeLists.txt" | awk '
    /^set\(MYBOT_VERSION_(MAJOR|MINOR|PATCH|PRERELEASE)[[:space:]]/ {
        key = $1; sub(/^set\(MYBOT_VERSION_/, "", key)
        value = $0; sub(/^[^[:space:]]+[[:space:]]+/, "", value)
        sub(/\)[[:space:]]*$/, "", value); gsub(/"/, "", value)
        part[key] = value
    }
    END {
        if (part["MAJOR"] == "" || part["MINOR"] == "" || part["PATCH"] == "") exit 1
        printf "%s.%s.%s", part["MAJOR"], part["MINOR"], part["PATCH"]
        if (part["PRERELEASE"] != "") printf "-%s", part["PRERELEASE"]
    }') || die 'cannot read the version from the tagged CMakeLists.txt'
[[ $tag == "v$version" ]] || die "$tag does not match tagged CMake version $version"

name="mybot-$version"
archive="$name-source.tar.gz"
checksums="$name-SHA256SUMS"
output_dir=${3:-"$repo_root/build-release/$tag"}
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/mybot-release.XXXXXX")
trap 'rm -rf -- "$work_dir"' EXIT

if [[ $mode == verify ]]; then
    command -v gh >/dev/null || die 'verify requires GitHub CLI (gh) on PATH'
    [[ -f "$output_dir/$archive" && -f "$output_dir/$checksums" ]] ||
        die "local release artifacts are missing in $output_dir"
    output_dir=$(CDPATH= cd -- "$output_dir" && pwd)
    (cd -- "$output_dir" && sha256sum "$archive") | cmp -s - "$output_dir/$checksums" ||
        die 'local archive does not match its checksum file'
    mkdir -- "$work_dir/download"
    (cd -- "$repo_root" && gh release download "$tag" --pattern "$archive" \
        --pattern "$checksums" --dir "$work_dir/download")
    cmp -s "$output_dir/$checksums" "$work_dir/download/$checksums" ||
        die 'Release checksum asset is missing or differs from the local release'
    (cd -- "$work_dir/download" && sha256sum "$archive") | cmp -s - "$output_dir/$checksums" ||
        die 'Release source asset is missing or has a different checksum'
    printf 'Verified %s: %s and %s\n' "$tag" "$archive" "$checksums"
    exit 0
fi

# Read the gitlink from the tag, never from the current index or submodule HEAD.
submodules=$(git -C "$repo_root" ls-tree -r "$commit" | awk '$1 == "160000" {print $4}')
[[ $submodules == third_party/aosl ]] || die 'expected only the third_party/aosl submodule'
aosl_commit=$(git -C "$repo_root" rev-parse "$commit:third_party/aosl")
aosl_dir="$repo_root/third_party/aosl"
[[ -e "$aosl_dir/.git" ]] || die 'initialize AOSL in this Git checkout with git submodule update --init --recursive'
git -C "$aosl_dir" cat-file -e "$aosl_commit^{commit}" ||
    die "fetch pinned AOSL commit $aosl_commit into third_party/aosl"
nested=$(git -C "$aosl_dir" ls-tree -r "$aosl_commit" | awk '$1 == "160000" {print $4}')
[[ -z $nested ]] || die 'pinned AOSL has nested submodules; extend packaging before releasing'

mkdir -- "$work_dir/$name"
git -C "$repo_root" archive "$commit" | tar -xf - -C "$work_dir/$name"
mkdir -p -- "$work_dir/$name/third_party/aosl"
git -C "$aosl_dir" archive "$aosl_commit" | tar -xf - -C "$work_dir/$name/third_party/aosl"

# Keep RTSA headers and build metadata, matching the existing source releases.
# GNU tar metadata normalization plus gzip -n makes repeated builds byte-identical.
timestamp=$(git -C "$repo_root" show -s --format=%ct "$commit")
tar --sort=name --format=gnu --mtime="@$timestamp" --owner=0 --group=0 --numeric-owner \
    --exclude="$name/third_party/agora_rtsa_sdk/agora_sdk/lib" \
    --exclude="$name/third_party/agora_rtsa_sdk/agora_sdk/bin" \
    --exclude="$name/third_party/agora_rtsa_sdk/example/out" \
    --exclude="$name/third_party/agora_rtsa_sdk/example/third-party/file_parser/lib" \
    -cf - -C "$work_dir" "$name" | gzip -n > "$work_dir/$archive"
(cd -- "$work_dir" && sha256sum "$archive") > "$work_dir/$checksums"
mkdir -p -- "$output_dir"
mv -f -- "$work_dir/$archive" "$work_dir/$checksums" "$output_dir/"
printf 'Packaged %s (%s), AOSL %s\n%s\n%s\n' \
    "$tag" "$commit" "$aosl_commit" "$output_dir/$archive" "$output_dir/$checksums"
