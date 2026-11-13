#!/bin/bash
# A34Metal.bundle -- HackiOS's Metal driver for the Mali (redwoodmetal.m over metal/*.cpp, MIT),
# loaded by Metal.framework as its GPU driver -- and its tests, for the iOS 16 user space, built
# on a Mac with Xcode after userland/mesa/build.sh. See README.md; used by
# .github/workflows/a34metal.yml.
#
#   MESA_OUT=.../out userland/a34metal/build.sh
#
# MESA_OUT is userland/mesa/build.sh's out/: its libgallium carries the driver's compiler
# library (the AIR frontend and air_metal.c, patches 0006/0007), which the plug-in dlopens as
# /a34/libgallium-26.3.0.dylib. Makes $A34METAL_OUT (default $WORK/a34metal-out):
#   A34Metal.bundle/   -> /System/Library/Extensions/A34Metal.bundle (A34Metal, Info.plist)
#   a34/               -> /a34: Mesa's four dylibs and mali-tri (as userland/mesa builds them, with
#                         the Metal library inside libgallium), metal_drv_test, metal-probe,
#                         metal_props
#   README.md, BUILD_INFO.txt, dSYM/ (debug symbols, not for the phone)
# Everything is arm64 for iOS $IOS_MIN, refers to /a34 and the system only, has no LC_RPATH and
# is ad-hoc signed last.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
IOS_MIN=${IOS_MIN:-16.0}
INSTALL_DIR=${INSTALL_DIR:-/a34}
WORK=${WORK:-$HERE/work}
MESA_OUT=${MESA_OUT:-$WORK/out}
OUT=${A34METAL_OUT:-$WORK/a34metal-out}
BUILD=$WORK/a34metal-build
AIR=$HERE/../mesa/air

IOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CLANG=$(xcrun -f clang)
CLANGXX=$(xcrun -f clang++)
TARGET=(-target "arm64-apple-ios$IOS_MIN" -isysroot "$IOS_SDK")
WARN=(-Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-deprecated-declarations
	-Wno-unguarded-availability-new -Wno-objc-method-access -Wno-incomplete-implementation -Wno-protocol)
CC=("$CLANG" "${TARGET[@]}" -O2 -g "${WARN[@]}")
CXX=("$CLANGXX" "${TARGET[@]}" -O2 -g -std=c++20 -fno-exceptions -fno-rtti "${WARN[@]}")
INCLUDES=(-I"$HERE" -I"$HERE/metal" -I"$AIR")
A=$OUT/a34

log() { printf '\n=== %s\n' "$*"; }

[ -f "$MESA_OUT/libgallium-${MESA_VERSION:-26.3.0}.dylib" ] ||
	{ echo "no Mesa in $MESA_OUT (userland/mesa/build.sh first)"; exit 1; }
[ -f "$AIR/air_metal.h" ] || { echo "no $AIR/air_metal.h"; exit 1; }

rm -rf "$BUILD" "$OUT"
mkdir -p "$BUILD/metal" "$OUT/A34Metal.bundle" "$A" "$OUT/dSYM"
echo "clang: $("$CLANG" --version | head -n 1)"
echo "iPhoneOS SDK $(xcrun --sdk iphoneos --show-sdk-version): $IOS_SDK"
plutil -lint "$HERE/Info.plist"

log "the driver's core (metal/*.cpp, C++20)"
for f in "$HERE"/metal/*.cpp; do
	name=$(basename "$f" .cpp)
	"${CXX[@]}" "${INCLUDES[@]}" -c "$f" -o "$BUILD/metal/$name.o" 2>&1 | tee -a "$BUILD/core.log"
	[ "${PIPESTATUS[0]}" = 0 ]
done
echo "compiler warnings: $(grep -c 'warning:' "$BUILD/core.log" || true)"

log "A34Metal (the plug-in: redwoodmetal.m over the core), an MH_BUNDLE"
"${CC[@]}" -fno-objc-arc "${INCLUDES[@]}" -bundle -o "$BUILD/A34Metal" "$HERE/redwoodmetal.m" "$BUILD"/metal/*.o \
	-framework Foundation -framework IOKit -framework IOSurface -framework Metal -framework CoreFoundation \
	-lobjc -lc++ 2>&1 | tee "$BUILD/plugin.log"
[ "${PIPESTATUS[0]}" = 0 ]
echo "compiler warnings: $(grep -c 'warning:' "$BUILD/plugin.log" || true)"

log "metal_drv_test (the driver's test, HackiOS's), with a34_watch"
"${CC[@]}" -fno-objc-arc -o "$BUILD/metal_drv_test" "$HERE/metal_drv_test.m" "$HERE/a34_watch.c" \
	-framework Foundation -framework IOKit -framework IOSurface -framework Metal -lobjc 2>&1 | tee "$BUILD/drvtest.log"
[ "${PIPESTATUS[0]}" = 0 ]
echo "compiler warnings: $(grep -c 'warning:' "$BUILD/drvtest.log" || true)"

log "metal-probe and metal_props"
"${CC[@]}" -fno-objc-arc -I"$HERE" -o "$BUILD/metal-probe" "$HERE/metal-probe.m" "$HERE/a34_watch.c" \
	-framework Foundation -framework Metal -framework IOKit -framework CoreFoundation -lobjc
"${CC[@]}" -o "$BUILD/metal_props" "$HERE/metal_props.c" -framework CoreFoundation -framework IOKit

log "package"
for f in A34Metal metal_drv_test metal-probe metal_props; do
	[ -d "$BUILD/$f.dSYM" ] && mv "$BUILD/$f.dSYM" "$OUT/dSYM/"
	codesign -f -s - "$BUILD/$f"            # (last: nothing changes a load command after this)
done
cp "$BUILD/A34Metal" "$HERE/Info.plist" "$OUT/A34Metal.bundle/"
cp "$BUILD/metal_drv_test" "$BUILD/metal-probe" "$BUILD/metal_props" "$A/"
cp "$MESA_OUT"/*.dylib "$MESA_OUT/mali-tri" "$A/"
cp "$HERE/README.md" "$OUT/"

log "check"
bad=0
for f in "$OUT/A34Metal.bundle/A34Metal" "$A"/*; do
	name=$(basename "$f")
	if otool -L "$f" | awk 'NR > 1 { print $1 }' | grep -v -E "^($INSTALL_DIR/|/usr/lib/|/System/Library/)"; then
		echo "error: $name refers to a library outside $INSTALL_DIR and the system"
		bad=1
	fi
	if otool -l "$f" | grep -E '^ *cmd LC_RPATH$' >/dev/null || otool -l "$f" | grep -E '^ *name @' >/dev/null; then
		echo "error: $name has an LC_RPATH or an @ path"
		bad=1
	fi
	[ "$(lipo -archs "$f")" = arm64 ] || { echo "error: $name is $(lipo -archs "$f"), not arm64"; bad=1; }
	codesign -v "$f" || { echo "error: $name: signature"; bad=1; }
done
otool -hv "$OUT/A34Metal.bundle/A34Metal" | grep -w BUNDLE >/dev/null || { echo "error: A34Metal is not an MH_BUNDLE"; bad=1; }
nm -u "$OUT/A34Metal.bundle/A34Metal" | grep -F '_OBJC_CLASS_$__MTLDevice' >/dev/null ||
	{ echo "error: A34Metal does not subclass Metal's _MTLDevice"; bad=1; }
strings "$OUT/A34Metal.bundle/A34Metal" | grep -x "$INSTALL_DIR/libgallium-${MESA_VERSION:-26.3.0}.dylib" >/dev/null ||
	{ echo "error: A34Metal does not name $INSTALL_DIR/libgallium as its compiler library"; bad=1; }
for sym in rwa_device_open rwa_compute_create rwa_texture_create rwa_sampler_create rwa_dispatch rwa_render_create; do
	nm -gU "$A/libgallium-${MESA_VERSION:-26.3.0}.dylib" | grep " _$sym\$" >/dev/null ||
		{ echo "error: libgallium lacks $sym, which the plug-in requires"; bad=1; }
done
[ "$(/usr/libexec/PlistBuddy -c 'Print :NSPrincipalClass' "$OUT/A34Metal.bundle/Info.plist")" = RedwoodMetalDevice ] ||
	{ echo "error: the principal class is not RedwoodMetalDevice"; bad=1; }
[ "$bad" = 0 ] && echo ok
[ "$bad" = 0 ]

{
	echo "a34metal: HackiOS's Metal driver for the Mali (A34Metal.bundle), its tests, and Mesa with its"
	echo "compiler library, for iOS 16.7.16 on the Galaxy A34 (Mali-G68)"
	echo
	echo "repository:    ${GITHUB_REPOSITORY:-local} ${GITHUB_SHA:-$(git -C "$HERE" rev-parse HEAD 2>/dev/null || echo '?')}"
	echo "run:           ${GITHUB_SERVER_URL:-}/${GITHUB_REPOSITORY:-}/actions/runs/${GITHUB_RUN_ID:-local}"
	echo "built:         $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "iPhoneOS SDK:  $(xcrun --sdk iphoneos --show-sdk-version); target arm64-apple-ios$IOS_MIN; ad-hoc signed"
	echo
	echo "files:"
	(cd "$OUT" && for f in A34Metal.bundle/* a34/*; do
		printf '  %-34s %9s  %s\n' "$f" "$(stat -f %z "$f")" "$(shasum -a 256 "$f" | cut -d' ' -f1)"
	done)
	echo
	echo "Mesa (userland/mesa/build.sh):"
	sed 's/^/  /' "$MESA_OUT/BUILD_INFO.txt"
} >"$OUT/BUILD_INFO.txt"
cat "$OUT/BUILD_INFO.txt"
