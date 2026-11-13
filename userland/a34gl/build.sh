#!/bin/bash
# liba34gl for the iOS 16 user space: CoreAnimation's OpenGL ES through Mesa's panfrost, built on
# a Mac with Xcode after userland/mesa/build.sh (see README.md). Used by
# .github/workflows/a34gl.yml.
#
#   MESA_OUT=.../out MESA_SRC=.../mesa-26.3.0 userland/a34gl/build.sh
#
# MESA_OUT is userland/mesa/build.sh's out/ (the dylibs, named $INSTALL_DIR/<file>, and
# include/), MESA_SRC the Mesa tree it built (for the Khronos registry, gl.xml). Makes
# $A34GL_OUT (default $WORK/a34gl-out):
#   a34/          what goes into /a34 on the phone: Mesa's four dylibs and mali-tri, and
#                 liba34gl.dylib, liba34gl-inject.dylib, a34gl-bbd, a34gl-surftest,
#                 backboardd.env.example
#   INSTALL.md    how to put it there and into backboardd
#   BUILD_INFO.txt, dSYM/ (debug symbols, not for the phone)
# Install names are absolute ($INSTALL_DIR/<file>), nothing has an LC_RPATH, and everything
# is ad-hoc signed last.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
IOS_MIN=${IOS_MIN:-16.0}
INSTALL_DIR=${INSTALL_DIR:-/a34}
WORK=${WORK:-$HERE/work}
MESA_OUT=${MESA_OUT:-$WORK/out}
MESA_SRC=${MESA_SRC:-$WORK/mesa-${MESA_VERSION:-26.3.0}}
A34GL_OUT=${A34GL_OUT:-$WORK/a34gl-out}
BUILD=$WORK/a34gl-build

IOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CLANG=$(xcrun -f clang)
CC=("$CLANG" -target "arm64-apple-ios$IOS_MIN" -isysroot "$IOS_SDK" -std=gnu11 -O2 -g -Wall -Wextra
	-fvisibility=hidden -DA34GL_DIR="\"$INSTALL_DIR\"")
INCLUDES=(-I"$BUILD" -I"$HERE" -I"$HERE/../mesa/redwood-drm" -I"$MESA_OUT/include")
A=$A34GL_OUT/a34

log() { printf '\n=== %s\n' "$*"; }

[ -f "$MESA_OUT/libGLESv2.2.dylib" ] || { echo "no Mesa in $MESA_OUT (userland/mesa/build.sh first)"; exit 1; }
GL_XML=
for f in "$MESA_SRC/src/mesa/glapi/glapi/registry/gl.xml" "$MESA_SRC/src/mapi/glapi/registry/gl.xml"; do
	[ -f "$f" ] && { GL_XML=$f; break; }
done
[ -n "$GL_XML" ] || { echo "no gl.xml (the Khronos registry) under $MESA_SRC"; exit 1; }

rm -rf "$BUILD" "$A34GL_OUT"
mkdir -p "$BUILD" "$A"

log "eagl_gen.h: the function table of iOS's OpenGLES (eagl_slots.txt), against Mesa's libGLESv2"
nm -gU "$MESA_OUT/libGLESv2.2.dylib" | awk '$3 ~ /^_gl/ { print substr($3, 2) }' | sort -u >"$BUILD/exported-gl.txt"
echo "libGLESv2 exports $(wc -l <"$BUILD/exported-gl.txt") gl* functions"
python3 -I "$HERE/mkeagl.py" "$HERE/eagl_slots.txt" "$GL_XML" "$BUILD/exported-gl.txt" "$HERE/eagl.c" \
	>"$BUILD/eagl_gen.h" 2>"$BUILD/mkeagl.log"
cat "$BUILD/mkeagl.log"

log "liba34gl.dylib (EAGLContext on Mesa: eagl.c, gl_egl.c)"
"${CC[@]}" "${INCLUDES[@]}" -Wno-unused-parameter -dynamiclib -install_name "$INSTALL_DIR/liba34gl.dylib" \
	-Wl,-exported_symbol,_a34gl_function_values -Wl,-exported_symbol,_a34gl_function_refused \
	-o "$A/liba34gl.dylib" "$HERE/eagl.c" "$HERE/gl_egl.c" "$HERE/log.c" \
	"$MESA_OUT/libEGL.1.dylib" "$MESA_OUT/libGLESv2.2.dylib" \
	-framework CoreFoundation -framework IOSurface -framework IOKit -lobjc 2>&1 | tee "$BUILD/liba34gl.log"
[ "${PIPESTATUS[0]}" = 0 ]
echo "compiler warnings: $(grep -c 'warning:' "$BUILD/liba34gl.log" || true)"

log "liba34gl-inject.dylib (the loader in backboardd: inject.c)"
"${CC[@]}" "${INCLUDES[@]}" -Werror -dynamiclib -install_name "$INSTALL_DIR/liba34gl-inject.dylib" \
	-o "$A/liba34gl-inject.dylib" "$HERE/inject.c" "$HERE/log.c" -framework CoreFoundation -framework IOKit

log "a34gl-bbd (backboardd's stand-in in its launchd job: bbd.c)"
"${CC[@]}" -Werror -o "$A/a34gl-bbd" "$HERE/bbd.c"

log "a34gl-surftest (the IOSurface path, on its own: surftest.c)"
"${CC[@]}" "${INCLUDES[@]}" -Werror -o "$A/a34gl-surftest" "$HERE/surftest.c" \
	"$MESA_OUT/libEGL.1.dylib" "$MESA_OUT/libGLESv2.2.dylib" "$MESA_OUT/libdrm.2.dylib" \
	-framework CoreFoundation -framework IOSurface

log "package"
mkdir -p "$A34GL_OUT/dSYM"          # (clang -g leaves them next to what it links: not for the phone)
mv "$A"/*.dSYM "$A34GL_OUT/dSYM/"
cp "$MESA_OUT"/*.dylib "$MESA_OUT/mali-tri" "$A/"
cp "$HERE/backboardd.env.example" "$A/"
cp "$HERE/INSTALL.md" "$A34GL_OUT/"
for f in "$A"/liba34gl*.dylib "$A/a34gl-bbd" "$A/a34gl-surftest"; do
	codesign -f -s - "$f"           # (last: nothing changes a load command after this)
done

log "check"
bad=0
for f in "$A"/*.dylib "$A/mali-tri" "$A/a34gl-bbd" "$A/a34gl-surftest"; do
	name=$(basename "$f")
	if otool -L "$f" | awk 'NR > 1 { print $1 }' | grep -v -E "^($INSTALL_DIR/|/usr/lib/|/System/Library/)"; then
		echo "error: $name refers to a library outside $INSTALL_DIR and the system"
		bad=1
	fi
	case "$name" in
	*.dylib)
		[ "$(otool -D "$f" | tail -n 1)" = "$INSTALL_DIR/$name" ] || { echo "error: $name is not named $INSTALL_DIR/$name"; bad=1; } ;;
	esac
	if otool -l "$f" | grep -E '^ *cmd LC_RPATH$' >/dev/null || otool -l "$f" | grep -E '^ *name @' >/dev/null; then
		echo "error: $name has an LC_RPATH or an @ path"
		bad=1
	fi
	[ "$(lipo -archs "$f")" = arm64 ] || { echo "error: $name is $(lipo -archs "$f"), not arm64"; bad=1; }
	codesign -v "$f" || { echo "error: $name: signature"; bad=1; }
done
for sym in a34gl_function_values a34gl_function_refused; do
	nm -gU "$A/liba34gl.dylib" | grep " _$sym\$" >/dev/null || { echo "error: liba34gl does not export $sym"; bad=1; }
done
if nm -gU "$A/liba34gl.dylib" | grep -v -E ' _a34gl_function_(values|refused)$' | grep ' [TtDdSs] '; then
	echo "error: liba34gl exports more than the two functions inject.c looks up"
	bad=1
fi
for sym in eglCreateImageKHR glEGLImageTargetTexture2DOES; do
	strings "$A/liba34gl.dylib" | grep -x "$sym" >/dev/null || { echo "error: liba34gl does not ask for $sym"; bad=1; }
done
[ "$bad" = 0 ] && echo ok
[ "$bad" = 0 ]

{
	echo "liba34gl: CoreAnimation's OpenGL ES through Mesa's panfrost (Galaxy A34, Mali-G68), iOS 16.7.16"
	echo
	echo "repository:    ${GITHUB_REPOSITORY:-local} ${GITHUB_SHA:-$(git -C "$HERE" rev-parse HEAD 2>/dev/null || echo '?')}"
	echo "run:           ${GITHUB_SERVER_URL:-}/${GITHUB_REPOSITORY:-}/actions/runs/${GITHUB_RUN_ID:-local}"
	echo "built:         $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "install dir:   $INSTALL_DIR (absolute install names, no LC_RPATH, ad-hoc signed)"
	echo "iPhoneOS SDK:  $(xcrun --sdk iphoneos --show-sdk-version); target arm64-apple-ios$IOS_MIN"
	echo "eagl table:    $(grep -c '^0x' "$HERE/eagl_slots.txt") named slots (eagl_slots.txt), $(wc -l <"$BUILD/exported-gl.txt" | tr -d ' ') gl* exported by libGLESv2"
	echo "               $(grep '^mkeagl.py' "$BUILD/mkeagl.log" || true)"
	echo
	echo "files (into $INSTALL_DIR):"
	(cd "$A" && for f in *; do
		printf '  %-26s %9s  %s\n' "$f" "$(stat -f %z "$f")" "$(shasum -a 256 "$f" | cut -d' ' -f1)"
	done)
	echo
	for f in "$A"/liba34gl*.dylib "$A/a34gl-bbd" "$A/a34gl-surftest"; do
		echo "$(basename "$f"):"
		otool -L "$f" | tail -n +2
	done
	echo
	echo "Mesa (userland/mesa/build.sh):"
	sed 's/^/  /' "$MESA_OUT/BUILD_INFO.txt"
} >"$A34GL_OUT/BUILD_INFO.txt"
cat "$A34GL_OUT/BUILD_INFO.txt"
