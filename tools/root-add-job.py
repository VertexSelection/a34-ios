#!/usr/bin/env python3
"""Add files and launchd jobs to a mounted iOS root, or change a job (run as root).

  root-add-job.py ROOT LABEL DEST_DIR SRC_DIR PROGRAM [ARG...]
      Copies every file of SRC_DIR into ROOT/DEST_DIR (mode 0755, root:wheel) and
      registers LABEL in launchd's job list (System/Library/xpc/launchd.plist, read
      from disk with boot-arg launchd_unsecure_cache=1), running DEST_DIR/PROGRAM
      once at load with stdout and stderr on /dev/console, so its output lands in
      the kernel's persistent log (tools/pull-log.sh). Running it again replaces
      the job and the files.

  root-add-job.py --copy ROOT DEST_DIR SRC_DIR
      Only the copy (as above).

  root-add-job.py --env ROOT LABEL KEY=VALUE...
      Sets EnvironmentVariables of the existing job LABEL; KEY= (nothing after
      the "=") removes KEY. launchd drops DYLD_* variables from a job, so
      DYLD_INSERT_LIBRARIES cannot be given this way (see --wrap).

  root-add-job.py --wrap ROOT LABEL WRAPPER
      The job runs WRAPPER (a path on the phone) in place of its program, with
      the program's path in A34GL_REAL; the wrapper sets the environment the
      program is to have and execs it (same pid, same Mach services).
      userland/a34gl/bbd.c (a34gl-bbd) is such a wrapper for backboardd. Running
      it again changes nothing.

  root-add-job.py --unwrap ROOT LABEL
      Undoes --wrap.

  root-add-job.py --show ROOT LABEL
      Prints the job.

For CoreAnimation on the GPU (userland/a34gl/README.md):
  root-add-job.py --copy  /mnt /a34 a34gl/a34
  root-add-job.py --wrap  /mnt com.apple.backboardd /a34/a34gl-bbd
  root-add-job.py --env   /mnt com.apple.backboardd A34GL_LOG=/var/mobile/a34gl.log
"""
import os
import plistlib
import shutil
import sys

LAUNCHD_PLIST = 'System/Library/xpc/launchd.plist'
REAL = 'A34GL_REAL'


def copy_files(root, dest, src):
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


def load_launchd(root):
    path = os.path.join(root, LAUNCHD_PLIST)
    with open(path, 'rb') as f:
        return path, plistlib.load(f)


def save_plist(path, plist):
    """Write a binary plist in place of `path`, keeping its owner and mode."""
    st = os.stat(path)
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)


def find_job(plist, label):
    """The job whose Label is `label`, of the LaunchDaemons of launchd.plist."""
    for key, job in plist['LaunchDaemons'].items():
        if isinstance(job, dict) and job.get('Label') == label:
            return key, job
    sys.exit('no launchd job with the label %s' % label)


def set_environment(job, pairs):
    env = job.setdefault('EnvironmentVariables', {})
    for pair in pairs:
        key, sep, value = pair.partition('=')
        if not key or not sep:
            sys.exit('not KEY=VALUE: %s' % pair)
        if key.startswith('DYLD_'):
            print('note: launchd drops %s from a job: use --wrap' % key)
        if value:
            env[key] = value
        else:
            env.pop(key, None)
    if not env:
        del job['EnvironmentVariables']


def program_of(job):
    if 'Program' in job:
        return job['Program']
    return job['ProgramArguments'][0]


def wrap(job, wrapper):
    program = program_of(job)
    env = job.setdefault('EnvironmentVariables', {})
    if program == wrapper:
        if REAL not in env:
            sys.exit('the job runs %s already, but %s is not set: fix it by hand' % (wrapper, REAL))
        return False
    env[REAL] = program
    if 'Program' in job:
        job['Program'] = wrapper
    if 'ProgramArguments' in job:
        job['ProgramArguments'][0] = wrapper
    return True


def unwrap(job):
    env = job.get('EnvironmentVariables', {})
    if REAL not in env:
        return False
    program = env.pop(REAL)
    if 'Program' in job:
        job['Program'] = program
    if 'ProgramArguments' in job:
        job['ProgramArguments'][0] = program
    if not env:
        del job['EnvironmentVariables']
    return True


def show(key, job):
    print(key)
    for name, value in sorted(job.items()):
        if name == 'MachServices':
            value = '%d services' % len(value)
        print('  %-22s %s' % (name, value))


def add_job(root, label, dest, src, prog, args):
    copy_files(root, dest, src)
    path, plist = load_launchd(root)
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
    save_plist(path, plist)
    print('launchd job', label, '->', jobs[key]['ProgramArguments'])

    # The per-device jetsam table gives every daemon a 6 MB limit
    # (Version4/Daemon/Override/Global) that wins over the job's own
    # JetsamProperties; exempt ours the way com.apple.datamigrator is.
    for jp in ('com.apple.jetsamproperties.D22.plist', 'com.apple.jetsamproperties.D221.plist'):
        jpath = os.path.join(root, 'System/Library/LaunchDaemons', jp)
        if not os.path.exists(jpath):
            continue
        with open(jpath, 'rb') as f:
            jt = plistlib.load(f)
        jt['Version4']['Daemon']['Override'][label] = {
            'JetsamPriority': 180, 'ActiveSoftMemoryLimit': -1, 'InactiveHardMemoryLimit': -1,
        }
        save_plist(jpath, jt)
        print('jetsam override for', label, 'in', jp)


def change_job(root, label, change):
    path, plist = load_launchd(root)
    key, job = find_job(plist, label)
    if change(job):
        save_plist(path, plist)
        print('changed', label)
    else:
        print('nothing to change for', label)
    show(key, job)


def main():
    argv = sys.argv[1:]
    if not argv:
        sys.exit(__doc__)
    mode = argv[0]
    if mode == '--copy' and len(argv) == 4:
        copy_files(*argv[1:])
    elif mode == '--env' and len(argv) >= 4:
        change_job(argv[1], argv[2], lambda job: set_environment(job, argv[3:]) or True)
    elif mode == '--wrap' and len(argv) == 4:
        change_job(argv[1], argv[2], lambda job: wrap(job, argv[3]))
    elif mode == '--unwrap' and len(argv) == 3:
        change_job(argv[1], argv[2], unwrap)
    elif mode == '--show' and len(argv) == 3:
        _path, plist = load_launchd(argv[1])
        show(*find_job(plist, argv[2]))
    elif not mode.startswith('--') and len(argv) >= 5:
        add_job(argv[0], argv[1], argv[2], argv[3], argv[4], argv[5:])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
