#!/bin/bash
# Build the "hello" test recovery image: assemble, package, sign like TWRP, verify.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P="$ROOT/tools/payload"
OUT="$ROOT/build"
mkdir -p "$OUT"

aarch64-linux-gnu-as -o "$P/hello.o" "$P/hello.S"
aarch64-linux-gnu-ld -Ttext=0 -o "$P/hello.elf" "$P/hello.o"
aarch64-linux-gnu-objcopy -O binary "$P/hello.elf" "$P/hello.bin"

python3 -I "$ROOT/tools/mkrecovery.py" \
    "$ROOT/firmware/A346EXXSADYG1_TUR/images/recovery.img" "$P/hello.bin" "$OUT/hello-recovery.img"

python3 -I "$ROOT/ref/avb/avbtool.py" add_hash_footer \
    --image "$OUT/hello-recovery.img" --partition_name recovery --partition_size 67108864 \
    --key "$ROOT/ref/avb/testkey_rsa4096.pem" --algorithm SHA256_RSA4096

# avbtool verify_image looks the hashed partition up as <partition_name>.img
# next to the image, so verify from a directory where that name points at ours.
V=$(mktemp -d)
ln -s "$OUT/hello-recovery.img" "$V/recovery.img"
python3 -I "$ROOT/ref/avb/avbtool.py" verify_image --image "$V/recovery.img" \
    --key "$ROOT/ref/avb/testkey_rsa4096.pem"
rm -rf "$V"
ls -la "$OUT/hello-recovery.img"
sha1sum "$OUT/hello-recovery.img"
