#!/bin/bash
# Write the iOS root image to the start of the userdata partition, from TWRP.
#
#   write-root.sh IMAGE
#
# This erases Android's data: Android will not boot until Samsung firmware is
# flashed again. The image is streamed compressed (it is mostly empty space)
# and verified by SHA-256 of the written range afterwards.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
A="$ROOT/tools/platform-tools/adb"
IMG=${1:?usage: write-root.sh IMAGE}
DEV=/dev/block/by-name/userdata

[ "$("$A" get-state)" = "recovery" ] || { echo "phone must be in TWRP" >&2; exit 1; }
size=$(stat -c %s "$IMG")
[ $((size % 4194304)) -eq 0 ] || { echo "image size is not a multiple of 4 MiB" >&2; exit 1; }
blocks=$((size / 4194304))
part=$("$A" shell "blockdev --getsize64 $DEV" | tr -d '\r')
[ "$part" -ge "$size" ] || { echo "userdata ($part) is smaller than the image ($size)" >&2; exit 1; }
"$A" shell 'command -v gzip >/dev/null' || { echo "no gzip in TWRP" >&2; exit 1; }

echo "unmounting /data and friends"
"$A" shell 'twrp unmount data >/dev/null 2>&1; umount /sdcard /data/media /data 2>/dev/null; mount | grep -c " /data " || true'
if "$A" shell "grep -q ' /data ' /proc/mounts"; then
    echo "/data is still mounted; refusing to write" >&2
    exit 1
fi

want=$(sha256sum "$IMG" | cut -d' ' -f1)
echo "local sha256: $want"
echo "[$(date +%H:%M:%S)] streaming $((size >> 20)) MiB to $DEV"
# Through `adb shell` (shell_v2 forwards stdin); TWRP's `adb exec-in` silently
# ran nothing here. dd's own messages stay visible.
gzip -1 -c "$IMG" | "$A" shell "gzip -dc | dd of=$DEV bs=4194304; echo dd-rc=\$?; sync"
echo "[$(date +%H:%M:%S)] written; verifying"
got=$("$A" shell "dd if=$DEV bs=4194304 count=$blocks 2>/dev/null | sha256sum" | cut -d' ' -f1)
echo "phone sha256: $got"
[ "$got" = "$want" ] && echo "root image verified" || { echo "MISMATCH" >&2; exit 1; }
