#!/bin/bash
# Flash a test recovery image and boot into it without `reboot recovery`.
#
#   run-test.sh IMAGE
#
# `reboot recovery` makes Samsung's kernel store a recovery reboot mode in the
# param partition, and LK then rewrites the misc bootloader message from it on
# every start, so a test that hangs or resets loops forever. Instead, write the
# misc bootloader message ourselves and do a plain reboot: LK boots recovery
# once, and our loader clears the message, so any later reset boots Android.
#
# Works from Android (Magisk root) or from TWRP.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
A="$ROOT/tools/platform-tools/adb"
IMG=${1:?usage: run-test.sh IMAGE}

state=$("$A" get-state)
if [ "$state" = "recovery" ]; then SU="sh -c"; else SU="su -c"; fi
want=$(sha1sum "$IMG" | cut -c1-40)

"$A" push "$IMG" /data/local/tmp/test-recovery.img >/dev/null 2>&1 || \
    "$A" push "$IMG" /tmp/test-recovery.img >/dev/null
src=/data/local/tmp/test-recovery.img
"$A" shell "[ -f $src ]" || src=/tmp/test-recovery.img
"$A" shell "$SU 'dd if=$src of=/dev/block/by-name/recovery bs=4194304 conv=fsync 2>/dev/null; sync; rm $src'"
got=$("$A" shell "$SU 'sha1sum /dev/block/by-name/recovery'" | cut -c1-40)
if [ "$got" != "$want" ]; then
    echo "recovery partition does not match the image ($got != $want); not rebooting" >&2
    exit 1
fi
echo "recovery partition = $IMG ($want)"

# Bootloader message: command "boot-recovery", the status field after it zero.
# Built on the host and pushed as a file: TWRP's busybox dd splits a short
# read from a pipe across two blocks.
BCB=$(mktemp)
python3 -I -c 'import sys; sys.stdout.buffer.write(b"boot-recovery".ljust(64, b"\0"))' > "$BCB"
"$A" push "$BCB" /data/local/tmp/bcb.bin >/dev/null 2>&1 || "$A" push "$BCB" /tmp/bcb.bin >/dev/null
rm -f "$BCB"
bcb=/data/local/tmp/bcb.bin
"$A" shell "[ -f $bcb ]" || bcb=/tmp/bcb.bin
"$A" shell "$SU 'dd if=$bcb of=/dev/block/by-name/misc bs=64 count=1 conv=notrunc 2>/dev/null; sync; rm $bcb'"
got=$("$A" shell "$SU 'dd if=/dev/block/by-name/misc bs=64 count=1 2>/dev/null | od -A n -c'" | tr -d ' \n')
if [ "$got" != 'boot-recovery\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0*' ] && [ "${got:0:13}" != "boot-recovery" ]; then
    echo "misc command not written as expected: $got" >&2
    exit 1
fi
echo "misc command: ${got:0:13}"
echo "[$(date +%H:%M:%S)] plain reboot"
"$A" shell "$SU 'reboot'" || true
