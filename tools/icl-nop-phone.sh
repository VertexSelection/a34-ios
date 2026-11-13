#!/bin/bash
# Apply the InstalledContentLibrary NOP (tools/icl-nop.py) to the iOS root
# already written to userdata, from TWRP: one 4 KiB block is read, checked,
# patched on the host, written back and read back.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
A="$ROOT/tools/platform-tools/adb"
DEV=/dev/block/by-name/userdata
BLK=$(python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import importlib; print(importlib.import_module("icl-nop").ROOT_BLOCK)' "$ROOT/tools")
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

[ "$("$A" get-state)" = "recovery" ] || { echo "phone must be in TWRP" >&2; exit 1; }
"$A" exec-out "dd if=$DEV bs=4096 skip=$BLK count=1 2>/dev/null" > "$T/orig.bin"
python3 -I "$ROOT/tools/icl-nop.py" block "$T/orig.bin" "$T/new.bin"
if cmp -s "$T/orig.bin" "$T/new.bin"; then
    exit 0
fi
"$A" push "$T/new.bin" /tmp/icl-block.bin >/dev/null
"$A" shell "dd if=/tmp/icl-block.bin of=$DEV bs=4096 seek=$BLK count=1 conv=notrunc,fsync 2>/dev/null; sync; rm /tmp/icl-block.bin"
"$A" exec-out "dd if=$DEV bs=4096 skip=$BLK count=1 2>/dev/null" > "$T/check.bin"
cmp "$T/new.bin" "$T/check.bin" && echo "block $BLK on the phone verified"
