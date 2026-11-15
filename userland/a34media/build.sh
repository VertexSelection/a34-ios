#!/bin/bash
# a34media for the iOS 16 user space: liba34media.dylib (in mediaserverd), RedwoodAudio.driver (the
# HAL's null audio devices) and liba34gl-inject.dylib (the loader the kernel inserts, which opens
# liba34media in mediaserverd: userland/a34gl/inject.c, A34_LOAD). Built on a Mac with Xcode; used by
# .github/workflows/a34media.yml. Nothing of Mesa's is needed.
#
#   userland/a34media/build.sh
#
# Makes $A34MEDIA_OUT (default $WORK/a34media-out), laid out by where the files go on the root:
#   a34/liba34media.dylib, a34/liba34gl-inject.dylib                         -> /a34
#   System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver/{Info.plist,RedwoodAudio}
#   INSTALL.md, BUILD_INFO.txt, dSYM/ (debug symbols, not for the phone)
# Install names are absolute ($INSTALL_DIR/<file>), nothing has an LC_RPATH, every Mach-O is
# ad-hoc signed last.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
GL=$HERE/../a34gl
IOS_MIN=${IOS_MIN:-16.0}
INSTALL_DIR=${INSTALL_DIR:-/a34}
WORK=${WORK:-$HERE/work}
A34MEDIA_OUT=${A34MEDIA_OUT:-$WORK/a34media-out}
BUILD=$WORK/a34media-build
HAL=System/Library/Audio/Plug-Ins/HAL
DRIVER=$A34MEDIA_OUT/$HAL/RedwoodAudio.driver
A=$A34MEDIA_OUT/a34

IOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CLANG=$(xcrun -f clang)
CC=("$CLANG" -target "arm64-apple-ios$IOS_MIN" -isysroot "$IOS_SDK" -std=gnu11 -O2 -g -Wall -Wextra
	-fvisibility=hidden -DA34GL_DIR="\"$INSTALL_DIR\"")

log() { printf '\n=== %s\n' "$*"; }

rm -rf "$BUILD" "$A34MEDIA_OUT"
mkdir -p "$BUILD" "$A" "$DRIVER" "$A34MEDIA_OUT/dSYM"

log "liba34media.dylib (inside mediaserverd: media.c)"
"${CC[@]}" -Werror -dynamiclib -install_name "$INSTALL_DIR/liba34media.dylib" \
	-o "$BUILD/liba34media.dylib" "$HERE/media.c" -framework CoreFoundation -framework IOKit -lobjc

log "RedwoodAudio.driver (the HAL's null devices: audio_plugin.c)"
"${CC[@]}" -bundle -o "$BUILD/RedwoodAudio" "$HERE/audio_plugin.c" -framework CoreFoundation 2>&1 | tee "$BUILD/RedwoodAudio.log"
[ "${PIPESTATUS[0]}" = 0 ]
echo "compiler warnings: $(grep -c 'warning:' "$BUILD/RedwoodAudio.log" || true)"
plutil -lint "$HERE/Info.plist"

log "liba34gl-inject.dylib (the kernel's a34inject library, with A34_LOAD: ../a34gl/inject.c)"
"${CC[@]}" -I"$GL" -I"$HERE/../mesa/redwood-drm" -Werror -dynamiclib -install_name "$INSTALL_DIR/liba34gl-inject.dylib" \
	-o "$BUILD/liba34gl-inject.dylib" "$GL/inject.c" "$GL/log.c" -framework CoreFoundation -framework IOKit

log "sign, package"
for f in "$BUILD/liba34media.dylib" "$BUILD/liba34gl-inject.dylib" "$BUILD/RedwoodAudio"; do
	codesign -f -s - "$f"           # (last: nothing changes a load command after this)
done
mv "$BUILD"/*.dSYM "$A34MEDIA_OUT/dSYM/" 2>/dev/null || true
cp "$BUILD/liba34media.dylib" "$BUILD/liba34gl-inject.dylib" "$A/"
cp "$BUILD/RedwoodAudio" "$DRIVER/RedwoodAudio"
cp "$HERE/Info.plist" "$DRIVER/Info.plist"
cp "$HERE/INSTALL.md" "$A34MEDIA_OUT/"

log "check"
bad=0
for f in "$A"/*.dylib "$DRIVER/RedwoodAudio"; do
	name=$(basename "$f")
	if otool -L "$f" | awk 'NR > 1 { print $1 }' | grep -v -E "^($INSTALL_DIR/|/usr/lib/|/System/Library/)"; then
		echo "error: $name refers to a library outside $INSTALL_DIR and the system"
		bad=1
	fi
	case "$name" in
	*.dylib)
		[ "$(otool -D "$f" | tail -n 1)" = "$INSTALL_DIR/$name" ] || { echo "error: $name is not named $INSTALL_DIR/$name"; bad=1; } ;;
	*)
		otool -hv "$f" | grep BUNDLE >/dev/null || { echo "error: $name is not a bundle (MH_BUNDLE)"; bad=1; } ;;
	esac
	if otool -l "$f" | grep -E '^ *cmd LC_RPATH$' >/dev/null || otool -l "$f" | grep -E '^ *name @' >/dev/null; then
		echo "error: $name has an LC_RPATH or an @ path"
		bad=1
	fi
	[ "$(lipo -archs "$f")" = arm64 ] || { echo "error: $name is $(lipo -archs "$f"), not arm64"; bad=1; }
	# (the signature of the file as signed: inside the bundle codesign -v would want the bundle signed, with
	# a _CodeSignature/CodeResources the HAL does not ask for)
	codesign -v "$BUILD/$name" || { echo "error: $name: signature"; bad=1; }
	cmp -s "$BUILD/$name" "$f" || { echo "error: $name is not the file signed"; bad=1; }
done
nm -gU "$DRIVER/RedwoodAudio" | grep ' _RedwoodAudio_Create$' >/dev/null || { echo "error: RedwoodAudio does not export RedwoodAudio_Create"; bad=1; }
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$DRIVER/Info.plist")" = RedwoodAudio ] ||
	{ echo "error: Info.plist's CFBundleExecutable is not RedwoodAudio"; bad=1; }
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFPlugInFactories:7C3E5B0A-52D4-4F6B-9E0D-6B1A2D7F0C51' "$DRIVER/Info.plist")" = RedwoodAudio_Create ] ||
	{ echo "error: Info.plist's factory is not RedwoodAudio_Create"; bad=1; }
for s in "A34_LOAD" "liba34media.dylib"; do
	strings "$A/liba34gl-inject.dylib" | grep "$s" >/dev/null || { echo "error: liba34gl-inject.dylib has no $s"; bad=1; }
done
[ "$bad" = 0 ] && echo ok
[ "$bad" = 0 ]

{
	echo "a34media: mediaserverd on the Galaxy A34 (iOS 16.7.16): liba34media, RedwoodAudio.driver, liba34gl-inject"
	echo
	echo "repository:    ${GITHUB_REPOSITORY:-local} ${GITHUB_SHA:-$(git -C "$HERE" rev-parse HEAD 2>/dev/null || echo '?')}"
	echo "run:           ${GITHUB_SERVER_URL:-}/${GITHUB_REPOSITORY:-}/actions/runs/${GITHUB_RUN_ID:-local}"
	echo "built:         $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "iPhoneOS SDK:  $(xcrun --sdk iphoneos --show-sdk-version); target arm64-apple-ios$IOS_MIN"
	echo
	echo "files (by their place on the root):"
	(cd "$A34MEDIA_OUT" && find a34 System -type f | sort | while read -r f; do
		printf '  /%-62s %9s  %s\n' "$f" "$(stat -f %z "$f")" "$(shasum -a 256 "$f" | cut -d' ' -f1)"
	done)
	echo
	for f in "$A"/*.dylib "$DRIVER/RedwoodAudio"; do
		echo "$(basename "$f"):"
		otool -L "$f" | tail -n +2
	done
} >"$A34MEDIA_OUT/BUILD_INFO.txt"
cat "$A34MEDIA_OUT/BUILD_INFO.txt"
