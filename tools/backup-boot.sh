#!/bin/bash
# Read the phone's `boot` partition into build/ (gitignored, never committed) BEFORE anything is
# written to it. Read only; the partition name is fixed here and nothing else is read.
#
#   backup-boot.sh        -> build/boot-backup-<first 8 hex of sha1>.img
#
# Works from TWRP or from Android (Magisk root). Checks: 32 MiB, Android boot image header v4,
# SHA-1 on the phone equal to the copy's; says whether it is the stock A346EXXSADYG1 boot.img.
# Restore with: tools/run-boot-test.sh build/boot-backup-XXXXXXXX.img
set -euo pipefail
ROOT=${A34_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}   # A34_ROOT: the checkout with adb, build/, firmware/
A="$ROOT/tools/platform-tools/adb"
DEV=/dev/block/by-name/boot
STOCK="$ROOT/firmware/A346EXXSADYG1_TUR/flash/boot.img"

state=$("$A" get-state)
if [ "$state" = "recovery" ]; then
    sh_root() { "$A" shell "$1"; }
    out_root() { "$A" exec-out "$1"; }
else
    sh_root() { "$A" shell "su -c '$1'"; }
    out_root() { "$A" exec-out "su -c '$1'"; }
fi

size=$(sh_root "blockdev --getsize64 $DEV" | tr -d '\r')
[ "$size" = 33554432 ] || { echo "boot partition is '$size' bytes, expected 33554432; stopping" >&2; exit 1; }
dev_sha=$(sh_root "sha1sum $DEV" | cut -c1-40)
[ ${#dev_sha} = 40 ] || { echo "no SHA-1 from the phone; stopping" >&2; exit 1; }
mkdir -p "$ROOT/build"
OUT="$ROOT/build/boot-backup-${dev_sha:0:8}.img"
if [ -f "$OUT" ] && [ "$(sha1sum "$OUT" | cut -c1-40)" = "$dev_sha" ]; then
    echo "already backed up: $OUT"
else
    out_root "dd if=$DEV bs=4194304 2>/dev/null" > "$OUT.part"
    got=$(sha1sum "$OUT.part" | cut -c1-40)
    if [ "$got" != "$dev_sha" ] || [ "$(stat -c %s "$OUT.part")" != 33554432 ]; then
        echo "copy does not match the partition ($got != $dev_sha); kept as $OUT.part" >&2
        exit 1
    fi
    mv "$OUT.part" "$OUT"
    chmod 0444 "$OUT"
    echo "backed up: $OUT (sha1 $dev_sha)"
fi
python3 -I - "$OUT" <<'PYEOF'
import struct, sys
d = open(sys.argv[1], "rb").read(4096)
print("header: %s v%d, kernel %d bytes" % (d[:8], struct.unpack_from("<I", d, 40)[0],
                                          struct.unpack_from("<I", d, 8)[0]))
PYEOF
if [ -f "$STOCK" ]; then
    if [ "$(sha1sum "$STOCK" | cut -c1-40)" = "$dev_sha" ]; then
        echo "it is the stock A346EXXSADYG1 boot.img"
    else
        echo "it differs from the stock A346EXXSADYG1 boot.img (keep this backup; the stock file is a second way back)"
    fi
fi
