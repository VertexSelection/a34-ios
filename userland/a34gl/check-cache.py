#!/usr/bin/env python3
"""Check a dyld shared cache against the addresses and offsets liba34gl relies on.

  check-cache.py CACHE_DIR [SLOTS]
      CACHE_DIR: a mounted iOS root's System/Library/Caches/com.apple.dyld
      SLOTS:     eagl_slots.txt (default: the one next to this script)

1. inject.c replaces two entries of CoreAnimation's CA::OGL::GLESContext virtual table, at
   addresses of the iOS 16.7.16 (20H392) iPhone X cache (from ios-topaz's topaz_inject.c); at
   run time it only does so when each entry holds the function it expects. This does the same
   check off the phone, through the cache's slide info.
2. eagl.c hands CoreAnimation a function table laid out as iOS's OpenGLES lays out the one
   behind -[EAGLContext getMacroContextPrivate] (eagl_slots.txt, HackiOS, read off 16.5). Every
   exported gl* function of OpenGLES is a stub that loads the GL context from thread-local
   storage and jumps through the entry at that offset (plus the table's place in the context):
   for each named slot whose function OpenGLES exports, the stub's loads must include it.

Prints the cache's UUID. Exit status 0 when everything matches. Reads only.
"""
import os
import struct
import sys

# (what, vtable slot, expected function) -- keep in step with inject.c
VT_GLES_CONTEXT = 0x1d5b2f6e8
VTABLE_CHECKS = [
    ('GLESContext::function_supported', VT_GLES_CONTEXT + 0x10 + 0x378, 0x1884b2164),
    ('GLESContext::function_uniform', VT_GLES_CONTEXT + 0x10 + 0x388, 0x1884b21ac),
]
OPENGLES = '/System/Library/Frameworks/OpenGLES.framework/OpenGLES'


class Cache:
    def __init__(self, directory):
        self.maps = []                  # (address, size, file, file offset, slide info or None)
        self.uuid = None
        self.directory = directory
        names = sorted(n for n in os.listdir(directory) if n.startswith('dyld_shared_cache_arm64')
                       and not n.endswith(('.map', '.symbols', '.atlas')))
        if not names:
            sys.exit('no dyld_shared_cache_arm64* in ' + directory)
        for name in names:
            f = open(os.path.join(directory, name), 'rb')
            head = f.read(0x200)
            if not head.startswith(b'dyld_v1'):
                continue
            mapping_offset, mapping_count = struct.unpack_from('<II', head, 0x10)
            if name == 'dyld_shared_cache_arm64':
                self.uuid = head[0x58:0x68]
                self.main = f
            with_slide_offset, with_slide_count = (struct.unpack_from('<II', head, 0x138)
                                                   if mapping_offset >= 0x140 else (0, 0))
            if with_slide_count:
                f.seek(with_slide_offset)
                for _ in range(with_slide_count):
                    address, size, offset, slide_offset, slide_size, _flags, _maxp, _initp = \
                        struct.unpack('<QQQQQQII', f.read(56))
                    self.maps.append((address, size, f, offset, (slide_offset, slide_size) if slide_size else None))
            else:
                f.seek(mapping_offset)
                for _ in range(mapping_count):
                    address, size, offset, _maxp, _initp = struct.unpack('<QQQII', f.read(32))
                    self.maps.append((address, size, f, offset, None))

    def where(self, address):
        for start, size, f, offset, slide in self.maps:
            if start <= address < start + size:
                return f, offset + (address - start), slide
        return None, None, None

    def read(self, address, size):
        f, offset, _ = self.where(address)
        if f is None:
            return None
        f.seek(offset)
        return f.read(size)

    def pointer(self, address):
        f, offset, slide = self.where(address)
        if f is None:
            return None
        f.seek(offset)
        raw = struct.unpack('<Q', f.read(8))[0]
        if slide is None:
            return raw
        f.seek(slide[0])
        version = struct.unpack('<I', f.read(4))[0]
        if version == 2:                # arm64: the target in the bits outside delta_mask
            f.seek(slide[0])
            _v, _page, _starts, _count, _extras, _ecount, delta_mask, value_add = struct.unpack('<IIIIIIQQ', f.read(40))
            value = raw & ~delta_mask & 0xffffffffffffffff
            return value + value_add if value else 0
        if version == 3:                # arm64e
            f.seek(slide[0])
            _v, _page, _count, auth_value_add = struct.unpack('<IIIQ', f.read(20))
            if raw >> 63:
                return (raw & 0xffffffff) + auth_value_add
            return (raw & 0x7ffffffffff) | ((raw >> 43) & 0xff) << 56
        sys.exit('slide info version %d is not known here' % version)

    def image(self, path):
        head = self.main
        head.seek(0x1c0)
        images_offset, images_count = struct.unpack('<II', head.read(8))
        for i in range(images_count):
            head.seek(images_offset + 32 * i)
            address, _mtime, _inode, path_offset, _pad = struct.unpack('<QQQII', head.read(32))
            head.seek(path_offset)
            if head.read(len(path) + 1) == path.encode() + b'\0':
                return address
        return None

    def exports(self, address):
        """name -> address of a cache image's defined external symbols (its symbol table)."""
        header = self.read(address, 0x8000)
        ncmds = struct.unpack_from('<I', header, 16)[0]
        at, linkedit, symtab = 32, None, None
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', header, at)
            if cmd == 0x19 and header[at + 8:at + 24].split(b'\0')[0] == b'__LINKEDIT':
                vmaddr, _vmsize, fileoff = struct.unpack_from('<QQQ', header, at + 24)
                linkedit = (vmaddr, fileoff)
            elif cmd == 0x2:
                symtab = struct.unpack_from('<IIII', header, at + 8)
            at += size
        symoff, nsyms, stroff, strsize = symtab
        symbols = self.read(linkedit[0] + symoff - linkedit[1], nsyms * 16)
        strings = self.read(linkedit[0] + stroff - linkedit[1], strsize)
        found = {}
        for i in range(nsyms):
            strx, kind, _sect, _desc, value = struct.unpack_from('<IBBHQ', symbols, 16 * i)
            if kind & 0x0e == 0x0e and kind & 0x01 and value:        # N_SECT | N_EXT
                found[strings[strx:strings.index(b'\0', strx)].decode()] = value
        return found


def stub_offsets(code):
    """The immediate offsets of the 64-bit loads of a stub, up to its indirect branch."""
    loads = []
    for k in range(len(code) // 4):
        ins = struct.unpack_from('<I', code, 4 * k)[0]
        if ins & 0xffc00000 == 0xf9400000:              # LDR Xt, [Xn, #imm]
            loads.append(((ins >> 10) & 0xfff) * 8)
        if ins & 0xfffffc1f in (0xd61f0000, 0xd63f0000):  # BR / BLR
            break
    return loads


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    cache = Cache(sys.argv[1])
    slots_path = sys.argv[2] if len(sys.argv) == 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'eagl_slots.txt')
    bad = 0
    uuid = cache.uuid.hex() if cache.uuid else '?' * 32
    print('cache UUID %s-%s-%s-%s-%s' % (uuid[:8], uuid[8:12], uuid[12:16], uuid[16:20], uuid[20:]))

    for what, slot, expected in VTABLE_CHECKS:
        held = cache.pointer(slot)
        ok = held == expected
        bad += not ok
        print('%-34s slot %#x holds %s, expected %#x: %s' % (
            what, slot, '%#x' % held if held is not None else 'nothing (unmapped)', expected, 'ok' if ok else 'DIFFERENT'))

    opengles = cache.image(OPENGLES)
    if opengles is None:
        print('no %s in the cache' % OPENGLES)
        sys.exit(1)
    exported = cache.exports(opengles)
    stubs = []                                  # (slot, name, the stub's loads)
    for line in open(slots_path):
        words = line.split('#')[0].split()
        if not words:
            continue
        offset, names = int(words[0], 16), words[2:]
        for name in names:
            address = exported.get('_' + name)
            if address is not None:
                stubs.append((offset, name, stub_offsets(cache.read(address, 64))))
                break
    # where the table sits in the context the stubs load: the one distance every stub agrees on
    common = None
    for offset, _name, loads in stubs:
        distances = {x - offset for x in loads if x >= offset}
        common = distances if common is None else common & distances
    base = min(common) if common else None
    checked = differ = 0
    for offset, name, loads in stubs:
        checked += 1
        if base is None or offset + base not in loads:
            differ += 1
            print('slot %#06x %-36s stub loads %s: DIFFERENT' % (offset, name, ' '.join('%#x' % x for x in loads)))
    print('dispatch table: %d slots checked against OpenGLES\'s stubs (table at +%s in the context), %d different' % (
        checked, '%#x' % base if base is not None else '?', differ))
    bad += differ != 0 or checked == 0
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
