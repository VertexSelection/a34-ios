#!/bin/bash
# Pull the kernel's persistent console log (ios-sweet "SWEETLOG" ring, the
# last 4100 KiB of userdata) from TWRP and decode it.
#
#   pull-log.sh OUT.txt
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
A="$ROOT/tools/platform-tools/adb"
OUT=${1:?usage: pull-log.sh OUT.txt}
DEV=/dev/block/by-name/userdata
AREA=1025                                   # 4 KiB blocks: header + 4 MiB ring

[ "$("$A" get-state)" = "recovery" ] || { echo "phone must be in TWRP" >&2; exit 1; }
size=$("$A" shell "blockdev --getsize64 $DEV" | tr -d '\r')
start=$((size / 4096 - AREA))
"$A" exec-out "dd if=$DEV bs=4096 skip=$start count=$AREA 2>/dev/null" > "$OUT.bin"
python3 -I "$ROOT/third_party/ios-sweet/tools/sweet-log.py" "$OUT.bin" "$OUT"
rm -f "$OUT.bin"
wc -l "$OUT"
