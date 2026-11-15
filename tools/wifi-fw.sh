#!/bin/bash
# The Wi-Fi firmware for /a34/wifi, taken from the phone's own Samsung firmware (never kept in
# this repository): docs/wifi-plan.md.
#
#   tools/wifi-fw.sh SUPER_IMG OUT_DIR
#
# SUPER_IMG is super.img of the firmware the phone runs (A346EXXSADYG1: from the AP tar, lz4 -d;
# sparse or raw). The vendor partition is cut out of it (its LP metadata), mounted read-only
# (EROFS; needs sudo) and these are copied to OUT_DIR:
#   soc5_0_ram_wmmcu_1_1_hdr.bin   the Wi-Fi MCU's RAM image (into CONNSYS's memory)
#   WIFI_RAM_CODE_soc5_0_1_1.bin   the Wi-Fi firmware (part DMA download, part memory)
#   WIFI, wifi.cfg                 the default NVRAM and the driver's settings (read, not yet used)
# The "1c" variants are for the single-antenna build (device tree flavor "c"); the A34's
# wifi node has no flavor, so the "1_1" files are the ones.
#
# Then, with a34wifi from the a34wifi workflow's artifact copied into OUT_DIR as well:
#   sudo tools/root-add-job.py /mnt com.a34.wifi /a34/wifi OUT_DIR a34wifi up 7
set -euo pipefail

SUPER=${1:?usage: tools/wifi-fw.sh SUPER_IMG OUT_DIR}
OUT=${2:?usage: tools/wifi-fw.sh SUPER_IMG OUT_DIR}
FILES=(soc5_0_ram_wmmcu_1_1_hdr.bin WIFI_RAM_CODE_soc5_0_1_1.bin WIFI wifi.cfg)
# What A346EXXSADYG1's vendor has (a different firmware may differ: a warning, not an error)
declare -A SHA=(
	[soc5_0_ram_wmmcu_1_1_hdr.bin]=55c37820dba5674e3f37266f91dfd05e3e81b4677a96e9c4878dd8641b76efa1
	[WIFI_RAM_CODE_soc5_0_1_1.bin]=208ab3c6e34633ffed00879adc9a912f9dd6c0cd64a258b96437c5cf43165bea
	[WIFI]=40f43f812a1fe1d0d097336cfcee43cfe0e9acfc0d67ca4bbea0f47006ae8461
	[wifi.cfg]=dbf130af5667c3961e88766378ac75e9b5693d504619ffecf3711ed3036f206c
)

TMP=$(mktemp -d)
MNT=$TMP/mnt
cleanup() {
	mountpoint -q "$MNT" 2>/dev/null && sudo umount "$MNT"
	rm -rf "$TMP"
}
trap cleanup EXIT

RAW=$SUPER
if [ "$(head -c 4 "$SUPER" | od -An -tx1 | tr -d ' ')" = "3aff26ed" ]; then
	echo "sparse image: simg2img to $TMP (needs ~$(($(stat -c %s "$SUPER") / 1000000000 + 2)) GB)"
	RAW=$TMP/super.raw
	simg2img "$SUPER" "$RAW"
fi

echo "vendor partition out of the LP metadata"
python3 -I - "$RAW" "$TMP/vendor.img" <<'EOF'
import struct, sys
raw, out = sys.argv[1], sys.argv[2]
f = open(raw, 'rb')
f.seek(4096)
geo = f.read(4096)
assert struct.unpack_from('<I', geo, 0)[0] == 0x616c4467, 'no LP geometry at 4096'
f.seek(3 * 4096)                       # primary metadata, slot 0
hdr = f.read(4096)
assert struct.unpack_from('<I', hdr, 0)[0] == 0x414c5030, 'no LP metadata header'
hsz = struct.unpack_from('<I', hdr, 8)[0]
tsz = struct.unpack_from('<I', hdr, 44)[0]
parts, exts = struct.unpack_from('<III', hdr, 80), struct.unpack_from('<III', hdr, 92)
f.seek(3 * 4096 + hsz)
t = f.read(tsz)
for i in range(parts[1]):
    o = parts[0] + i * parts[2]
    name = t[o:o + 36].split(b'\0')[0].decode()
    first, num = struct.unpack_from('<II', t, o + 40)
    if name != 'vendor':
        continue
    with open(out, 'wb') as w:
        for j in range(first, first + num):
            nsec, ttype, tdata = struct.unpack_from('<QIQ', t, exts[0] + j * exts[2])
            assert ttype == 0, 'vendor extent is not linear'
            f.seek(tdata * 512)
            left = nsec * 512
            while left:
                b = f.read(min(left, 1 << 24))
                w.write(b)
                left -= len(b)
    print('vendor: %d extent(s)' % num)
    break
else:
    sys.exit('no vendor partition')
EOF
[ "$RAW" = "$SUPER" ] || rm -f "$RAW"

mkdir -p "$MNT" "$OUT"
sudo mount -o loop,ro "$TMP/vendor.img" "$MNT"
for f in "${FILES[@]}"; do
	sudo cat "$MNT/firmware/$f" >"$OUT/$f"
	got=$(sha256sum "$OUT/$f" | cut -d' ' -f1)
	if [ "$got" = "${SHA[$f]}" ]; then
		echo "$f: $(stat -c %s "$OUT/$f") bytes, as A346EXXSADYG1's"
	else
		echo "$f: $(stat -c %s "$OUT/$f") bytes, NOT A346EXXSADYG1's ($got): check docs/wifi-plan.md"
	fi
done
echo "done: $OUT (firmware of Samsung/MediaTek: keep it out of git)"
