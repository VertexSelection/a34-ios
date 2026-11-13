#!/bin/bash
# A34Metal.bundle (the Metal plug-in) and metal-probe for the iOS 16 user space, built on a Mac
# with Xcode. See README.md; used by .github/workflows/a34metal.yml.
#
#   userland/a34metal/build.sh
#
# Makes $A34METAL_OUT (default $WORK/a34metal-out):
#   A34Metal.bundle/   -> /System/Library/Extensions/A34Metal.bundle on the phone (A34Metal, Info.plist)
#   a34/metal-probe    -> /a34/metal-probe
#   README.md, BUILD_INFO.txt, dSYM/ (debug symbols, not for the phone)
# Both programs are arm64 for iOS $IOS_MIN, refer to system libraries only, have no LC_RPATH
# and are ad-hoc signed last. HOSTTEST=0 skips the check against this Mac's own Metal.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
IOS_MIN=${IOS_MIN:-16.0}
WORK=${WORK:-$HERE/work}
OUT=${A34METAL_OUT:-$WORK/a34metal-out}
BUILD=$WORK/a34metal-build

IOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CLANG=$(xcrun -f clang)
FLAGS=(-fno-objc-arc -O2 -g -Wall -Wextra -Wno-unused-parameter)
IOS=("$CLANG" -target "arm64-apple-ios$IOS_MIN" -isysroot "$IOS_SDK" "${FLAGS[@]}")
HOST=("$CLANG" -isysroot "$(xcrun --sdk macosx --show-sdk-path)" "${FLAGS[@]}")
FRAMEWORKS=(-framework Metal -framework Foundation -framework CoreFoundation -framework IOKit)
# Metal's private base class, if an SDK's Metal.tbd does not list it: looked up when the bundle
# is loaded (Metal exports it: IOGPU and the AGX drivers bind to the same symbol)
LOOKUP=(-Wl,-U,'_OBJC_CLASS_$__MTLDevice' -Wl,-U,'_OBJC_METACLASS_$__MTLDevice')

log() { printf '\n=== %s\n' "$*"; }

# link_plugin LOG COMPILER... : the plug-in, bound to _MTLDevice through the SDK if it can be
link_plugin() {
	local logfile=$1
	shift
	if "$@" >"$logfile" 2>&1; then
		cat "$logfile"
		echo "_MTLDevice: bound through the SDK's Metal.tbd" | tee -a "$logfile"
	else
		cat "$logfile"
		echo "the link failed; again with _MTLDevice looked up at load time"
		"$@" "${LOOKUP[@]}" 2>&1 | tee "$logfile"
		[ "${PIPESTATUS[0]}" = 0 ]
		echo "_MTLDevice: looked up at load time (-U)" | tee -a "$logfile"
	fi
}

rm -rf "$BUILD" "$OUT"
mkdir -p "$BUILD/ios" "$BUILD/host/A34Metal.bundle" "$OUT/A34Metal.bundle" "$OUT/a34" "$OUT/dSYM"

echo "clang: $("$CLANG" --version | head -n 1)"
echo "iPhoneOS SDK $(xcrun --sdk iphoneos --show-sdk-version): $IOS_SDK"
plutil -lint "$HERE/Info.plist"

log "A34Metal (the plug-in: A34Metal.m), arm64 iOS $IOS_MIN"
link_plugin "$BUILD/plugin.log" "${IOS[@]}" -bundle -o "$BUILD/ios/A34Metal" "$HERE/A34Metal.m" "${FRAMEWORKS[@]}" -lobjc
echo "compiler warnings: $(grep -c 'warning:' "$BUILD/plugin.log" || true)"

log "metal-probe (metal-probe.m), arm64 iOS $IOS_MIN"
"${IOS[@]}" -o "$BUILD/ios/metal-probe" "$HERE/metal-probe.m" "${FRAMEWORKS[@]}" -lobjc 2>&1 | tee "$BUILD/probe.log"
[ "${PIPESTATUS[0]}" = 0 ]
echo "compiler warnings: $(grep -c 'warning:' "$BUILD/probe.log" || true)"

if [ "${HOSTTEST:-1}" != 0 ]; then
	log "host: the plug-in against this Mac's Metal ($(sw_vers -productVersion), $(uname -m))"
	link_plugin "$BUILD/host-plugin.log" "${HOST[@]}" -bundle -o "$BUILD/host/A34Metal.bundle/A34Metal" \
		"$HERE/A34Metal.m" "${FRAMEWORKS[@]}" -lobjc
	cp "$HERE/Info.plist" "$BUILD/host/A34Metal.bundle/"
	"${HOST[@]}" -o "$BUILD/host/a34metal-hosttest" "$HERE/hosttest.m" -framework Metal -framework Foundation -lobjc
	"$BUILD/host/a34metal-hosttest" load "$BUILD/host/A34Metal.bundle/A34Metal"
	# (macOS's Metal is not iOS 16.7's: making a device here shows the plug-in's side only)
	"$BUILD/host/a34metal-hosttest" make "$BUILD/host/A34Metal.bundle/A34Metal" ||
		echo "note: 'make' did not pass on this Mac's Metal (not part of the build's verdict)"
fi

log "package"
for f in A34Metal metal-probe; do
	[ -d "$BUILD/ios/$f.dSYM" ] && mv "$BUILD/ios/$f.dSYM" "$OUT/dSYM/"
	codesign -f -s - "$BUILD/ios/$f"        # (signed as plain files: nothing but the Mach-O carries it)
done
cp "$BUILD/ios/A34Metal" "$HERE/Info.plist" "$OUT/A34Metal.bundle/"
cp "$BUILD/ios/metal-probe" "$OUT/a34/"
cp "$HERE/README.md" "$OUT/"

log "check"
bad=0
for f in "$OUT/A34Metal.bundle/A34Metal" "$OUT/a34/metal-probe"; do
	name=$(basename "$f")
	if otool -L "$f" | awk 'NR > 1 { print $1 }' | grep -v -E '^(/usr/lib/|/System/Library/)'; then
		echo "error: $name refers to a library outside the system"
		bad=1
	fi
	if otool -l "$f" | grep -E '^ *cmd LC_RPATH$' >/dev/null || otool -l "$f" | grep -E '^ *name @' >/dev/null; then
		echo "error: $name has an LC_RPATH or an @ path"
		bad=1
	fi
	[ "$(lipo -archs "$f")" = arm64 ] || { echo "error: $name is $(lipo -archs "$f"), not arm64"; bad=1; }
	otool -l "$f" | grep -A 4 'cmd LC_BUILD_VERSION' | grep -i -E '^ *platform (2|ios)$' >/dev/null ||
		{ echo "error: $name is not built for iOS"; otool -l "$f" | grep -A 4 'cmd LC_BUILD_VERSION'; bad=1; }
	# (the signature is the Mach-O's own, made before the file went into the bundle's directory)
	cmp -s "$f" "$BUILD/ios/$name" && codesign -v "$BUILD/ios/$name" || { echo "error: $name: signature"; bad=1; }
done
otool -hv "$OUT/A34Metal.bundle/A34Metal" | grep -w BUNDLE >/dev/null ||
	{ echo "error: A34Metal is not a bundle (MH_BUNDLE)"; otool -hv "$OUT/A34Metal.bundle/A34Metal"; bad=1; }
nm -u "$OUT/A34Metal.bundle/A34Metal" | grep -F '_OBJC_CLASS_$__MTLDevice' >/dev/null ||
	{ echo "error: A34Metal does not refer to Metal's _MTLDevice"; bad=1; }
nm -u "$OUT/a34/metal-probe" | grep -F '_MTLCreateSystemDefaultDevice' >/dev/null ||
	{ echo "error: metal-probe does not call MTLCreateSystemDefaultDevice"; bad=1; }
[ "$(/usr/libexec/PlistBuddy -c 'Print :NSPrincipalClass' "$OUT/A34Metal.bundle/Info.plist")" = A34MetalDevice ] ||
	{ echo "error: Info.plist's principal class is not A34MetalDevice"; bad=1; }
strings "$OUT/A34Metal.bundle/A34Metal" | grep -x 'A34MetalDevice' >/dev/null ||
	{ echo "error: no class A34MetalDevice in the plug-in"; bad=1; }
[ "$bad" = 0 ] && echo ok
[ "$bad" = 0 ]

{
	echo "a34metal: the Metal plug-in (milestone 1: a device) and metal-probe, iOS 16.7.16 on the Galaxy A34"
	echo
	echo "repository:    ${GITHUB_REPOSITORY:-local} ${GITHUB_SHA:-$(git -C "$HERE" rev-parse HEAD 2>/dev/null || echo '?')}"
	echo "run:           ${GITHUB_SERVER_URL:-}/${GITHUB_REPOSITORY:-}/actions/runs/${GITHUB_RUN_ID:-local}"
	echo "built:         $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "iPhoneOS SDK:  $(xcrun --sdk iphoneos --show-sdk-version); target arm64-apple-ios$IOS_MIN; ad-hoc signed"
	echo "_MTLDevice:    $(grep '^_MTLDevice:' "$BUILD/plugin.log" || tail -n 1 "$BUILD/plugin.log")"
	echo
	echo "files:"
	(cd "$OUT" && for f in A34Metal.bundle/A34Metal A34Metal.bundle/Info.plist a34/metal-probe; do
		printf '  %-30s %9s  %s\n' "$f" "$(stat -f %z "$f")" "$(shasum -a 256 "$f" | cut -d' ' -f1)"
	done)
	echo
	for f in A34Metal.bundle/A34Metal a34/metal-probe; do
		echo "$f:"
		otool -L "$OUT/$f" | tail -n +2
	done
} >"$OUT/BUILD_INFO.txt"
cat "$OUT/BUILD_INFO.txt"
