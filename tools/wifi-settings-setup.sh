#!/bin/bash
# What Settings > Wi-Fi needs in the iOS root, on top of the kernel's side (xnu/patches/0969,
# 0995; docs/wifi-plan.md, "0969" and "0995"). Idempotent: run it again and it changes nothing.
#
#   sudo tools/wifi-settings-setup.sh ROOT
#   sudo tools/wifi-settings-setup.sh --undo ROOT     wifid's job as it was (stock _LimitLoadToDeviceTree:
#                                                    ["wlan", "marconi-wifi"]); nothing else is touched
#
# wifid needs kernel 0999 too (the registry going quiet): without it wifid waits for configd's
# interface namer at boot, and SpringBoard waits for wifid (log 0995-wifi-a: stuck at the logo).
#
# ROOT is the iOS root, mounted read-write. What it does:
#
#   1. wifid (com.apple.wifid): its job asks for a device-tree node "wlan" or "marconi-wifi"
#      (_LimitLoadToDeviceTree), which our ADT does not have, so launchd never starts it. The
#      condition is removed (root-add-job.py --unlimit), as HackiOS's mkjobs.py does; the Wi-Fi
#      wifid finds is in the registry (A34WLAN, kernel 0969/0995).
#   2. What wifid and Settings need besides: configd, mDNSResponder and its helper, trustd,
#      securityd, nsurlsessiond, CFNetworkAgent. All of them are jobs of the stock launchd.plist
#      (configd and mDNSResponder start at boot; the others on demand, by their Mach services, so
#      a boot log without their "exec" line is no sign of a problem) and the root's tweaks remove
#      none of them. They are checked -- the job there, its program on the root, not disabled,
#      not limited to hardware we lack -- and only what is missing would be added: nothing can be
#      added faithfully without the stock plist, so a missing one is reported and the script stops.
#   3. The a34wifi job (com.a34.wifi, 'a34wifi connect') is left as it is: the fallback join
#      from /a34/wifi/network until wifid joins by itself.
#   4. /a34/wifi/network, if there, is made root's and mode 0600. Its content is never read or
#      printed here: the passphrase comes from that file or from Settings, and goes into no log.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
if [ "${1:-}" = "--undo" ]; then
	ROOT=${2:?usage: sudo tools/wifi-settings-setup.sh --undo ROOT}
	python3 "$HERE/root-add-job.py" --relimit "$ROOT" com.apple.wifid | head -n 1
	exit 0
fi
ROOT=${1:?usage: sudo tools/wifi-settings-setup.sh [--undo] ROOT}
PLIST="$ROOT/System/Library/xpc/launchd.plist"
[ -f "$PLIST" ] || { echo "error: $PLIST not found (is ROOT the mounted iOS root?)" >&2; exit 1; }
[ -w "$PLIST" ] || { echo "error: $PLIST is not writable (mounted read-write? run as root?)" >&2; exit 1; }

echo "== the jobs Settings > Wi-Fi needs"
python3 -I - "$PLIST" "$ROOT" <<'EOF'
import os, plistlib, sys
plist_path, root = sys.argv[1], sys.argv[2]
with open(plist_path, 'rb') as f:
    jobs = plistlib.load(f)['LaunchDaemons']
by_label = {j.get('Label'): j for j in jobs.values() if isinstance(j, dict)}
wanted = [
    ('com.apple.wifid', 'wifid'),
    ('com.apple.configd', 'configd'),
    ('com.apple.mDNSResponder.reloaded', 'mDNSResponder'),
    ('com.apple.mDNSResponderHelper.reloaded', 'mDNSResponderHelper'),
    ('com.apple.trustd', 'trustd'),
    ('com.apple.securityd', 'securityd'),
    ('com.apple.nsurlsessiond', 'nsurlsessiond'),
    ('com.apple.cfnetwork.cfnetworkagent', 'CFNetworkAgent'),
]
missing = 0
for label, name in wanted:
    job = by_label.get(label)
    if job is None:
        print(f'  {name:20s} MISSING: no job {label} in launchd.plist')
        missing += 1
        continue
    program = job.get('Program') or (job.get('ProgramArguments') or [''])[0]
    notes = []
    if not os.path.exists(os.path.join(root, program.lstrip('/'))):
        notes.append(f'its program {program} is NOT on the root')
        missing += 1
    if job.get('Disabled'):
        notes.append('Disabled')
    for key in ('_LimitLoadToDeviceTree', '_LimitLoadToHardware', '_LimitLoadFromHardware'):
        if key in job:
            notes.append(f'{key} {job[key]}' + (' (removed below)' if label == 'com.apple.wifid' and
                                                 key == '_LimitLoadToDeviceTree' else ''))
    start = 'at boot' if job.get('RunAtLoad') or job.get('KeepAlive') else 'on demand'
    print(f'  {name:20s} present ({label}, {program}, starts {start})' + ('; ' + '; '.join(notes) if notes else ''))
a34 = by_label.get('com.a34.wifi')
print('  a34wifi job          ' + (f"present: {' '.join(a34.get('ProgramArguments', []))} (kept as the fallback)"
                                   if a34 else 'not there (the fallback join from /a34/wifi/network is not installed)'))
if missing:
    print(f'error: {missing} of them missing: the root is not the stock one; nothing changed', file=sys.stderr)
    sys.exit(1)
EOF

echo "== wifid: started without a \"wlan\" device-tree node"
python3 "$HERE/root-add-job.py" --unlimit "$ROOT" com.apple.wifid | head -n 1

echo "== /a34/wifi/network"
NET="$ROOT/a34/wifi/network"
if [ -f "$NET" ]; then
	chown 0:0 "$NET"
	chmod 600 "$NET"
	echo "  root's, mode 0600 (its content is not read here)"
else
	echo "  not there: the fallback join has no network (Settings can join one)"
fi
echo "done"
