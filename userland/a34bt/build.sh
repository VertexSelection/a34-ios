#!/bin/bash
# a34bt for the iOS 16 user space (docs/bluetooth-plan.md, milestone 1), built on a Mac with
# Xcode. Used by .github/workflows/a34bt.yml.
#
#   userland/a34bt/build.sh
#
# Makes $A34BT_OUT (default $WORK/a34bt-out):
#   a34/          what goes into /a34 on the phone: a34bt, a34bt-bluetool, a34bt-btd, liba34bt-btd.dylib
#                 (ad-hoc signed, arm64, iOS $IOS_MIN)
#   INSTALL.md    how to put it and the firmware there, and the first test
#   install-firmware.sh
#   BUILD_INFO.txt, dSYM/ (debug symbols, not for the phone)
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
IOS_MIN=${IOS_MIN:-16.0}
INSTALL_DIR=${INSTALL_DIR:-/a34}
WORK=${WORK:-$HERE/work}
OUT=${A34BT_OUT:-$WORK/a34bt-out}
BUILD=$WORK/a34bt-build
A=$OUT/a34

IOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CLANG=$(xcrun -f clang)
CC=("$CLANG" -target "arm64-apple-ios$IOS_MIN" -isysroot "$IOS_SDK" -std=gnu11 -O2 -g -Wall -Wextra -Werror)

log() { printf '\n=== %s\n' "$*"; }

rm -rf "$BUILD" "$OUT"
mkdir -p "$BUILD" "$A" "$OUT/dSYM"
echo "clang: $("$CLANG" --version | head -n 1)"
echo "iPhoneOS SDK $(xcrun --sdk iphoneos --show-sdk-version): $IOS_SDK"

log "a34bt (a34bt.c)"
"${CC[@]}" -o "$A/a34bt" "$HERE/a34bt.c"

log "a34bt-bluetool (bluetool.c: BlueTool's stand-in for bluetoothd)"
"${CC[@]}" -fblocks -o "$A/a34bt-bluetool" "$HERE/bluetool.c"

log "a34bt-btd (btd.c: bluetoothd's wrapper) and liba34bt-btd.dylib (btdlib.c: inserted into bluetoothd)"
"${CC[@]}" -o "$A/a34bt-btd" "$HERE/btd.c"
"${CC[@]}" -fblocks -dynamiclib -install_name "$INSTALL_DIR/liba34bt-btd.dylib" -o "$A/liba34bt-btd.dylib" "$HERE/btdlib.c"

log "package"
mv "$A"/*.dSYM "$OUT/dSYM/" 2>/dev/null || true
cp "$HERE/INSTALL.md" "$HERE/install-firmware.sh" "$OUT/"
for f in "$A/a34bt" "$A/a34bt-bluetool" "$A/a34bt-btd" "$A/liba34bt-btd.dylib"; do
	codesign -f -s - "$f"       # (last: nothing changes a load command after this)
done

log "check"
bad=0
for f in "$A/a34bt" "$A/a34bt-bluetool" "$A/a34bt-btd" "$A/liba34bt-btd.dylib"; do
	name=$(basename "$f")
	if otool -L "$f" | awk 'NR > 1 { print $1 }' | grep -v -E "^($INSTALL_DIR/|/usr/lib/|/System/Library/)"; then
		echo "error: $name refers to a library outside $INSTALL_DIR and the system"
		bad=1
	fi
	if otool -l "$f" | grep -E '^ *cmd LC_RPATH$' >/dev/null; then
		echo "error: $name has an LC_RPATH"
		bad=1
	fi
	[ "$(lipo -archs "$f")" = arm64 ] || { echo "error: $name is $(lipo -archs "$f"), not arm64"; bad=1; }
	codesign -v "$f" || { echo "error: $name: signature"; bad=1; }
	otool -L "$f"
done
for s in com.a34.bt.hci kern.a34bt; do
	strings "$A/a34bt" | grep -x "$s" >/dev/null || { echo "error: a34bt does not name $s"; bad=1; }
done
strings "$A/a34bt-bluetool" | grep -x com.apple.BlueTool >/dev/null || { echo "error: a34bt-bluetool does not name com.apple.BlueTool"; bad=1; }
nm -u "$A/a34bt-bluetool" | grep -q _xpc_connection_create_mach_service || { echo "error: a34bt-bluetool does not import xpc"; bad=1; }
[ "$(otool -D "$A/liba34bt-btd.dylib" | tail -n 1)" = "$INSTALL_DIR/liba34bt-btd.dylib" ] || { echo "error: liba34bt-btd.dylib is not named $INSTALL_DIR/liba34bt-btd.dylib"; bad=1; }
otool -l "$A/liba34bt-btd.dylib" | grep -A2 __interpose | grep -q __interpose || { echo "error: liba34bt-btd.dylib has no __interpose section"; bad=1; }
strings "$A/a34bt-btd" | grep -x "$INSTALL_DIR/liba34bt-btd.dylib" >/dev/null || { echo "error: a34bt-btd does not name the library"; bad=1; }
[ "$bad" = 0 ] && echo ok
[ "$bad" = 0 ]

{
	echo "a34bt: the Galaxy A34's Bluetooth controller from user space (milestone 1), iOS 16.7.16"
	echo
	echo "repository:    ${GITHUB_REPOSITORY:-local} ${GITHUB_SHA:-$(git -C "$HERE" rev-parse HEAD 2>/dev/null || echo '?')}"
	echo "run:           ${GITHUB_SERVER_URL:-}/${GITHUB_REPOSITORY:-}/actions/runs/${GITHUB_RUN_ID:-local}"
	echo "built:         $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "install dir:   $INSTALL_DIR (ad-hoc signed)"
	echo "iPhoneOS SDK:  $(xcrun --sdk iphoneos --show-sdk-version); target arm64-apple-ios$IOS_MIN"
	echo "kernel:        xnu/patches/0939, 0970, 0971; boot-arg a34bt=1 (milestone 1) or a34bt=2 (bluetoothd's bridge)"
	echo
	echo "files (into $INSTALL_DIR):"
	(cd "$A" && for f in *; do
		printf '  %-12s %9s  %s\n' "$f" "$(stat -f %z "$f")" "$(shasum -a 256 "$f" | cut -d' ' -f1)"
	done)
} >"$OUT/BUILD_INFO.txt"
cat "$OUT/BUILD_INFO.txt"
