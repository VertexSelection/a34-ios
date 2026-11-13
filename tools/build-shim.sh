#!/bin/bash
# Build the test-shim recovery image: shim (paints the screen, records facts)
# followed by TWRP's kernel, packaged with TWRP's ramdisk/DTB and signed like TWRP.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P="$ROOT/tools/payload"
OUT="$ROOT/build"
TWRP="$ROOT/tools/twrp/twrp-3.7.1_12-2-a34x.img"
CC=aarch64-linux-gnu-gcc-11
mkdir -p "$OUT"

CFLAGS="-O2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fno-pie
        -mcmodel=small -mgeneral-regs-only -mstrict-align -nostdlib -fno-stack-protector
        -fno-asynchronous-unwind-tables -Wall -Werror"
$CC $CFLAGS -c -o "$P/shim-start.o" "$P/shim-start.S"
$CC $CFLAGS -c -o "$P/shim.o" "$P/shim.c"
aarch64-linux-gnu-ld -T "$P/shim.ld" -o "$P/shim.elf" "$P/shim-start.o" "$P/shim.o"

# The blob is loaded at an address LK picks, so absolute relocations would break it.
if aarch64-linux-gnu-readelf -r "$P/shim-start.o" "$P/shim.o" | grep -qE "R_AARCH64_(ABS|MOVW_UABS)"; then
    echo "absolute relocation found; shim is not position independent" >&2
    exit 1
fi
aarch64-linux-gnu-objcopy -O binary "$P/shim.elf" "$P/shim.bin"

# Raw TWRP kernel from the TWRP image.
python3 -I - "$TWRP" "$OUT/twrp-Image" <<'EOF'
import gzip, struct, sys
d = open(sys.argv[1], "rb").read()
ksz, page = struct.unpack_from("<I", d, 8)[0], struct.unpack_from("<I", d, 36)[0]
open(sys.argv[2], "wb").write(gzip.decompress(d[page:page + ksz]))
EOF

python3 -I - "$P/shim.bin" "$OUT/twrp-Image" "$OUT/shim-blob.bin" <<'EOF'
import struct, sys
shim, img = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
assert len(shim) <= 0xC000, len(shim)
assert img[56:60] == b"ARM\x64"
blob = bytearray(shim + b"\0" * (0x10000 - len(shim)) + img)
struct.pack_into("<Q", blob, 16, len(blob))       # image_size
struct.pack_into("<Q", blob, 64, len(img))        # shim_payload_size
open(sys.argv[3], "wb").write(blob)
print("shim %d bytes, payload %d bytes, blob %d bytes" % (len(shim), len(img), len(blob)))
EOF

PLACEHOLDER=$(printf 'x%.0s' $(seq 1 420))
python3 -I "$ROOT/tools/mkrecovery.py" "$TWRP" "$OUT/shim-blob.bin" "$OUT/shim-recovery.img" "a34t=$PLACEHOLDER"

python3 -I "$ROOT/ref/avb/avbtool.py" add_hash_footer \
    --image "$OUT/shim-recovery.img" --partition_name recovery --partition_size 67108864 \
    --key "$ROOT/ref/avb/testkey_rsa4096.pem" --algorithm SHA256_RSA4096
V=$(mktemp -d)
ln -s "$OUT/shim-recovery.img" "$V/recovery.img"
python3 -I "$ROOT/ref/avb/avbtool.py" verify_image --image "$V/recovery.img" \
    --key "$ROOT/ref/avb/testkey_rsa4096.pem" | tail -1
rm -rf "$V"
ls -la "$OUT/shim-recovery.img"
sha1sum "$OUT/shim-recovery.img"
