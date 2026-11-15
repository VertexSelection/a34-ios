#!/bin/bash
# Copies MediaTek's Bluetooth firmware for the MT6877 from the phone's own vendor partition into the
# iOS root, where the kernel (patch 0939, boot-arg a34bt=1) reads it: /a34/firmware/. Run on the PC,
# as root, with the iOS root mounted read-write. Nothing is downloaded and nothing is kept in git.
#
#   install-firmware.sh VENDOR_FIRMWARE_DIR IOS_ROOT
#
# VENDOR_FIRMWARE_DIR is the firmware/ directory of the A34's vendor partition (One UI 7, DYG1):
# e.g. a read-only mount of vendor.img taken from super.img of the firmware the phone runs
# (simg2img + lpunpack, then mount -o ro), or a copy of /vendor/firmware. The two files are the
# BGF MCU's patch and the Bluetooth RAM code that Android's bt_drv_6877.ko copies into CONNSYS's
# memory (bt/mt66xx/include/btif/platform_6877.h: BIN_NAME_MCU, BIN_NAME_BT; no "flavor_bin" in the
# A34's device tree, so the "_1_1" names and not "1c").
set -euo pipefail

FILES=(soc5_0_ram_mcu_1_1_hdr.bin soc5_0_ram_bt_1_1_hdr.bin)

[ $# = 2 ] || { sed -n '2,13p' "$0"; exit 1; }
SRC=$1
ROOT=$2
[ -d "$ROOT/System/Library" ] || { echo "$ROOT does not look like the iOS root (no System/Library)"; exit 1; }
for f in "${FILES[@]}"; do
	[ -f "$SRC/$f" ] || { echo "no $SRC/$f"; exit 1; }
done

DEST=$ROOT/a34/firmware
mkdir -p "$DEST"
chmod 755 "$ROOT/a34" "$DEST"
for f in "${FILES[@]}"; do
	cp "$SRC/$f" "$DEST/$f"
	chown 0:0 "$DEST/$f" 2>/dev/null || true
	chmod 644 "$DEST/$f"
	# the 48-byte EMI header: build time (16), platform (4), hw/sw version, EMI address (4), subsystem
	hdr=$(od -A n -t x1 -N 48 "$DEST/$f" | tr -s ' \n' ' ')
	built=$(head -c 14 "$DEST/$f")
	emi=$(echo "$hdr" | awk '{ printf "0x%s%s%s%s", $28, $27, $26, $25 }')
	printf '%-28s %8s bytes  built %s  EMI address %s  sha256 %s\n' "$f" "$(wc -c <"$DEST/$f" | tr -d ' ')" \
		"$built" "$emi" "$( (sha256sum "$DEST/$f" 2>/dev/null || shasum -a 256 "$DEST/$f") | cut -d' ' -f1)"
done
echo "installed into /a34/firmware/ of the iOS root"
