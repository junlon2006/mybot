#!/usr/bin/env bash
# Convert mybot prompt PCM assets (16 kHz mono s16le) to Opus-in-Ogg.
#
# The device decoder (mybot_ogg_pcm_bk725x.c) reads OpusHead (mono, pre_skip
# trimmed at the target rate) and the final granule position, so the exact
# container parameters below must be preserved:
#   - libopus, 24 kbps VBR, voip application (speech prompts)
#   - 16 kHz mono source -> encoder writes pre_skip=312 (6.5 ms @ 48 kHz)
#
# Usage:  ./convert_pcm_to_ogg.sh [assets_dir]
set -euo pipefail

ASSETS="${1:-$(cd "$(dirname "$0")/.." && pwd)/assets}"
ENCODER_OPTS=(-c:a libopus -b:a 24k -application voip -vbr on -compression_level 5)

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ffmpeg is required:  sudo apt-get install -y ffmpeg" >&2
    exit 1
fi

# Capture the list first: ffmpeg probes its stdin (here /dev/null), and a
# `while read < <(find ...)` pipe would have its leading bytes consumed by
# that probe.
mapfile -t pcms < <(find "$ASSETS" -type f -name '*.pcm' | sort)

count=0
for pcm in "${pcms[@]}"; do
    ogg="${pcm%.pcm}.ogg"
    orig_size=$(stat -c %s "$pcm")
    ffmpeg -y -v error -f s16le -ar 16000 -ac 1 -i "$pcm" "${ENCODER_OPTS[@]}" "$ogg" < /dev/null
    new_size=$(stat -c %s "$ogg")
    printf '%28s -> %-12s %6dKiB -> %5dKiB\n' "$(basename "$pcm")" "$(basename "$ogg")" \
        $((orig_size / 1024)) $((new_size / 1024))
    count=$((count + 1))
done

echo "converted $count files"
