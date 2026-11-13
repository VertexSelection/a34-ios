#!/usr/bin/env python3
"""Add files and a run-at-boot launchd job to a mounted iOS root (run as root).

  root-add-job.py ROOT LABEL DEST_DIR SRC_DIR PROGRAM [ARG...]

Copies every file of SRC_DIR into ROOT/DEST_DIR (mode 0755, root:wheel) and
registers LABEL in launchd's job list (System/Library/xpc/launchd.plist, read
from disk with boot-arg launchd_unsecure_cache=1), running DEST_DIR/PROGRAM
once at load with stdout and stderr on /dev/console, so its output lands in
the kernel's persistent log (tools/pull-log.sh). Running it again replaces
the job and the files.
"""
import os
import plistlib
import shutil
import sys


def main():
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    root, label, dest, src, prog = sys.argv[1:6]
    args = sys.argv[6:]
    ddir = os.path.join(root, dest.lstrip('/'))
    os.makedirs(ddir, exist_ok=True)
    os.chmod(ddir, 0o755)
    for name in sorted(os.listdir(src)):
        s = os.path.join(src, name)
        if not os.path.isfile(s):
            continue
        d = os.path.join(ddir, name)
        shutil.copyfile(s, d)
        os.chown(d, 0, 0)
        os.chmod(d, 0o755)
        print('copied', os.path.join(dest, name), os.path.getsize(d))

    path = os.path.join(root, 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    jobs = plist['LaunchDaemons']
    key = '/System/Library/LaunchDaemons/%s.plist' % label
    jobs[key] = {
        'Label': label,
        'ProgramArguments': [os.path.join(dest, prog)] + args,
        'RunAtLoad': True,
        'LaunchOnlyOnce': True,
        'UserName': 'root',
        'StandardOutPath': '/dev/console',
        'StandardErrorPath': '/dev/console',
        'EnvironmentVariables': {'EGL_LOG_LEVEL': 'debug', 'MESA_DEBUG': '1'},
        # Unknown daemons get a small jetsam limit; Mesa plus GPU buffers is
        # far above it (mali-tri was killed: reason namespace 1, code 7).
        'JetsamProperties': {'JetsamMemoryLimit': 2048},
    }
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print('launchd job', label, '->', jobs[key]['ProgramArguments'])

    # The per-device jetsam table gives every daemon a 6 MB limit
    # (Version4/Daemon/Override/Global) that wins over the job's own
    # JetsamProperties; exempt ours the way com.apple.datamigrator is.
    for jp in ('com.apple.jetsamproperties.D22.plist', 'com.apple.jetsamproperties.D221.plist'):
        jpath = os.path.join(root, 'System/Library/LaunchDaemons', jp)
        if not os.path.exists(jpath):
            continue
        jst = os.stat(jpath)
        with open(jpath, 'rb') as f:
            jt = plistlib.load(f)
        jt['Version4']['Daemon']['Override'][label] = {
            'JetsamPriority': 180, 'ActiveSoftMemoryLimit': -1, 'InactiveHardMemoryLimit': -1,
        }
        with open(jpath, 'wb') as f:
            plistlib.dump(jt, f, fmt=plistlib.FMT_BINARY)
        os.chown(jpath, jst.st_uid, jst.st_gid)
        os.chmod(jpath, jst.st_mode & 0o7777)
        print('jetsam override for', label, 'in', jp)


if __name__ == '__main__':
    main()
