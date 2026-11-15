#!/bin/bash
# Host check of the Wi-Fi kernel code (docs/wifi-plan.md, section 3), on Linux with gcc:
#
#   tools/wifi-host-check/run.sh FW_DIR
#
# FW_DIR holds the firmware tools/wifi-fw.sh extracts. Rebuilds bsd/dev/a34_wifi.c from the
# xnu/patches that touch it (0940, 0960, ...), (1) syntax- and type-checks it with -Werror against
# xnu_mock.h (stand-ins for the XNU interfaces it uses, each checked against xnu-8796.141.3), then
# (2) runs harness.c under ASan/UBSan: the firmware files parsed and placed into buffers standing
# in for CONNSYS's memory, the AP2WF window and the DMA rings, and the results compared byte for
# byte with the files. It is not a build of the kernel (CI's xnu-a34 is) and touches no hardware.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
FW=${1:?usage: tools/wifi-host-check/run.sh FW_DIR}
FW=$(cd "$FW" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# bsd/dev/a34_wifi.c and a34_connsys.c as the patches leave them: every patch that touches one, in order
mkdir -p "$TMP/tree"
git -C "$TMP/tree" init -q
for p in "$ROOT"/xnu/patches/*.patch; do
	for f in a34_wifi a34_connsys; do
		if grep -q "^+++ b/bsd/dev/$f.c" "$p"; then
			git -C "$TMP/tree" apply --include="bsd/dev/$f.c" "$p"
			echo "$f.c: applied $(basename "$p")"
		fi
	done
done
cp "$TMP/tree/bsd/dev/a34_wifi.c" "$TMP/tree/bsd/dev/a34_connsys.c" "$TMP/"
echo "a34_wifi.c: $(wc -l <"$TMP/a34_wifi.c") lines, a34_connsys.c: $(wc -l <"$TMP/a34_connsys.c") lines"
cp "$HERE/ruby_hal.h" "$TMP/"
# the XNU headers it includes, each standing for xnu_mock.h (ahead of the host's own on the -I path)
for h in sys/param.h sys/systm.h sys/errno.h sys/sysctl.h sys/vnode.h sys/vnode_internal.h sys/uio.h \
	kern/locks.h kern/clock.h kern/kalloc.h kern/thread.h kern/sched_prim.h machine/machine_routines.h \
	pexpert/pexpert.h libkern/libkern.h; do
	mkdir -p "$TMP/inc/$(dirname "$h")"
	echo '#include "xnu_mock.h"' >"$TMP/inc/$h"
done

echo "== syntax and types (-Werror)"
for f in a34_wifi a34_connsys; do
	gcc -fsyntax-only -std=gnu11 -Wall -Wextra -Wshadow -Wmissing-prototypes -Wstrict-prototypes -Wpointer-arith \
		-Wcast-qual -Wformat=2 -Wconversion -Wno-sign-conversion -Werror -DARM64_BOARD_CONFIG_SWEET \
		-I"$TMP/inc" -I"$HERE" -I"$TMP" -include "$HERE/xnu_mock.h" "$TMP/$f.c"
	echo "$f.c ok"
done

echo "== firmware placement (ASan/UBSan)"
sed -i 's/__asm__ volatile ("dsb sy" ::: "memory");/__asm__ volatile ("" ::: "memory");/' "$TMP/a34_wifi.c"
gcc -std=gnu11 -O1 -g -w -DARM64_BOARD_CONFIG_SWEET -I"$TMP/inc" -I"$HERE" -I"$TMP" -include "$HERE/xnu_mock.h" \
	-fsanitize=address,undefined -fno-sanitize-recover=all -o "$TMP/harness" "$HERE/harness.c"
ASAN_OPTIONS=detect_leaks=0 "$TMP/harness" "$FW"
