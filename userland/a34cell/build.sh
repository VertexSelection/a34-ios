#!/bin/bash
# a34cell -- the cellular daemon (docs/cellular-plan.md) -- for the iOS 16 user space, built on a
# Mac with Xcode; used by .github/workflows/a34cell.yml.
#
#   userland/a34cell/build.sh            host tests, then the iOS binary
#   A34CELL_HOST_ONLY=1 .../build.sh     host tests only (works on Linux too)
#
# Makes $A34CELL_OUT (default userland/a34cell/work/out):
#   a34/a34cell     -> /a34/a34cell on the phone (arm64, iOS $IOS_MIN, system libraries only,
#                      ad-hoc signed)
#   INSTALL.txt
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
IOS_MIN=${IOS_MIN:-16.0}
WORK=${WORK:-$HERE/work}
OUT=${A34CELL_OUT:-$WORK/out}
SRC=(at.c policy.c sms.c model.c)
WARN=(-Wall -Wextra -Werror -Wno-unused-parameter)

rm -rf "$WORK/host" "$OUT"
mkdir -p "$WORK/host" "$OUT/a34"

echo "=== host tests"
HOSTCC=${HOSTCC:-cc}
SAN=(-fsanitize=address,undefined)
[ "$(uname)" = Darwin ] || SAN+=(-D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L)
(cd "$HERE" && "$HOSTCC" -std=c11 -O1 -g "${WARN[@]}" "${SAN[@]}" -pthread \
	-o "$WORK/host/test_a34cell" tests/test_a34cell.c "${SRC[@]}")
"$WORK/host/test_a34cell"

[ "${A34CELL_HOST_ONLY:-0}" = 1 ] && exit 0

echo "=== iOS arm64"
IOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CLANG=$(xcrun -f clang)
echo "clang: $("$CLANG" --version | head -n 1); iPhoneOS SDK $(xcrun --sdk iphoneos --show-sdk-version)"
(cd "$HERE" && "$CLANG" -target "arm64-apple-ios$IOS_MIN" -isysroot "$IOS_SDK" -std=c11 -O2 -g \
	"${WARN[@]}" -o "$OUT/a34/a34cell" a34cell.c "${SRC[@]}")
# System libraries only, no rpath: nothing for dyld to look for outside /usr/lib.
if otool -L "$OUT/a34/a34cell" | tail -n +2 | grep -v -E '^\s+/usr/lib/'; then
	echo "error: a34cell links something outside /usr/lib"
	exit 1
fi
if otool -l "$OUT/a34/a34cell" | grep -q LC_RPATH; then
	echo "error: a34cell has an LC_RPATH"
	exit 1
fi
dsymutil "$OUT/a34/a34cell" -o "$WORK/a34cell.dSYM" >/dev/null 2>&1 || true
strip -S "$OUT/a34/a34cell"
codesign -f -s - "$OUT/a34/a34cell"     # last: nothing changes the file after this
codesign -v "$OUT/a34/a34cell"
lipo -info "$OUT/a34/a34cell"

cat >"$OUT/INSTALL.txt" <<'EOF'
a34cell -- cellular daemon for the A34 port (docs/cellular-plan.md on branch "cellular").

On the host, with the iOS root image mounted read-write at /mnt (as for the other /a34 tools):

  sudo tools/root-add-job.py /mnt com.a34.cell /a34 a34cell/a34 a34cell

That installs /a34/a34cell and a launchd job that runs it once at boot, output on the console
(so in the kernel's persistent log, tools/pull-log.sh). With no modem port (/dev/a34at0, made by
the kernel's CCCI stage, a34cell=2) it prints the kernel's cellular state and exits 0; nothing
else is touched. The transmitter stays off unless "-radio" is added to the job's arguments AND
the modem reports a well-formed identity of its own; the identity is never printed or stored.

  a34cell [-port /dev/a34at0] [-state /var/run/a34cell.plist] [-inbox DIR] [-radio] [-trace] [-once]

To remove: delete the job com.a34.cell from System/Library/xpc/launchd.plist (or reinstall the
root image) and /a34/a34cell.
EOF
ls -l "$OUT/a34"
