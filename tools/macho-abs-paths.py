#!/usr/bin/env python3
"""Rewrite @rpath/@loader_path/@executable_path dylib references to an
absolute directory, in place, without moving any load command.

  macho-abs-paths.py DIR FILE...

Every LC_ID_DYLIB, LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB and
LC_LOAD_UPWARD_DYLIB whose name starts with "@" becomes DIR/<basename>; the
new name must fit in the command's existing string space (padded with NULs),
so DIR should be short. iOS dyld on this port refuses "@" path expansion
("security policy does not allow @ path expansion") for our programs.
The ad-hoc code signature is left as is (code signing is not enforced here).
"""
import os
import struct
import sys

DYLIB_CMDS = {0xc: 'LC_LOAD_DYLIB', 0xd: 'LC_ID_DYLIB', 0x80000018: 'LC_LOAD_WEAK_DYLIB',
              0x8000001f: 'LC_REEXPORT_DYLIB', 0x80000023: 'LC_LOAD_UPWARD_DYLIB'}


def patch(path, newdir):
    data = bytearray(open(path, 'rb').read())
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', data, 0)
    if magic != 0xfeedfacf:
        sys.exit(f'{path}: not a 64-bit little-endian Mach-O (fat files not handled)')
    off, changed = 32, 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd in DYLIB_CMDS:
            name_off = struct.unpack_from('<I', data, off + 8)[0]
            start, end = off + name_off, off + size
            old = bytes(data[start:end]).split(b'\0')[0].decode()
            if old.startswith('@'):
                new = (newdir.rstrip('/') + '/' + os.path.basename(old)).encode()
                if len(new) + 1 > end - start:
                    sys.exit(f'{path}: {old} -> {new.decode()} does not fit ({end - start} bytes)')
                data[start:end] = new + b'\0' * (end - start - len(new))
                print(f'{os.path.basename(path)}: {DYLIB_CMDS[cmd]} {old} -> {new.decode()}')
                changed += 1
        off += size
    if changed:
        open(path, 'wb').write(data)
    return changed


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    total = sum(patch(p, sys.argv[1]) for p in sys.argv[2:])
    print(f'{total} references rewritten')


if __name__ == '__main__':
    main()
