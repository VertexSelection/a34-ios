#!/usr/bin/env python3
"""Build the Apple Device Tree for the sweet/ruby XNU on the Galaxy A34 (MT6877).

  mkadt_a34.py OUT.adt [--dump] [--d22ap PATH]

Based on ios-sweet's tools/mkadt.py (MIT, third_party/ios-sweet): same node
layout, because the ruby kernel is the sweet kernel plus MT6877 patches and
looks its hardware up by these node names (/sweet-ufs, /sweet-fbcon, /arm-io/gic).
Every MT6877/A34 value below comes from the phone's live FDT
(recon/android-DYG1/live.dts) or from what our test shim read at handoff.

The framebuffer entries are defaults: the shim checks which LK buffer the
display engine is scanning and rewrites /sweet-fbcon reg and stride before
starting XNU.
"""
import argparse, hashlib, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'third_party', 'ios-sweet', 'tools'))
import adt  # noqa: E402

# Kernel-managed RAM: 0x1_40000000 up to the one-page "dramc-rk1" reservation at
# 0x2_3ffff000. Nothing else in reserved-memory sits above 0x1_00000000, and
# the dynamic pools are all limited below 4 GiB.
PHYS_BASE = 0x140000000
MEM_SIZE = 0xfffff000 & ~0x3fff
# Panic log carved off the top (see ios-sweet: without /pram the kernel hangs
# after "kdp_core zlib memory").
PANIC_LOG_SIZE = 0x100000
# /arm-io children are relative to this; it must not be 0.
SOC_BASE = 0x01000000

# interrupt-controller: reg = <0xc000000 0x40000>, <0xc040000 0x200000>
GICD, GICD_SIZE = 0x0c000000, 0x40000
GICR, GICR_SIZE = 0x0c040000, 0x200000
# arch timer: clock-frequency 0xc65d40, virtual timer PPI 11 = INTID 27
TIMEBASE = 13000000
VTIMER_INTID = 27
# LK framebuffer reservation (mblock-23-framebuffer) and buffer 0 of three
FB, FB_SIZE = 0x74180000, 0x1d80000
FB_W, FB_H, FB_STRIDE = 1080, 2340, 4352
# dsi@14013000 and disp_mutex@14001000
DSI0, DISP_MUTEX = 0x14013000, 0x14001000
# ufshci@11270000 "mediatek,mt8183-ufshci", reg size 0x2300
UFS, UFS_SIZE = 0x11270000, 0x2300

A34_ECID = 0x4133345800000001  # 'A34X' + 1, synthetic (not the phone's)
BOOT_MANIFEST_HASH = hashlib.sha384(b'a34x boot manifest').digest()
D22AP_ADT = os.path.join(HERE, '..', 'research', 'ipsw-x', 'd22ap.adt')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def d22ap_product(path):
    """/product of the real iPhone X tree (MobileGestalt identity). Optional:
    the kernel boots without it; the iOS userland needs it."""
    if not path or not os.path.exists(path):
        print(f'note: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(adt.load(path))
    for c in root.children:
        if c.prop('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')


def build(d22ap=D22AP_ADT, seed=None):
    phys_base, mem_size = PHYS_BASE, MEM_SIZE
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('a34-shim')),
        ('system-firmware-version', cstr('a34-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        ('display-scale', u32(3)),
        ('unique-chip-id', struct.pack('<Q', A34_ECID)),
        ('chip-id', u32(0x8015)),
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # MT6877: cores 0-5 Cortex-A55 (MPIDR 0x000-0x500), 6-7 big cores
    # (0x600, 0x700). One homogeneous cluster as in sweet; cpus=N boots a prefix.
    cpu_nodes = []
    for core in range(8):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,kryo470') + cstr('ARM,v8')),
            ('reg', u32(core << 8)),
            ('cpu-id', u32(core)),
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(TIMEBASE)),
            ('clock-frequency', u32(2600000000 if core >= 6 else 2000000000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(2133000000)),
            ('peripheral-frequency', u32(TIMEBASE)),
            ('fixed-frequency', u32(TIMEBASE)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(VTIMER_INTID)),
        ('reg', u64(GICD - SOC_BASE, GICD_SIZE, GICR - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    # xnu/patches/0900-a34-display-kick.patch: DSI0 and the display mutex, so
    # the framebuffer console can ask the command-mode panel for frames.
    disp = node('a34-disp', [
        ph(),
        ('reg', u64(DSI0, 0x1000, DISP_MUTEX, 0x1000)),
    ])

    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('mediatek,mt8183-ufshci')),
        ('reg', u64(UFS, UFS_SIZE)),
    ])

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])
    product_props = d22ap_product(d22ap)
    if product_props:
        product_props = [(k, cstr('sRGB') if k == 'artwork-display-gamut' else v) for k, v in product_props]
    product = [node('product', [ph()] + product_props)] if product_props else []
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,sweet') +
         cstr('samsung,a34x') + cstr('mediatek,mt6877') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, disp, buttons, pram] + product + [ufs])


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--d22ap', default=D22AP_ADT)
    a = ap.parse_args()
    blob = adt.serialize(build(a.d22ap))
    open(a.out, 'wb').write(blob)
    if a.dump:
        adt.dump(adt.parse(blob)[0])
    print(f'{a.out}: {len(blob)} bytes', file=sys.stderr)
