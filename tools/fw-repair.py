#!/usr/bin/env python3
"""Inspect or rebuild a samloader-rs (2.2.0) resumable ".part" file.

samloader-rs treats a connection the server closes early as a finished chunk,
so it can rename a file that still has unwritten gaps to its final name and
throw away the resume footer. This tool rebuilds that footer from the gaps so
that samloader can resume only what is missing.

  fw-repair.py info   FILE.part
      Print the footer and how much of the download is marked complete.

  fw-repair.py holes  FILE
      Print the unwritten gaps (filesystem holes) in FILE.

  fw-repair.py rebuild HOLEY_FILE FRESH.part OUT.part
      HOLEY_FILE  the prematurely finalised download (has gaps)
      FRESH.part  a just-started download of the same firmware (valid footer)
      OUT.part    written as a sparse copy of HOLEY_FILE plus a footer whose
                  bitmap marks only the blocks that are safely complete

Footer layout (little endian), appended after the payload:
  u32 version | u64 file_size | 16B key | u32 block_size | u32 num_blocks |
  u16 len + filename | u16 len + version | bitmap | 16B md5 | u32 footer_len |
  "SAMLOADR"
The md5 covers the header followed by an all-zero bitmap.
"""
import hashlib
import os
import struct
import sys

MAGIC = b"SAMLOADR"
TRAILER = 28
PAGE = 4096


def read_footer(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        f.seek(size - 12)
        tail = f.read(12)
        if tail[4:] != MAGIC:
            raise SystemExit("%s: no SAMLOADR footer" % path)
        (flen,) = struct.unpack("<I", tail[:4])
        f.seek(size - flen)
        raw = f.read(flen)
    c = 0
    (ver,) = struct.unpack_from("<I", raw, c); c += 4
    (fsize,) = struct.unpack_from("<Q", raw, c); c += 8
    key = raw[c:c + 16]; c += 16
    (bsize,) = struct.unpack_from("<I", raw, c); c += 4
    (nblocks,) = struct.unpack_from("<I", raw, c); c += 4
    (n,) = struct.unpack_from("<H", raw, c); c += 2
    fname = raw[c:c + n].decode(); c += n
    (n,) = struct.unpack_from("<H", raw, c); c += 2
    fwver = raw[c:c + n].decode(); c += n
    bf_off = c
    bf_len = (nblocks + 7) // 8
    if bf_off + bf_len + TRAILER != flen:
        raise SystemExit("%s: footer length does not match its fields" % path)
    want = hashlib.md5(raw[:bf_off] + bytes(bf_len)).digest()
    if want != raw[bf_off + bf_len:bf_off + bf_len + 16]:
        raise SystemExit("%s: footer checksum mismatch" % path)
    if size != fsize + flen:
        raise SystemExit("%s: size %d != payload %d + footer %d" % (path, size, fsize, flen))
    if nblocks != (fsize + bsize - 1) // bsize:
        raise SystemExit("%s: block count does not match size" % path)
    return dict(raw=bytearray(raw), version=ver, file_size=fsize, key=key, block_size=bsize,
                num_blocks=nblocks, filename=fname, fwver=fwver, bf_off=bf_off, bf_len=bf_len,
                footer_len=flen)


def holes(path, limit=None):
    out = []
    fd = os.open(path, os.O_RDWR if os.access(path, os.W_OK) else os.O_RDONLY)
    try:
        try:
            os.fsync(fd)
        except OSError:
            pass
        size = os.fstat(fd).st_size if limit is None else limit
        pos = 0
        while pos < size:
            try:
                h = os.lseek(fd, pos, os.SEEK_HOLE)
            except OSError:
                break
            if h >= size:
                break
            try:
                d = os.lseek(fd, h, os.SEEK_DATA)
            except OSError:
                d = size
            d = min(d, size)
            out.append((h, d))
            pos = d
    finally:
        os.close(fd)
    return out


def done_bytes(ft):
    bf = ft["raw"][ft["bf_off"]:ft["bf_off"] + ft["bf_len"]]
    total = 0
    for b in range(ft["num_blocks"]):
        if bf[b // 8] & (1 << (b % 8)):
            total += min(ft["block_size"], ft["file_size"] - b * ft["block_size"])
    return total


def cmd_info(path):
    ft = read_footer(path)
    d = done_bytes(ft)
    print("file      : %s" % ft["filename"])
    print("firmware  : %s" % ft["fwver"])
    print("payload   : %d bytes, %d blocks of %d" % (ft["file_size"], ft["num_blocks"], ft["block_size"]))
    print("complete  : %.2f of %.2f GB (%.1f%%)" % (d / 1e9, ft["file_size"] / 1e9, 100.0 * d / ft["file_size"]))


def cmd_holes(path):
    hs = holes(path)
    print("%s: %d bytes, %d gap(s), %.2f GB missing" % (
        os.path.basename(path), os.path.getsize(path), len(hs), sum(d - h for h, d in hs) / 1e9))
    for h, d in hs:
        print("  gap %.3f .. %.3f GB (%.1f MB)" % (h / 1e9, d / 1e9, (d - h) / 1e6))
    return 1 if hs else 0


def cmd_rebuild(holey, fresh, out):
    ft = read_footer(fresh)
    S, B, N = ft["file_size"], ft["block_size"], ft["num_blocks"]
    hsize = os.path.getsize(holey)
    if not (S - 16 <= hsize <= S):
        raise SystemExit("holey file is %d bytes, expected %d minus padding" % (hsize, S))
    want_name = ft["filename"]
    for ext in (".enc4", ".enc2"):
        if want_name.endswith(ext):
            want_name = want_name[:-len(ext)]
    if os.path.basename(holey) != want_name:
        raise SystemExit("name mismatch: %s vs footer %s" % (os.path.basename(holey), ft["filename"]))
    if os.path.exists(out):
        raise SystemExit("%s already exists" % out)
    hs = holes(holey)
    # Treat the padding at the very end as missing so samloader re-fetches the last block.
    gaps = hs + [(hsize, S)]

    def safe(b):
        lo, hi = b * B, min((b + 1) * B, S)
        if b == N - 1:
            return False
        for h, d in gaps:
            if lo < d + PAGE and h - PAGE < hi:
                return False
        return True

    bf = bytearray(ft["bf_len"])
    kept = 0
    for b in range(N):
        if safe(b):
            bf[b // 8] |= 1 << (b % 8)
            kept += min(B, S - b * B)
    footer = ft["raw"]
    footer[ft["bf_off"]:ft["bf_off"] + ft["bf_len"]] = bf

    if os.system("cp --sparse=always -- '%s' '%s'" % (holey, out)) != 0:
        raise SystemExit("copy failed")
    with open(out, "r+b") as f:
        f.truncate(S)
        f.seek(S)
        f.write(footer)
        f.flush()
        os.fsync(f.fileno())
    read_footer(out)  # validates size, checksum and layout exactly as written
    print("rebuilt %s" % out)
    print("  gaps found   : %d (%.2f GB)" % (len(hs), sum(d - h for h, d in hs) / 1e9))
    print("  kept complete: %.2f of %.2f GB" % (kept / 1e9, S / 1e9))
    print("  to download  : %.2f GB" % ((S - kept) / 1e9))


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "info":
        cmd_info(a[1])
    elif len(a) == 2 and a[0] == "holes":
        sys.exit(cmd_holes(a[1]))
    elif len(a) == 4 and a[0] == "rebuild":
        cmd_rebuild(a[1], a[2], a[3])
    else:
        sys.exit(__doc__)
