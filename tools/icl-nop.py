#!/usr/bin/env python3
"""InstalledContentLibrary NOP for the iOS 16.7.16 (20H392) iPhone X shared cache.

On first boot lsd and SpringBoard deadlock behind a MobileGestalt call in
InstalledContentLibrary (__GenerateBuiltInAppInstallSessionID), which waits
for an IONetworkController that this phone does not have. The phone then sits
on the Apple logo forever while bluetoothd/fairplayd respawn. Replacing that
one `bl` with `nop` lets the first boot reach the lock screen. Found and
documented by lky005/ruby-ios-bringup (docs/en/05-userland.md, MIT); the
offsets and checksums below are theirs and match our image exactly.

  icl-nop.py cache PATH        patch dyld_shared_cache_arm64.15 in place
                               (a mounted root image)
  icl-nop.py block IN OUT      patch a 4096-byte block read from the root
                               (block ROOT_BLOCK of an image made by
                               mkiosroot.sh); writes OUT

Both modes check the whole 4 KiB block against the known original checksum
first, and say so (exit 0) if it is already patched.
"""
import hashlib
import sys

CACHE_OFFSET = 0x1619f0c          # in dyld_shared_cache_arm64.15 (VA 0x1b11b9f0c)
ROOT_BLOCK = 2438140              # 4 KiB block of that byte in a mkiosroot.sh image
ORIG = bytes.fromhex("02255e95")  # bl 0x1b6943314
NOP = bytes.fromhex("1f2003d5")
ORIG_SHA = "ae3178046fe7cb9e08291c39dc4773429be26c35472c6fbd9135d5f2a225fd73"
PATCHED_SHA = "4a4895422dcec766b90b65c067ecb06ef8fd3e346c03ee7e2464a85f62a73b74"
IN_BLOCK = CACHE_OFFSET % 4096


def patch_block(blk):
    """Return the patched block, or None if blk is already patched."""
    sha = hashlib.sha256(blk).hexdigest()
    if sha == PATCHED_SHA:
        return None
    if sha != ORIG_SHA or blk[IN_BLOCK:IN_BLOCK + 4] != ORIG:
        sys.exit(f"block does not match the known original (sha256 {sha}); not patching")
    new = blk[:IN_BLOCK] + NOP + blk[IN_BLOCK + 4:]
    assert hashlib.sha256(new).hexdigest() == PATCHED_SHA
    return new


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "cache":
        base = CACHE_OFFSET - IN_BLOCK
        with open(sys.argv[2], "r+b") as f:
            f.seek(base)
            new = patch_block(f.read(4096))
            if new is None:
                print("ICL NOP: already patched")
                return
            f.seek(base)
            f.write(new)
            f.flush()
            f.seek(base)
            assert hashlib.sha256(f.read(4096)).hexdigest() == PATCHED_SHA
        print("ICL NOP: patched", sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == "block":
        blk = open(sys.argv[2], "rb").read()
        if len(blk) != 4096:
            sys.exit(f"{sys.argv[2]}: {len(blk)} bytes, expected 4096")
        new = patch_block(blk)
        open(sys.argv[3], "wb").write(blk if new is None else new)
        print("ICL NOP: already patched" if new is None else "ICL NOP: block patched")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
