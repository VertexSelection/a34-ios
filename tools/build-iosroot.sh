#!/bin/bash
# Build the iOS 16.7.16 root image for the A34 from the iPhone X IPSW.
#
#   build-iosroot.sh IPSW [OUT.img]
#
# Picks the D22AP "Erase" build identity from BuildManifest.plist, extracts its
# OS, SystemOS and AppOS disk images and its device tree, then runs ios-sweet's
# mkiosroot.sh (16 GiB journaled HFS+) and sweet-root-tweaks.py. The D22AP
# device tree goes to research/ipsw-x/d22ap.adt, where mkadt_a34.py takes the
# iPhone X /product identity from. Everything extracted stays out of git
# (/research/, /ipsw/, /build/ are ignored). Needs sudo (loop mount, rsync as root).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SWEET="$ROOT/third_party/ios-sweet/tools"
IPSW=$(realpath "${1:?usage: build-iosroot.sh IPSW [OUT.img]}")
OUT=${2:-$ROOT/build/ios16-root.img}
X="$ROOT/research/ipsw-x"
export PATH="$ROOT/tools/bin:$PATH"      # apfs-fuse, lzfse
mkdir -p "$X"

unzip -o -q -d "$X" "$IPSW" BuildManifest.plist
python3 -I - "$X/BuildManifest.plist" > "$X/files.txt" <<'PYEOF'
import plistlib, sys
m = plistlib.load(open(sys.argv[1], 'rb'))
for bi in m['BuildIdentities']:
    info = bi['Info']
    if info.get('DeviceClass', '').lower() == 'd22ap' and 'Erase' in info.get('Variant', ''):
        man = bi['Manifest']
        for name, key in (('OS', 'OS'), ('SystemOS', 'Cryptex1,SystemOS'),
                          ('AppOS', 'Cryptex1,AppOS'), ('DeviceTree', 'DeviceTree')):
            print(name, man[key]['Info']['Path'])
        break
else:
    sys.exit('no D22AP Erase identity in BuildManifest.plist')
PYEOF
cat "$X/files.txt"
get() { awk -v k="$1" '$1 == k { print $2 }' "$X/files.txt"; }
unzip -o -q -d "$X" "$IPSW" "$(get OS)" "$(get SystemOS)" "$(get AppOS)" "$(get DeviceTree)"

python3 -I "$SWEET/adt.py" unwrap "$X/$(get DeviceTree)" "$X/d22ap.adt"
python3 -I "$SWEET/adt.py" dump "$X/d22ap.adt" | grep -m3 -E "model|compatible" || true

rm -f "$OUT"
bash "$SWEET/mkiosroot.sh" "$X/$(get OS)" "$X/$(get SystemOS)" "$X/$(get AppOS)" "$OUT" 16G

M=$(mktemp -d)
sudo mount -t hfsplus -o loop,force "$OUT" "$M"
sudo python3 -I "$SWEET/sweet-root-tweaks.py" "$M" || { sudo umount "$M"; exit 1; }
df -h "$M"
sudo umount "$M"
rmdir "$M"
fsck.hfsplus -f -n "$OUT"
ls -la "$OUT"
