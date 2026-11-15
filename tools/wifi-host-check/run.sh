#!/bin/bash
# Host check of the Wi-Fi kernel code (docs/wifi-plan.md, section 3), on Linux with gcc:
#
#   tools/wifi-host-check/run.sh FW_DIR
#
# FW_DIR holds the firmware tools/wifi-fw.sh extracts. Takes bsd/dev/a34_wifi.c out of
# xnu/patches/0939-a34-wifi-m1.patch, (1) syntax- and type-checks it with -Werror against
# xnu_mock.h (stand-ins for the XNU interfaces it uses, each checked against xnu-8796.141.3), then
# (2) runs harness.c under ASan/UBSan: the firmware files parsed and placed into buffers standing
# in for CONNSYS's memory, the AP2WF window and the DMA rings, and the results compared byte for
# byte with the files. It is not a build of the kernel (CI's xnu-a34 is) and touches no hardware.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
FW=${1:?usage: tools/wifi-host-check/run.sh FW_DIR}
FW=$(cd "$FW" && pwd)
PATCH=$(ls "$ROOT"/xnu/patches/*-a34-wifi-m1.patch | tail -n 1)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# the new file's lines out of the patch
awk '/^diff --git a\/bsd\/dev\/a34_wifi.c/{f=1; next} /^diff --git/{f=0} f && /^\+/ && !/^\+\+\+/{print substr($0, 2)}' \
	"$PATCH" >"$TMP/a34_wifi.c"
echo "a34_wifi.c: $(wc -l <"$TMP/a34_wifi.c") lines from $(basename "$PATCH")"
cp "$HERE/ruby_hal.h" "$TMP/"
# the XNU headers it includes, each standing for xnu_mock.h (ahead of the host's own on the -I path)
for h in sys/param.h sys/systm.h sys/errno.h sys/sysctl.h sys/vnode.h sys/vnode_internal.h sys/uio.h \
	kern/locks.h kern/clock.h kern/kalloc.h kern/thread.h kern/sched_prim.h machine/machine_routines.h \
	pexpert/pexpert.h libkern/libkern.h; do
	mkdir -p "$TMP/inc/$(dirname "$h")"
	echo '#include "xnu_mock.h"' >"$TMP/inc/$h"
done

echo "== syntax and types (-Werror)"
gcc -fsyntax-only -std=gnu11 -Wall -Wextra -Wshadow -Wmissing-prototypes -Wstrict-prototypes -Wpointer-arith \
	-Wcast-qual -Wformat=2 -Wconversion -Wno-sign-conversion -Werror -DARM64_BOARD_CONFIG_SWEET \
	-I"$TMP/inc" -I"$HERE" -I"$TMP" -include "$HERE/xnu_mock.h" "$TMP/a34_wifi.c"
echo ok

echo "== firmware placement (ASan/UBSan)"
sed -i 's/__asm__ volatile ("dsb sy" ::: "memory");/__asm__ volatile ("" ::: "memory");/' "$TMP/a34_wifi.c"
gcc -std=gnu11 -O1 -g -w -DARM64_BOARD_CONFIG_SWEET -I"$TMP/inc" -I"$HERE" -I"$TMP" -include "$HERE/xnu_mock.h" \
	-fsanitize=address,undefined -fno-sanitize-recover=all -o "$TMP/harness" "$HERE/harness.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP/harness" "$FW"
