#!/bin/bash
# Build a `boot`-partition image that starts XNU on the A34 through LK's NORMAL boot path, the
# only path on which LK loads and verifies the modem (md1img) and the audio DSP
# (docs/cellular-plan.md, "Stage 1a"). Same loader and payload as build-xnu.sh, packed as a
# boot image v4 for the 32 MiB `boot` partition, without TWRP's kernel (TWRP stays in `recovery`).
#
#   build-xnu-boot.sh KERNEL "CMDLINE" OUT.img
#
# Everything intermediate goes to a temporary directory; only OUT.img is written (build/'s shared
# files -- xnu-recovery.img, a34.adt, xnu-blob.bin -- are left alone).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P="$ROOT/tools/payload"
SWEET="$ROOT/third_party/ios-sweet/tools"
KERNEL=${1:?usage: build-xnu-boot.sh KERNEL CMDLINE OUT.img}
CMDLINE=${2:?usage: build-xnu-boot.sh KERNEL CMDLINE OUT.img}
OUT_IMG=${3:?usage: build-xnu-boot.sh KERNEL CMDLINE OUT.img}
CC=aarch64-linux-gnu-gcc-11
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

if [ ${#CMDLINE} -gt 600 ]; then
    echo "boot-args are ${#CMDLINE} characters; XNU keeps 607 (README)" >&2
    exit 1
fi

CFLAGS="-O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fno-pie
        -mcmodel=small -mgeneral-regs-only -mstrict-align -nostdlib -fno-stack-protector
        -fno-asynchronous-unwind-tables -Wall -Werror"
$CC $CFLAGS -c -o "$T/shim-start.o" "$P/shim-start.S"
$CC $CFLAGS -c -o "$T/a34boot.o" "$P/a34boot.c"
$CC $CFLAGS -c -o "$T/a34ufs.o" "$P/a34ufs.c"
aarch64-linux-gnu-ld -T "$P/shim.ld" -o "$T/a34boot.elf" "$T/shim-start.o" "$T/a34boot.o" "$T/a34ufs.o"
if aarch64-linux-gnu-readelf -r "$T/shim-start.o" "$T/a34boot.o" "$T/a34ufs.o" | grep -qE "R_AARCH64_(ABS|MOVW_UABS)"; then
    echo "absolute relocation found; loader is not position independent" >&2
    exit 1
fi
aarch64-linux-gnu-objcopy -O binary "$T/a34boot.elf" "$T/a34boot.bin"

# The ADT's /product node (MobileGestalt's identity: without it SpringBoard dies on a UIKit
# asset, "BoundingPathBitmap-1125x2436...") comes from the real iPhone X tree; mkadt_a34.py only
# notes when that file is missing, so check here.
D22AP=${D22AP:-$ROOT/research/ipsw-x/d22ap.adt}
[ -s "$D22AP" ] || { echo "no $D22AP (the iPhone X device tree for /product); stopping" >&2; exit 1; }
python3 -I "$ROOT/tools/mkadt_a34.py" "$T/a34.adt" --d22ap "$D22AP"
python3 -I "$ROOT/tools/mkadt_a34.py" "$T/check.adt" --d22ap "$D22AP" --dump 2>/dev/null | grep -q -E "^\s*product \{" ||
    { echo "the ADT has no /product node; stopping" >&2; exit 1; }
python3 -I "$SWEET/mkpayload.py" "$KERNEL" "$T/a34.adt" "$T/xnu-payload.bin" --cmdline "$CMDLINE"

# Loader, then the XNU payload; no second payload (shim_payload2_size 0: the Volume Up escape
# finds nothing, shows a red bar and goes on to XNU).
python3 -I - "$T/a34boot.bin" "$T/xnu-payload.bin" "$T/xnu-blob.bin" <<'PYEOF'
import struct, sys
shim, payload = (open(f, "rb").read() for f in sys.argv[1:3])
assert len(shim) <= 0xC000, len(shim)
assert payload[:8] == b"SWEETXNU"
payload += b"\0" * (-len(payload) & 15)
blob = bytearray(shim + b"\0" * (0x10000 - len(shim)) + payload)
struct.pack_into("<Q", blob, 16, len(blob))     # image_size
struct.pack_into("<Q", blob, 64, len(payload))  # shim_payload_size: XNU payload
struct.pack_into("<Q", blob, 72, 0)             # shim_payload2_size: none
open(sys.argv[3], "wb").write(blob)
print("loader %d, xnu payload %d, blob %d bytes" % (len(shim), len(payload), len(blob)))
PYEOF

python3 -I "$ROOT/tools/mkboot-v4.py" "$T/xnu-blob.bin" "$OUT_IMG"
size=$(stat -c %s "$OUT_IMG")
[ "$size" = 33554432 ] || { echo "image is $size bytes, expected 33554432 (the boot partition)" >&2; exit 1; }
cp "$OUT_IMG" "$T/boot.img"     # avbtool finds a hash descriptor's image by partition name
python3 -I "$ROOT/ref/avb/avbtool.py" verify_image --image "$T/boot.img" \
    --key "$ROOT/ref/avb/testkey_rsa4096.pem" 2>&1 | grep -q "Successfully verified" ||
    { echo "AVB footer does not verify" >&2; exit 1; }
echo "OK: $OUT_IMG ($size bytes, boot image v4, AVB footer verified), boot-args ${#CMDLINE} characters"
