#!/bin/bash
# Bring the iOS root on the phone's userdata to IMAGE by writing only the
# 4 MiB chunks that differ, from TWRP. Much faster than write-root.sh when
# only a few files changed: the phone checksums its partition (chunksum,
# ~1 min), only differing chunks are sent, then those chunks are checked.
#
#   write-root-diff.sh IMAGE
#
# Every chunk the phone has changed while iOS ran is also put back, so the
# result is exactly IMAGE (same as a full write).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
A="$ROOT/tools/platform-tools/adb"
IMG=${1:?usage: write-root-diff.sh IMAGE}
DEV=/dev/block/by-name/userdata
CH=4194304
CS="$ROOT/tools/chunksum"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

[ "$("$A" get-state)" = "recovery" ] || { echo "phone must be in TWRP" >&2; exit 1; }
[ -x "$CS/chunksum-arm64" ] || "$CS/build.sh"
size=$(stat -c %s "$IMG")
[ $((size % CH)) -eq 0 ] || { echo "image size is not a multiple of 4 MiB" >&2; exit 1; }
n=$((size / CH))
"$A" shell 'twrp unmount data >/dev/null 2>&1; umount /sdcard /data/media /data 2>/dev/null; true'
if "$A" shell "grep -q ' /data ' /proc/mounts"; then
    echo "/data is still mounted; refusing to write" >&2
    exit 1
fi

echo "[$(date +%H:%M:%S)] checksumming $n chunks here and on the phone"
"$A" push "$CS/chunksum-arm64" /tmp/chunksum >/dev/null
"$A" shell "chmod 755 /tmp/chunksum && /tmp/chunksum $DEV $CH $n" | tr -d '\r' > "$T/phone" &
"$CS/chunksum-host" "$IMG" $CH $n > "$T/host"
wait
[ "$(wc -l < "$T/phone")" -eq "$n" ] || { echo "phone checksums incomplete" >&2; exit 1; }
diff <(cat "$T/host") <(cat "$T/phone") | awk '/^</ {print $2}' > "$T/changed" || true
c=$(wc -l < "$T/changed")
echo "[$(date +%H:%M:%S)] $c of $n chunks differ ($((c * 4)) MiB)"
[ "$c" -eq 0 ] && { echo "already identical"; exit 0; }

# runs of consecutive chunks
awk 'NR==1 {s=$1; p=$1; next} $1==p+1 {p=$1; next} {print s, p-s+1; s=$1; p=$1} END {print s, p-s+1}' "$T/changed" > "$T/runs"
while read -r start len; do
    dd if="$IMG" bs=$CH skip="$start" count="$len" status=none | gzip -1 -c |
        "$A" shell "gzip -dc | dd of=$DEV bs=$CH seek=$start conv=notrunc 2>/dev/null; echo rc=\$?" |
        tr -d '\r' | grep -q '^rc=0$' || { echo "write of chunks $start+$len failed" >&2; exit 1; }
done < "$T/runs"
"$A" shell sync
echo "[$(date +%H:%M:%S)] written $(wc -l < "$T/runs") runs; verifying"
bad=0
while read -r start len; do
    "$A" shell "/tmp/chunksum $DEV $CH $len $start" | tr -d '\r' > "$T/check"
    awk -v s="$start" -v l="$len" '$1 >= s && $1 < s + l' "$T/host" | diff -q - "$T/check" >/dev/null || bad=$((bad + 1))
done < "$T/runs"
[ $bad -eq 0 ] && echo "[$(date +%H:%M:%S)] root image verified ($c chunks rewritten)" || { echo "MISMATCH in $bad runs" >&2; exit 1; }
