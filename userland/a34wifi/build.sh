#!/bin/bash
# a34wifi for the iOS 16 user space (README.md, docs/wifi-plan.md), built on a Mac with Xcode.
# Used by .github/workflows/a34wifi.yml.
#
#   userland/a34wifi/build.sh
#
# Makes $A34WIFI_OUT (default $WORK/a34wifi-out):
#   a34/wifi/a34wifi   -> /a34/wifi/a34wifi on the phone (arm64, iOS $IOS_MIN, ad-hoc signed)
#   README.md, BUILD_INFO.txt, dSYM/ (debug symbols, not for the phone)
# The firmware files that go beside it come from the phone's own firmware (tools/wifi-fw.sh),
# never from here.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
IOS_MIN=${IOS_MIN:-16.0}
WORK=${WORK:-$HERE/work}
OUT=${A34WIFI_OUT:-$WORK/a34wifi-out}
IOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CLANG=$(xcrun -f clang)
CC=("$CLANG" -target "arm64-apple-ios$IOS_MIN" -isysroot "$IOS_SDK" -std=gnu11 -O2 -g -Wall -Wextra -Werror)
A=$OUT/a34/wifi

log() { printf '\n=== %s\n' "$*"; }

rm -rf "$OUT"
mkdir -p "$A" "$OUT/dSYM"
echo "clang: $("$CLANG" --version | head -n 1)"
echo "iPhoneOS SDK $(xcrun --sdk iphoneos --show-sdk-version): $IOS_SDK"

log "a34wifi (a34wifi.c)"
"${CC[@]}" -o "$A/a34wifi" "$HERE/a34wifi.c" -framework CoreFoundation
mv "$A"/*.dSYM "$OUT/dSYM/" 2>/dev/null || true

log "host check of the same source (macOS: it builds and says what it is without the kernel side)"
xcrun --sdk macosx clang -std=gnu11 -O2 -Wall -Wextra -Werror -o "$WORK/a34wifi-host" "$HERE/a34wifi.c" \
	-framework CoreFoundation
# (no kern.a34wifi and no /a34/wifi on a Mac: each must say so and fail, not crash)
for args in "" "files" "log" "up 3 1" "scan 1" "mac" "join 1" "dhcp 1" "bogus"; do
	rc=0
	# shellcheck disable=SC2086
	"$WORK/a34wifi-host" $args || rc=$?
	echo "a34wifi $args: exit $rc"
	[ "$rc" -lt 128 ] || { echo "error: a34wifi $args crashed"; exit 1; }
done

log "sign and check"
codesign -f -s - "$A/a34wifi"
codesign -v "$A/a34wifi"
lipo -info "$A/a34wifi"
otool -L "$A/a34wifi"
if otool -l "$A/a34wifi" | grep -q LC_RPATH; then
	echo "error: a34wifi has an LC_RPATH"; exit 1
fi
vtool -show-build "$A/a34wifi" | grep -E 'platform|minos'

cp "$HERE/README.md" "$OUT/"
{
	echo "a34wifi built $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "clang:  $("$CLANG" --version | head -n 1)"
	echo "iPhoneOS SDK: $(xcrun --sdk iphoneos --show-sdk-version); target arm64-apple-ios$IOS_MIN; ad-hoc signed"
	echo "commit: ${GITHUB_SHA:-$(git -C "$HERE" rev-parse HEAD 2>/dev/null || echo unknown)}"
	(cd "$OUT" && shasum -a 256 a34/wifi/a34wifi)
} >"$OUT/BUILD_INFO.txt"
cat "$OUT/BUILD_INFO.txt"
