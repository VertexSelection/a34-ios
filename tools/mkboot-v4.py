#!/usr/bin/env python3
"""Pack our loader + XNU payload as a boot image v4 for the `boot` partition (cellular stage 1a).

  mkboot-v4.py BLOB OUT.img [--template STOCK_BOOT.img]

Why: LK loads and verifies the modem image (md1img) only on a normal boot, never on a recovery
boot ("[ccci] recovery mode, no need load modem" in lk-verified.img; docs/cellular-plan.md).
XNU therefore has to start from `boot` for the modem to exist.

BLOB is build-xnu.sh's build/xnu-blob.bin (loader, XNU payload, TWRP kernel). In a normal boot
LK hands over vendor_boot's/init_boot's ramdisk and the normal device tree, so the loader's
Volume Up escape cannot start a working TWRP from here; use the bootloader's recovery key combo
(Volume Up + Power) for TWRP in `recovery` instead.

The header copies the stock boot.img's os_version (the template, default the DYG1 stock image);
kernel = gzip of BLOB (as in the stock and recovery images), no ramdisk (LK takes vendor_boot's
and init_boot's), then "SEANDROIDENFORCE" and an AVB hash footer for a 64 MiB `boot` partition
signed with AOSP's test key, like build-xnu.sh does for `recovery`.

UNTESTED on the phone. Flashing it replaces Android's kernel: Android does not start again until
the stock boot.img is flashed back (download mode). Nothing else is touched; never flash any of
the identity partitions (README).
"""
import argparse
import gzip
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PAGE = 4096
BOOT_PARTITION_SIZE = 64 << 20          # PIT: boot, 16384 blocks of 4 KiB
HEADER_V4_SIZE = 1584


def pad(b):
    return b + b"\0" * (-len(b) % PAGE)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("blob")
    ap.add_argument("out")
    ap.add_argument("--template", default=os.path.join(
        ROOT, "firmware/A346EXXSADYG1_TUR/flash/boot.img"))
    ap.add_argument("--no-avb", action="store_true", help="skip the AVB footer (for inspection)")
    a = ap.parse_args()

    t = open(a.template, "rb").read(PAGE)
    if t[:8] != b"ANDROID!" or struct.unpack_from("<I", t, 40)[0] != 4:
        sys.exit("template is not a boot image v4")
    os_version = struct.unpack_from("<I", t, 16)[0]

    blob = open(a.blob, "rb").read()
    if blob[56:60] != b"ARM\x64":
        sys.exit("blob has no arm64 Image header (expected build/xnu-blob.bin)")
    kernel = gzip.compress(blob, compresslevel=9, mtime=0)

    hdr = bytearray(PAGE)
    hdr[0:8] = b"ANDROID!"
    # kernel_size, ramdisk_size, os_version, header_size, reserved[4], header_version
    struct.pack_into("<IIII", hdr, 8, len(kernel), 0, os_version, HEADER_V4_SIZE)
    struct.pack_into("<I", hdr, 40, 4)
    # cmdline[1536] at 44: empty (boot-args travel in our own payload)
    struct.pack_into("<I", hdr, 44 + 1536, 0)       # signature_size: no boot signature
    body = bytes(hdr) + pad(kernel) + b"SEANDROIDENFORCE"
    if len(body) > BOOT_PARTITION_SIZE - (1 << 20):
        sys.exit("image too large for the boot partition: %d bytes" % len(body))
    open(a.out, "wb").write(body)
    print("boot v4: kernel %d bytes (gzip of %d), os_version 0x%08x, image %d bytes" % (
        len(kernel), len(blob), os_version, len(body)))

    if not a.no_avb:
        subprocess.check_call([
            sys.executable, "-I", os.path.join(ROOT, "ref/avb/avbtool.py"), "add_hash_footer",
            "--image", a.out, "--partition_name", "boot",
            "--partition_size", str(BOOT_PARTITION_SIZE),
            "--key", os.path.join(ROOT, "ref/avb/testkey_rsa4096.pem"),
            "--algorithm", "SHA256_RSA4096"])
        print("AVB footer for a %d MiB boot partition added" % (BOOT_PARTITION_SIZE >> 20))


if __name__ == "__main__":
    main()
