#!/bin/bash
# Build a recovery image that boots XNU on the A34:
#   our loader (a34boot.c) + SWEETXNU payload (kernel, A34 ADT, boot-args),
#   packed into the stock DYG1 recovery layout and signed like TWRP.
#
#   build-xnu.sh KERNEL [CMDLINE]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P="$ROOT/tools/payload"
OUT="$ROOT/build"
SWEET="$ROOT/third_party/ios-sweet/tools"
KERNEL=${1:?usage: build-xnu.sh KERNEL [CMDLINE]}
CMDLINE=${2:-"debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=ufsroot0 ufsroot=userdata ufslun=2 sweethw=0 sweetgt=0 wdt=-1 wdt_test=no_user_checkin launchd_unsecure_cache=1 unentitled_ios_sim_launch=1 ignition_level=1"}
CC=aarch64-linux-gnu-gcc-11
mkdir -p "$OUT"

CFLAGS="-O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fno-pie
        -mcmodel=small -mgeneral-regs-only -mstrict-align -nostdlib -fno-stack-protector
        -fno-asynchronous-unwind-tables -Wall -Werror"
$CC $CFLAGS -c -o "$P/shim-start.o" "$P/shim-start.S"
$CC $CFLAGS -c -o "$P/a34boot.o" "$P/a34boot.c"
$CC $CFLAGS -c -o "$P/a34ufs.o" "$P/a34ufs.c"
aarch64-linux-gnu-ld -T "$P/shim.ld" -o "$P/a34boot.elf" "$P/shim-start.o" "$P/a34boot.o" "$P/a34ufs.o"
if aarch64-linux-gnu-readelf -r "$P/shim-start.o" "$P/a34boot.o" "$P/a34ufs.o" | grep -qE "R_AARCH64_(ABS|MOVW_UABS)"; then
    echo "absolute relocation found; loader is not position independent" >&2
    exit 1
fi
aarch64-linux-gnu-objcopy -O binary "$P/a34boot.elf" "$P/a34boot.bin"

python3 -I "$ROOT/tools/mkadt_a34.py" "$OUT/a34.adt"
python3 -I "$SWEET/mkpayload.py" "$KERNEL" "$OUT/a34.adt" "$OUT/xnu-payload.bin" --cmdline "$CMDLINE"

python3 -I - "$P/a34boot.bin" "$OUT/xnu-payload.bin" "$OUT/xnu-blob.bin" <<'EOF'
import struct, sys
shim, payload = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
assert len(shim) <= 0xC000, len(shim)
assert payload[:8] == b"SWEETXNU"
blob = bytearray(shim + b"\0" * (0x10000 - len(shim)) + payload)
struct.pack_into("<Q", blob, 16, len(blob))     # image_size
struct.pack_into("<Q", blob, 64, len(payload))  # shim_payload_size (informational here)
open(sys.argv[3], "wb").write(blob)
print("loader %d bytes, payload %d bytes, blob %d bytes" % (len(shim), len(payload), len(blob)))
EOF

python3 -I "$ROOT/tools/mkrecovery.py" "$ROOT/firmware/A346EXXSADYG1_TUR/images/recovery.img" \
    "$OUT/xnu-blob.bin" "$OUT/xnu-recovery.img"
python3 -I "$ROOT/ref/avb/avbtool.py" add_hash_footer \
    --image "$OUT/xnu-recovery.img" --partition_name recovery --partition_size 67108864 \
    --key "$ROOT/ref/avb/testkey_rsa4096.pem" --algorithm SHA256_RSA4096
V=$(mktemp -d)
ln -s "$OUT/xnu-recovery.img" "$V/recovery.img"
python3 -I "$ROOT/ref/avb/avbtool.py" verify_image --image "$V/recovery.img" \
    --key "$ROOT/ref/avb/testkey_rsa4096.pem" | tail -1
rm -rf "$V"
echo "cmdline: $CMDLINE"
sha1sum "$OUT/xnu-recovery.img"
