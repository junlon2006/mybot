#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Installs the repository-local git hooks and commit-message template.
# Safe to re-run; only touches this repository's .git/config.

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

git -C "$repo_root" config core.hooksPath githooks
git -C "$repo_root" config commit.template .gitmessage

echo "git hooks installed: core.hooksPath=githooks commit.template=.gitmessage"
