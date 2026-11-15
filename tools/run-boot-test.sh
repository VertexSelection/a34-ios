#!/bin/bash
# Write a boot-partition image (tools/build-xnu-boot.sh, or a backup to restore) to `boot` and
# start it with a plain reboot: LK's normal boot path, on which it loads the modem.
#
#   run-boot-test.sh IMAGE
#
# Refuses unless a backup of the phone's own `boot` exists in build/ (tools/backup-boot.sh) and
# IMAGE is a 32 MiB Android boot image v4. Writes `boot` only (and clears a "boot-recovery"
# note in misc if one is set, so that LK does not take the recovery path). Works from TWRP or
# Android (Magisk root). TWRP afterwards: power off, then Volume Up + Power (USB cable to a
# computer, as Samsung asks on this generation) -> `recovery`; keep Volume Up held through our
# loader's white bar if `recovery` holds an xnu-recovery image, or flash plain TWRP there.
set -euo pipefail
ROOT=${A34_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}   # A34_ROOT: the checkout with adb, build/, firmware/
A="$ROOT/tools/platform-tools/adb"
IMG=${1:?usage: run-boot-test.sh IMAGE}
DEV=/dev/block/by-name/boot

ls "$ROOT"/build/boot-backup-*.img >/dev/null 2>&1 ||
    { echo "no build/boot-backup-*.img: run tools/backup-boot.sh first" >&2; exit 1; }
[ "$(stat -c %s "$IMG")" = 33554432 ] || { echo "$IMG is not 33554432 bytes" >&2; exit 1; }
python3 -I - "$IMG" <<'PYEOF' || { echo "$IMG is not an Android boot image v4" >&2; exit 1; }
import struct, sys
d = open(sys.argv[1], "rb").read(64)
sys.exit(0 if d[:8] == b"ANDROID!" and struct.unpack_from("<I", d, 40)[0] == 4 else 1)
PYEOF

state=$("$A" get-state)
if [ "$state" = "recovery" ]; then SU="sh -c"; else SU="su -c"; fi
want=$(sha1sum "$IMG" | cut -c1-40)

"$A" push "$IMG" /data/local/tmp/test-boot.img >/dev/null 2>&1 || \
    "$A" push "$IMG" /tmp/test-boot.img >/dev/null
src=/data/local/tmp/test-boot.img
"$A" shell "[ -f $src ]" || src=/tmp/test-boot.img
"$A" shell "$SU 'dd if=$src of=$DEV bs=4194304 conv=fsync 2>/dev/null; sync; rm $src'"
got=$("$A" shell "$SU 'sha1sum $DEV'" | cut -c1-40)
if [ "$got" != "$want" ]; then
    echo "boot partition does not match the image ($got != $want); not rebooting" >&2
    echo "restore: tools/run-boot-test.sh $(ls "$ROOT"/build/boot-backup-*.img | head -n 1)" >&2
    exit 1
fi
echo "boot partition = $IMG ($want)"

# A "boot-recovery" note in misc would send LK to `recovery`: clear it (64 zero bytes, as our
# loader does on every start).
note=$("$A" shell "$SU 'dd if=/dev/block/by-name/misc bs=13 count=1 2>/dev/null'" | tr -d '\0\r')
if [ "$note" = "boot-recovery" ]; then
    Z=$(mktemp)
    head -c 64 /dev/zero > "$Z"
    "$A" push "$Z" /data/local/tmp/zero64.bin >/dev/null 2>&1 || "$A" push "$Z" /tmp/zero64.bin >/dev/null
    rm -f "$Z"
    z=/data/local/tmp/zero64.bin
    "$A" shell "[ -f $z ]" || z=/tmp/zero64.bin
    "$A" shell "$SU 'dd if=$z of=/dev/block/by-name/misc bs=64 count=1 conv=notrunc 2>/dev/null; sync; rm $z'"
    echo "misc: boot-recovery note cleared"
fi
echo "[$(date +%H:%M:%S)] plain reboot (LK normal boot -> boot partition)"
"$A" shell "$SU 'reboot'" || true
