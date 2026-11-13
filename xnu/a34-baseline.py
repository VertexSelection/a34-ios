#!/usr/bin/env python3
"""Retarget the ruby baseline's hard-coded framebuffer constants to the Galaxy A34.

  a34-baseline.py PATCH_DIR

ruby's ci/baseline-changes.patch points ios-sweet's early console (patches 0010
and 0012) at the Redmi Note 12 Pro's LK buffer, 0x7cdfc000. On the A34 that
address is inside mblock-15-tee-reserved, so XNU's first instructions in
start.s would write into TEE memory. Point it at the A34's LK buffer 0
(0x74180000, 4352-byte rows, 2340 rows; the loader makes that buffer the
visible one) and map the 32 MB block around it as Device memory: the block
0x74000000-0x76000000 also covers the first 1 MiB of the TEE carve-out at
0x75f00000, and Device memory is never read speculatively.

Edits are applied to the patch files themselves (added and context lines
alike), so 0012 still applies on top of 0010. Each replacement must match at
least once; anything unexpected aborts the build.
"""
import os, sys

EDITS = {
    '0010-sweet-early-framebuffer-debug.patch': [
        ('(0x7cdfc000 + 2368 * 4352)', '(0x74180000 + 2308 * 4352)'),
        ('MOV64\tx10, 0x7c000000', 'MOV64\tx10, 0x74000000'),
        ('ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB)', 'ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_DISABLE)'),
        ('(uintptr_t)0x7cdfc000)', '(uintptr_t)0x74180000)'),
        ('#define EARLY_HEIGHT 2400u', '#define EARLY_HEIGHT 2340u'),
    ],
    '0012-sweet-keep-debug-console-after-ttbr0.patch': [
        ('*slot = 0x7c000000ULL', '*slot = 0x74000000ULL'),
        ('ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_WRITECOMB)', 'ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_DISABLE)'),
        ('SWEET_EARLY_FB_VA + 0xdfc000', 'SWEET_EARLY_FB_VA + 0x180000'),
        ('(uintptr_t)0x7cdfc000', '(uintptr_t)0x74180000'),
        ('#define EARLY_HEIGHT 2400u', '#define EARLY_HEIGHT 2340u'),
    ],
}


def main(d):
    for name, edits in EDITS.items():
        path = os.path.join(d, name)
        text = open(path).read()
        for old, new in edits:
            n = text.count(old)
            if n == 0:
                sys.exit(f'{name}: "{old}" not found')
            text = text.replace(old, new)
            print(f'{name}: {n}x {old!r} -> {new!r}')
        open(path, 'w').write(text)
    # Nothing of ruby's buffer may survive in these two patches.
    for name in EDITS:
        text = open(os.path.join(d, name)).read()
        for bad in ('0x7cdfc000', '0x7c000000', '0xdfc000', '2400u'):
            if bad in text:
                sys.exit(f'{name}: {bad} still present')


if __name__ == '__main__':
    main(sys.argv[1])
