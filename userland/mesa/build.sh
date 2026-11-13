#!/bin/bash
# Mesa's panfrost (EGL surfaceless + OpenGL ES) for the iOS 16 arm64 user space, built on
# a Mac with Xcode: see README.md. Used by .github/workflows/mesa-ios.yml; runs locally too
# (Homebrew: llvm spirv-llvm-translator spirv-tools bison pkgconf ccache; Python: meson
# mako pyyaml packaging ninja).
#
#   userland/mesa/build.sh            everything, into $WORK/out
#
# 1. host tools: Mesa's mesa_clc, vtn_bindgen2 and panfrost_compile for this Mac (they
#    compile panfrost's OpenCL C helpers at build time; a cross build cannot make them);
# 2. libdrm.2.dylib for iOS: redwood-drm/, against libdrm's own xf86drm.h;
# 3. Mesa for iOS: libgallium (panfrost), libEGL, libGLESv2;
# 4. out/: the dylibs, with absolute install names $INSTALL_DIR/<name> (default /a34: iOS's
#    dyld refuses @rpath and other "@" paths for our binaries), headers;
# 5. mali-tri, the test program, linked against those;
# 6. everything ad-hoc signed, last (a load command changed after signing breaks the
#    signature, and launchd panics on such a program), checked, BUILD_INFO.txt.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
MESA_VERSION=${MESA_VERSION:-26.3.0}
MESA_SHA256=${MESA_SHA256:-91d729aa68bb89e4ec857841300bbe922bcb3e52b1f97566ca83930d3b28343a}
LIBDRM_VERSION=${LIBDRM_VERSION:-2.4.134}
LIBDRM_SHA256=${LIBDRM_SHA256:-ac5e74d157830eb8bee44c6a6bf3ad49774ef0dd2a72bdad74a8f20308b52a95}
IOS_MIN=${IOS_MIN:-16.0}
INSTALL_DIR=${INSTALL_DIR:-/a34}         # where the dylibs live on the phone: their install names
WORK=${WORK:-$HERE/work}
OUT=${OUT:-$WORK/out}
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}

DL=$WORK/dl
SRC=$WORK/mesa-$MESA_VERSION
HOST_DRM=$WORK/host-drm
HOST_BIN=$WORK/host-bin
IOS_DRM=$WORK/ios-drm
STAGE=$WORK/stage
PREFIX=/usr/local

IOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path)
MAC_SDK=$(xcrun --sdk macosx --show-sdk-path)
CLANG=$(xcrun -f clang)
CLANGXX=$(xcrun -f clang++)
IOS_TARGET=(-target "arm64-apple-ios$IOS_MIN" -isysroot "$IOS_SDK")
CCACHE=$(command -v ccache || true)
BREW=$(command -v brew >/dev/null && brew --prefix || echo /opt/homebrew)
LLVM_PREFIX=${LLVM_PREFIX:-$BREW/opt/llvm}
BISON_PREFIX=${BISON_PREFIX:-$BREW/opt/bison}

log() { printf '\n=== %s\n' "$*"; }

meson_setup() {
	local dir=$1
	shift
	rm -rf "$dir"
	if ! meson setup "$dir" "$@" >"$dir.setup.log" 2>&1; then
		cat "$dir.setup.log"
		tail -n 80 "$dir/meson-logs/meson-log.txt" 2>/dev/null || true
		return 1
	fi
	# the configuration summary
	sed -n '/^  [A-Za-z]/,$p' "$dir.setup.log" | head -n 80
}

fetch() {
	local url=$1 sum=$2 file
	file=$DL/$(basename "$url")
	mkdir -p "$DL"
	if [ ! -f "$file" ]; then
		# (a download that fails in $(...) would not stop the script by itself)
		curl -fsSL --retry 5 --retry-delay 5 --retry-all-errors -o "$file.part" "$url" ||
			{ echo "error: cannot fetch $url" >&2; return 1; }
		mv "$file.part" "$file"
	fi
	echo "$sum  $file" | shasum -a 256 -c --quiet >&2
	echo "$file"
}

sources() {
	log "sources: Mesa $MESA_VERSION, libdrm $LIBDRM_VERSION headers"
	local mesa_tar drm_tar
	mesa_tar=$(fetch "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz" "$MESA_SHA256")
	drm_tar=$(fetch "https://dri.freedesktop.org/libdrm/libdrm-$LIBDRM_VERSION.tar.xz" "$LIBDRM_SHA256")
	rm -rf "$SRC" "$WORK/libdrm-$LIBDRM_VERSION"
	tar -xJf "$mesa_tar" -C "$WORK"
	tar -xJf "$drm_tar" -C "$WORK" "libdrm-$LIBDRM_VERSION/xf86drm.h"
	for p in "$HERE"/patches/*.patch; do
		echo "patch: $(basename "$p")"
		patch -p1 --no-backup-if-mismatch --forward -d "$SRC" <"$p"
	done
}

# libdrm.2.dylib and its pkg-config file: $1 prefix, $2 install name, then compiler and flags
libdrm_standin() {
	local prefix=$1 install_name=$2
	shift 2
	rm -rf "$prefix"
	mkdir -p "$prefix/include" "$prefix/lib/pkgconfig"
	cp "$WORK/libdrm-$LIBDRM_VERSION/xf86drm.h" "$HERE/redwood-drm/redwood_drm.h" "$prefix/include/"
	cp -R "$HERE/redwood-drm/compat" "$prefix/include/compat"
	"$@" -std=gnu11 -O2 -g -Wall -Wextra -fvisibility=hidden -dynamiclib \
		-install_name "$install_name" -compatibility_version 3.0.0 -current_version 3.0.0 \
		-I"$prefix/include" -I"$SRC/include" -I"$SRC/include/drm-uapi" -I"$HERE/redwood-drm" \
		-o "$prefix/lib/libdrm.2.dylib" "$HERE/redwood-drm/redwood_drm.c" \
		-framework IOKit -framework CoreFoundation
	ln -sf libdrm.2.dylib "$prefix/lib/libdrm.dylib"
	cat >"$prefix/lib/pkgconfig/libdrm.pc" <<-EOF
		prefix=$prefix
		includedir=\${prefix}/include
		libdir=\${prefix}/lib

		Name: libdrm
		Description: libdrm stand-in for Darwin: DRM requests to the RedwoodMali IOKit user client
		Version: $LIBDRM_VERSION
		Libs: -L\${libdir} -ldrm
		Cflags: -I\${includedir} -I\${includedir}/compat -I$SRC/include/drm-uapi
	EOF
}

host_tools() {
	log "host tools (macOS $(sw_vers -productVersion), LLVM $("$LLVM_PREFIX/bin/llvm-config" --version))"
	# the IOKit values and prototypes redwood_iokit.h declares for the iPhoneOS SDK, checked
	# against the macOS SDK's real headers
	"$CLANG" -isysroot "$MAC_SDK" -fsyntax-only -Wall -Werror -I"$HERE/redwood-drm" "$HERE/redwood-drm/iokit_check.c"
	libdrm_standin "$HOST_DRM" "$HOST_DRM/lib/libdrm.2.dylib" "$CLANG" -isysroot "$MAC_SDK"
	(
		export PATH="$LLVM_PREFIX/bin:$BISON_PREFIX/bin:$PATH"
		export SDKROOT="$MAC_SDK"       # the toolchain's clang, called directly, knows no SDK
		export PKG_CONFIG_PATH="$HOST_DRM/lib/pkgconfig:$BREW/lib/pkgconfig"
		export CC="${CCACHE:+$CCACHE }$CLANG" CXX="${CCACHE:+$CCACHE }$CLANGXX" OBJC="${CCACHE:+$CCACHE }$CLANG"
		meson_setup "$WORK/build-host" "$SRC" --buildtype=release -Db_ndebug=true \
			-Dgallium-drivers= -Dvulkan-drivers= -Dtools=panfrost -Dplatforms= \
			-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
			-Dllvm=enabled -Dshared-llvm=enabled -Ddraw-use-llvm=false -Dspirv-tools=enabled \
			-Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true \
			-Dzstd=disabled -Dzlib=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dvalgrind=disabled \
			-Dlibunwind=disabled -Dlmsensors=disabled -Dshader-cache=disabled -Dvideo-codecs= \
			-Dgallium-va=disabled -Ddisplay-info=disabled -Dbuild-tests=false
		ninja -C "$WORK/build-host" -j "$JOBS" \
			src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 src/panfrost/clc/panfrost_compile
	)
	rm -rf "$HOST_BIN"
	mkdir -p "$HOST_BIN"
	cp "$WORK/build-host/src/compiler/clc/mesa_clc" "$WORK/build-host/src/compiler/spirv/vtn_bindgen2" \
		"$WORK/build-host/src/panfrost/clc/panfrost_compile" "$HOST_BIN/"
	"$HOST_BIN/mesa_clc" --help >/dev/null 2>&1 || true
	ls -l "$HOST_BIN"
}

ios_libdrm() {
	log "libdrm stand-in for iOS $IOS_MIN ($(xcrun --sdk iphoneos --show-sdk-version) SDK)"
	if [ -f "$IOS_SDK/System/Library/Frameworks/IOKit.framework/Headers/IOKitLib.h" ]; then
		echo "iPhoneOS SDK has IOKit headers"
	else
		echo "iPhoneOS SDK has no IOKit headers: redwood_iokit.h declares the calls"
	fi
	# the branch for SDKs without IOKit headers must build too
	"$CLANG" "${IOS_TARGET[@]}" -fsyntax-only -Wall -Wextra -DREDWOOD_OWN_IOKIT -I"$WORK/libdrm-$LIBDRM_VERSION" \
		-I"$SRC/include" -I"$SRC/include/drm-uapi" -I"$HERE/redwood-drm" "$HERE/redwood-drm/redwood_drm.c"
	libdrm_standin "$IOS_DRM" "$INSTALL_DIR/libdrm.2.dylib" "$CLANG" "${IOS_TARGET[@]}"
}

ios_mesa() {
	log "Mesa $MESA_VERSION for iOS"
	local cc=("$CLANG" "${IOS_TARGET[@]}") cxx=("$CLANGXX" "${IOS_TARGET[@]}")
	q() { local s=() a; for a in "$@"; do s+=("'$a'"); done; local IFS=,; echo "[${s[*]}]"; }
	cat >"$WORK/ios-cross.ini" <<-EOF
		[binaries]
		c = $(q ${CCACHE:+"$CCACHE"} "${cc[@]}")
		cpp = $(q ${CCACHE:+"$CCACHE"} "${cxx[@]}")
		objc = $(q ${CCACHE:+"$CCACHE"} "${cc[@]}")
		ar = '$(xcrun -f ar)'
		strip = '$(xcrun -f strip)'
		pkg-config = '$(command -v pkg-config)'

		[properties]
		needs_exe_wrapper = true
		pkg_config_libdir = ['$IOS_DRM/lib/pkgconfig']

		[host_machine]
		system = 'darwin'
		subsystem = 'ios'
		kernel = 'xnu'
		cpu_family = 'aarch64'
		cpu = 'arm64'
		endian = 'little'
	EOF
	(
		export PATH="$HOST_BIN:$BISON_PREFIX/bin:$PATH"
		meson_setup "$WORK/build-ios" "$SRC" --cross-file "$WORK/ios-cross.ini" \
			--prefix="$PREFIX" --libdir=lib --buildtype=release -Db_ndebug=true \
			-Dgallium-drivers=panfrost -Dvulkan-drivers= -Dtools= -Dplatforms= \
			-Degl=enabled -Dglx=disabled -Dgbm=disabled -Dglvnd=disabled \
			-Dopengl=false -Dgles1=disabled -Dgles2=enabled \
			-Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system -Dspirv-tools=disabled \
			-Dmicrosoft-clc=disabled -Dgallium-rusticl=false -Dteflon=false -Dpanfrost-rust=false \
			-Dzstd=disabled -Dzlib=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dvalgrind=disabled \
			-Dlibunwind=disabled -Dlmsensors=disabled -Dshader-cache=disabled -Dvideo-codecs= \
			-Dgallium-va=disabled -Ddisplay-info=disabled -Dbuild-tests=false
		if ! ninja -C "$WORK/build-ios" -j "$JOBS" -k 0 >"$WORK/build-ios.log" 2>&1; then
			grep -E -B2 -A12 'error:|FAILED:|Undefined symbols|ld: ' "$WORK/build-ios.log" | head -n 600
			echo "ninja failed; the end of the log:"
			tail -n 60 "$WORK/build-ios.log"
			return 1
		fi
		tail -n 3 "$WORK/build-ios.log"
		echo "compiler warnings: $(grep -c 'warning:' "$WORK/build-ios.log" || true)"
		# an API newer than iOS $IOS_MIN, called unguarded, would keep the libraries from loading there
		if grep -E 'warning: .*(only available on iOS|is unavailable|-Wunguarded-availability)' "$WORK/build-ios.log" | sort -u | grep .; then
			echo "error: APIs newer than iOS $IOS_MIN are used (above)"
			return 1
		fi
		rm -rf "$STAGE"
		meson install -C "$WORK/build-ios" --no-rebuild --destdir "$STAGE" >"$WORK/install-ios.log"
	)
}

# The libraries into out/, each named $INSTALL_DIR/<file> (LC_ID_DYLIB) and naming the others
# so (meson links them by @rpath/..., meson install by $PREFIX/lib/...), with no LC_RPATH.
# Signed later, by sign().
package() {
	log "package (install names $INSTALL_DIR/...)"
	local lib=$STAGE$PREFIX/lib f dep rpath
	rm -rf "$OUT"
	mkdir -p "$OUT/include"
	cp "$IOS_DRM/lib/libdrm.2.dylib" "$OUT/"
	for f in "$lib"/libgallium*.dylib "$lib"/libEGL.*.dylib "$lib"/libGLESv2.*.dylib; do
		[ -L "$f" ] || cp "$f" "$OUT/"
	done
	cp -R "$STAGE$PREFIX/include/EGL" "$STAGE$PREFIX/include/GLES2" "$STAGE$PREFIX/include/GLES3" \
		"$STAGE$PREFIX/include/KHR" "$OUT/include/" 2>/dev/null || true
	cp "$HERE/redwood-drm/redwood_drm.h" "$OUT/include/"
	(
		cd "$OUT"
		for f in *.dylib; do
			install_name_tool -id "$INSTALL_DIR/$f" "$f" 2>/dev/null
			for dep in $(otool -L "$f" | awk 'NR > 1 { print $1 }'); do
				case "$dep" in
				@rpath/* | @loader_path/* | @executable_path/* | "$PREFIX"/lib/*)
					install_name_tool -change "$dep" "$INSTALL_DIR/$(basename "$dep")" "$f" 2>/dev/null ;;
				esac
			done
			for rpath in $(otool -l "$f" | awk '/cmd LC_RPATH/ { r = 1 } r && $1 == "path" { print $2; r = 0 }'); do
				install_name_tool -delete_rpath "$rpath" "$f" 2>/dev/null
			done
		done
	)
}

# mali-tri, linked against out/'s libraries: it names them $INSTALL_DIR/<file> from the start.
mali_tri() {
	log "mali-tri"
	mkdir -p "$WORK/mali-tri"
	"$CLANG" "${IOS_TARGET[@]}" -std=gnu11 -O2 -g -Wall -Wextra \
		-I"$OUT/include" -o "$WORK/mali-tri/mali-tri" "$HERE/mali-tri/mali-tri.c" \
		"$OUT/libEGL.1.dylib" "$OUT/libGLESv2.2.dylib"
	cp "$WORK/mali-tri/mali-tri" "$OUT/"     # (its .dSYM stays behind)
}

# Ad-hoc signatures, after the last change to any load command.
sign() {
	log "sign"
	local f
	for f in "$OUT"/*.dylib "$OUT/mali-tri"; do
		codesign -f -s - "$f"
	done
	check
	build_info >"$OUT/BUILD_INFO.txt"
	cat "$OUT/BUILD_INFO.txt"
}

# What a loader on the phone would trip over, found here instead.
check() {
	local f sym bad=0
	log "check"
	for f in "$OUT"/*.dylib "$OUT/mali-tri"; do
		if otool -L "$f" | awk 'NR > 1 { print $1 }' | grep -v -E "^($INSTALL_DIR/|/usr/lib/|/System/Library/)"; then
			echo "error: $(basename "$f") refers to a library outside $INSTALL_DIR and the system"
			bad=1
		fi
		if [ "$f" != "$OUT/mali-tri" ] && [ "$(otool -D "$f" | tail -n 1)" != "$INSTALL_DIR/$(basename "$f")" ]; then
			echo "error: $(basename "$f") is named $(otool -D "$f" | tail -n 1), not $INSTALL_DIR/$(basename "$f")"
			bad=1
		fi
		if otool -l "$f" | grep -E '^ *cmd LC_RPATH$' >/dev/null || otool -l "$f" | grep -E '^ *name @' >/dev/null; then
			echo "error: $(basename "$f") has an LC_RPATH or an @ path"
			bad=1
		fi
		if [ "$(lipo -archs "$f")" != arm64 ]; then
			echo "error: $(basename "$f") is $(lipo -archs "$f"), not arm64"
			bad=1
		fi
		codesign -v "$f" || bad=1
	done
	for sym in eglGetPlatformDisplay eglInitialize eglCreatePbufferSurface eglGetProcAddress; do
		nm -gU "$OUT/libEGL.1.dylib" | grep " _$sym\$" >/dev/null || { echo "error: libEGL lacks $sym"; bad=1; }
	done
	for sym in glClear glDrawArrays glReadPixels glCreateShader glGetString; do
		nm -gU "$OUT/libGLESv2.2.dylib" | grep " _$sym\$" >/dev/null || { echo "error: libGLESv2 lacks $sym"; bad=1; }
	done
	for sym in drmIoctl drmGetDevices2 drmGetVersion drmSyncobjWait drmPrimeFDToHandle redwood_drm_mmap redwood_drm_munmap \
		redwood_drm_surface redwood_drm_stats redwood_drm_queued; do
		nm -gU "$OUT/libdrm.2.dylib" | grep " _$sym\$" >/dev/null || { echo "error: libdrm lacks $sym"; bad=1; }
	done
	# panfrost maps BOs through the user client (patch 0002) and sizes IOSurface imports by it (0003)
	for sym in redwood_drm_mmap redwood_drm_munmap redwood_drm_surface; do
		nm -u "$OUT/libgallium-$MESA_VERSION.dylib" | grep "^_$sym\$" >/dev/null || { echo "error: libgallium does not call $sym"; bad=1; }
	done
	# a context taken over from another thread: this thread's GL thread-locals cleared (0004)
	nm -u "$OUT/libEGL.1.dylib" | grep "^__mesa_glapi_set_context\$" >/dev/null || { echo "error: libEGL does not call _mesa_glapi_set_context (0004)"; bad=1; }
	# EGL_IOSURFACE_A34 (0003)
	nm -u "$OUT/libEGL.1.dylib" | grep "^_redwood_drm_surface\$" >/dev/null || { echo "error: libEGL does not call redwood_drm_surface"; bad=1; }
	grep -q 'EGL_IOSURFACE_A34' "$OUT/include/EGL/eglmesaext.h" || { echo "error: eglmesaext.h lacks EGL_IOSURFACE_A34"; bad=1; }
	[ "$bad" = 0 ] && echo "ok"
	return "$bad"
}

build_info() {
	local f
	echo "Mesa for the iOS 16 user space on the Galaxy A34 (Mali-G68 MC4, panfrost)"
	echo
	echo "repository:    ${GITHUB_REPOSITORY:-local} ${GITHUB_SHA:-$(git -C "$HERE" rev-parse HEAD 2>/dev/null || echo '?')}"
	echo "run:           ${GITHUB_SERVER_URL:-}/${GITHUB_REPOSITORY:-}/actions/runs/${GITHUB_RUN_ID:-local}"
	echo "built:         $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "Mesa:          $MESA_VERSION  (archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz, sha256 $MESA_SHA256)"
	echo "Mesa commit:   ${MESA_COMMIT:-$(git ls-remote https://gitlab.freedesktop.org/mesa/mesa.git "refs/tags/mesa-$MESA_VERSION^{}" 2>/dev/null | cut -f1)}"
	echo "libdrm:        xf86drm.h of libdrm $LIBDRM_VERSION (sha256 $LIBDRM_SHA256); implementation: redwood-drm/"
	echo "patches:"
	for f in "$HERE"/patches/*.patch; do
		echo "  $(shasum -a 256 "$f" | cut -c1-16)  $(basename "$f")"
	done
	echo "Xcode:         $(xcodebuild -version | tr '\n' ' ')"
	echo "iPhoneOS SDK:  $(xcrun --sdk iphoneos --show-sdk-version) ($IOS_SDK)"
	echo "clang:         $("$CLANG" --version | head -n 1)"
	echo "target:        arm64-apple-ios$IOS_MIN (arm64, not arm64e)"
	echo "installed in:  $INSTALL_DIR (absolute install names; no LC_RPATH)"
	echo "host LLVM:     $("$LLVM_PREFIX/bin/llvm-config" --version 2>/dev/null) (mesa_clc, build time only)"
	echo "meson:         $(meson --version)"
	echo "meson options: -Dgallium-drivers=panfrost -Dplatforms= -Degl=enabled (surfaceless) -Dgles2=enabled"
	echo "               -Dopengl=false -Dglx=disabled -Dgbm=disabled -Dllvm=disabled -Dshader-cache=disabled"
	echo "               -Dxmlconfig=disabled -Dbuildtype=release -Db_ndebug=true (see build.sh)"
	echo
	echo "files:"
	(cd "$OUT" && for f in *.dylib mali-tri; do
		printf '  %-26s %9s  %s\n' "$f" "$(stat -f %z "$f")" "$(shasum -a 256 "$f" | cut -d' ' -f1)"
	done)
	echo
	echo "libdrm functions Mesa imports (all from libdrm.2.dylib, redwood-drm):"
	nm -u "$OUT"/libgallium-*.dylib "$OUT"/libEGL.1.dylib | grep -E '^_(drm|redwood_drm)' | sort -u | sed 's/^_//' |
		paste -sd ' ' - | fold -s -w 92 | sed 's/^/  /' || true
	echo
	for f in "$OUT"/*.dylib "$OUT/mali-tri"; do
		echo "$(basename "$f"):"
		otool -L "$f" | tail -n +2
		otool -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b&&/platform|minos|sdk/{print "\t" $1 " " $2} /LC_RPATH/{r=1} r&&/ path /{print "\trpath " $2; r=0}' | head -n 6
	done
}

mkdir -p "$WORK"
sources
host_tools
ios_libdrm
ios_mesa
package
mali_tri
sign
