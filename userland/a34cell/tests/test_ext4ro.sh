#!/bin/bash
# ext4ro against images made by mke2fs -d from a generated tree (synthetic test data only):
# ext4 with extents (4 KiB and 1 KiB blocks, 64-bit), ext4 without extents (block maps), a
# directory large enough for an htree, sparse files with many extents (an index node).
#   tests/test_ext4ro.sh WORKDIR CC
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=${1:?workdir}
CC=${2:-cc}
MKE2FS=$(command -v mke2fs || ls /opt/homebrew/opt/e2fsprogs/sbin/mke2fs /usr/local/opt/e2fsprogs/sbin/mke2fs 2>/dev/null | head -n 1 || true)
[ -n "$MKE2FS" ] || { echo "ext4ro: no mke2fs here, skipped"; exit 0; }
rm -rf "$W/tree" "$W"/*.img
mkdir -p "$W/tree/md/NVRAM/NVD_TEST" "$W/tree/md/NVRAM/NVD_DATA" "$W/tree/big"
python3 -I - "$W/tree" <<'PY'
import os, random, sys
t = sys.argv[1]
r = random.Random(7)
def put(p, n):
    with open(os.path.join(t, p), "wb") as f:
        f.write(bytes(r.getrandbits(8) for _ in range(n)))
for n in (0, 1, 1023, 1024, 1025, 4095, 4096, 4097, 70000):
    put("md/NVRAM/NVD_DATA/F%05d" % n, n)
put("md/NVRAM/NVD_TEST/LD00_001", 380)          # random bytes
put("big/three_mb", 3 * 1024 * 1024 + 123)
for i in range(400):                              # htree-sized directory
    put("md/NVRAM/NVD_DATA/E%04d" % i, (i * 37) % 900)
# sparse: 40 islands of data far apart -> many extents (an index node above them)
with open(os.path.join(t, "big/sparse"), "wb") as f:
    for i in range(40):
        f.seek(i * 3 * 65536)
        f.write(bytes(r.getrandbits(8) for _ in range(5000)))
PY
"$CC" -std=c11 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	-fsanitize=address,undefined -o "$W/test_ext4ro" "$HERE/tests/test_ext4ro.c" "$HERE/ext4ro.c"
run() {
	local name=$1; shift
	"$MKE2FS" -q -F -t ext4 "$@" -d "$W/tree" "$W/$name.img" 64M >/dev/null
	"$W/test_ext4ro" "$W/$name.img" "$W/tree"
}
run ext4-4k -b 4096 -O 64bit
run ext4-1k -b 1024
run blockmap -b 4096 -O ^extent,^64bit,^flex_bg
run blockmap-1k -b 1024 -O ^extent,^64bit,^flex_bg
run ext4-nojournal -b 4096 -O ^has_journal
