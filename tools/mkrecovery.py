#!/usr/bin/env python3
"""Build a recovery image for the Galaxy A34 that boots our own code.

  mkrecovery.py TEMPLATE.img KERNEL.bin OUT.img [CMDLINE_APPEND]

TEMPLATE is a stock recovery image (Android boot image header v2). Its header,
ramdisk, recovery DTBO and DTB are reused unchanged; only the kernel is replaced
by KERNEL (gzip-compressed here, like the stock and TWRP kernels). The layout
copies TWRP's, which is known to boot on this bootloader: payload, then
"SEANDROIDENFORCE", then an AVB hash footer for a 64 MiB partition signed with
AOSP's test key (added by avbtool afterwards, see build-hello.sh).
"""
import gzip
import hashlib
import struct
import sys

PAGE_SIZE_OFF = 36


def pad(data, page):
    return data + b"\0" * ((-len(data)) % page)


def main(template, kernel_path, out, cmdline_append=""):
    t = open(template, "rb").read()
    if t[:8] != b"ANDROID!":
        sys.exit("template is not an Android boot image")
    ksz, kaddr, rsz, raddr, ssz, saddr, tags, page = struct.unpack_from("<8I", t, 8)
    hv = struct.unpack_from("<I", t, 40)[0]
    if hv != 2:
        sys.exit("expected header v2, got v%d" % hv)
    rdo_sz, rdo_off, hsz = struct.unpack_from("<IQI", t, 1632)
    dtb_sz, dtb_addr = struct.unpack_from("<IQ", t, 1648)

    def section(off, size):
        return t[off:off + size]

    off = page + len(pad(b"\0" * ksz, page))
    ramdisk = section(off, rsz)
    off += len(pad(ramdisk, page))
    second = section(off, ssz)
    off += len(pad(second, page)) if ssz else 0
    if rdo_off != off:
        sys.exit("unexpected recovery_dtbo offset %d (computed %d)" % (rdo_off, off))
    rdo = section(off, rdo_sz)
    off += len(pad(rdo, page))
    dtb = section(off, dtb_sz)

    raw = open(kernel_path, "rb").read()
    if raw[56:60] != b"ARM\x64":
        sys.exit("kernel has no arm64 Image header")
    kernel = gzip.compress(raw, compresslevel=9, mtime=0)

    hdr = bytearray(t[:page])
    struct.pack_into("<I", hdr, 8, len(kernel))
    if cmdline_append:
        cmd = bytes(hdr[64:576]).split(b"\0")[0] + b" " + cmdline_append.encode("ascii")
        if len(cmd) > 511:
            sys.exit("command line too long: %d bytes" % len(cmd))
        hdr[64:576] = cmd + b"\0" * (512 - len(cmd))
    new_rdo_off = page + len(pad(kernel, page)) + len(pad(ramdisk, page)) + (len(pad(second, page)) if ssz else 0)
    struct.pack_into("<Q", hdr, 1636, new_rdo_off)

    # mkbootimg's id: SHA-1 over each section followed by its size.
    h = hashlib.sha1()
    for blob in (kernel, ramdisk, second, rdo, dtb):
        h.update(blob)
        h.update(struct.pack("<I", len(blob)))
    hdr[576:608] = h.digest() + b"\0" * 12

    body = bytes(hdr) + pad(kernel, page) + pad(ramdisk, page)
    if ssz:
        body += pad(second, page)
    body += pad(rdo, page) + pad(dtb, page)
    body += b"SEANDROIDENFORCE"
    open(out, "wb").write(body)
    print("kernel %d bytes (gzip of %d), ramdisk %d, recovery_dtbo %d @ %d, dtb %d; image %d bytes" % (
        len(kernel), len(raw), len(ramdisk), len(rdo), new_rdo_off, len(dtb), len(body)))


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    main(*sys.argv[1:])
