// A34 XNU loader: plays iBoot for the sweet/ruby XNU kernel on the Galaxy A34.
//
// Started by Samsung LK from the recovery partition (see shim-start.S). In order:
//   1. Wakes the UFS device and clears the "boot-recovery" bootloader message
//      (a34ufs.c), so any crash or forced restart from here on boots Android.
//   2. Finds the LK framebuffer the display engine is actually scanning.
//   3. Loads the SWEETXNU payload appended at blob offset 0x10000 (built by
//      ios-sweet's mkpayload.py: pre-fixed-up kernel segments + ADT + cmdline),
//      patches /sweet-fbcon in the ADT, builds boot_args and enters XNU with the
//      MMU and caches off, x0 = boot_args.
//
// The loader part follows ios-sweet's shim/loader.c (MIT, third_party/ios-sweet).
// Progress is drawn as a row of squares at the top of the screen: one square
// per stage turns green; a red square marks the stage that failed.

#include <stdint.h>
#include "a34ufs.h"

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

#define FB_BASE       0x74180000UL
#define FB_SIZE       0x01d80000UL
#define FB_W          1080
#define FB_H          2340
#define OVL0          0x14005000UL
#define OVL0_2L       0x14006000UL
#define PAYLOAD_OFF   0x10000UL
#define BOOT_LINE_LENGTH 608
#define PAGE_16K      0x4000UL

#define GREEN   0xff00ff00u
#define RED     0xffff0000u
#define YELLOW  0xffffff00u
#define GREY    0xff606060u

extern u8 _start[];

static inline u32 rd32(u64 a) { return *(volatile u32 *)a; }

// ---------------------------------------------------------------- display --

static u64 fb_base = FB_BASE;
static u32 fb_stride = 4352;

static int layer_ok(u64 addr, u32 pitch)
{
    return pitch >= FB_W * 4 && addr >= FB_BASE && addr + (u64)pitch * FB_H <= FB_BASE + FB_SIZE;
}

// The topmost enabled overlay layer that scans an LK buffer: OVL0's layers sit
// above OVL0_2L's output; within an OVL the higher layer index is on top.
static void pick_fb(void)
{
    static const u64 ovls[2] = { OVL0, OVL0_2L };
    static const int nlayers[2] = { 4, 2 };
    for (int o = 0; o < 2; o++) {
        u32 en = rd32(ovls[o] + 0x2c);
        for (int n = nlayers[o] - 1; n >= 0; n--) {
            if (!(en & (1u << n)))
                continue;
            u64 a = rd32(ovls[o] + 0xf40 + 0x20 * n);
            u32 p = rd32(ovls[o] + 0x20 * n + 0x44) & 0xffff;
            if (layer_ok(a, p)) {
                fb_base = a;
                fb_stride = p;
                return;
            }
        }
    }
}

static void rect(u64 x, u64 y, u64 w, u64 h, u32 color)
{
    for (u64 r = 0; r < h; r++) {
        volatile u32 *row = (volatile u32 *)(fb_base + (y + r) * fb_stride + x * 4);
        for (u64 c = 0; c < w; c++)
            row[c] = color;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void stage(int n, u32 color)
{
    rect(40 + n * 120, 160, 100, 100, color);
}

// ------------------------------------------------------------- ADT patch --

static int name_is(const u8 *p, const char *s)
{
    for (int i = 0; i < 32; i++) {
        if (p[i] != (u8)s[i])
            return 0;
        if (!s[i])
            return 1;
    }
    return 0;
}

// Walk one ADT node at *off; patch /sweet-fbcon reg and stride. Returns 1 if patched.
static int adt_patch(u8 *adt, u64 *off, u64 end, int depth)
{
    if (*off + 8 > end || depth > 8)
        return 0;
    u32 nprops = *(u32 *)(adt + *off), nchildren = *(u32 *)(adt + *off + 4);
    *off += 8;
    u8 *reg = 0, *stride = 0;
    int is_fbcon = 0, done = 0;
    for (u32 i = 0; i < nprops; i++) {
        if (*off + 36 > end)
            return 0;
        u8 *name = adt + *off;
        u32 len = *(u32 *)(adt + *off + 32) & 0x7fffffff;
        u8 *val = adt + *off + 36;
        if (name_is(name, "name") && len >= 12 && name_is(val, "sweet-fbcon"))
            is_fbcon = 1;
        if (name_is(name, "reg") && len == 16)
            reg = val;
        if (name_is(name, "stride") && len == 4)
            stride = val;
        *off += 36 + ((len + 3) & ~3u);
    }
    if (is_fbcon && reg && stride) {
        u64 size = ((u64)fb_stride * FB_H + PAGE_16K - 1) & ~(PAGE_16K - 1);
        ((u64 *)reg)[0] = fb_base;
        ((u64 *)reg)[1] = size;
        *(u32 *)stride = fb_stride;
        done = 1;
    }
    for (u32 i = 0; i < nchildren; i++)
        done |= adt_patch(adt, off, end, depth + 1);
    return done;
}

// --------------------------------------------------------------- loader --

struct payload_seg { u64 vmaddr, vmsize, file_off, file_size; };

struct payload {
    char magic[8];
    u32 version, nsegs;
    u64 virt_base, entry_va, adt_off, adt_size, kernel_end;
    u64 phys_base, mem_size;
    char cmdline[BOOT_LINE_LENGTH];
    struct payload_seg segs[];
};

struct boot_video { u64 v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth; };

struct boot_args {
    u16 Revision, Version;
    u64 virtBase, physBase, memSize, topOfKernelData;
    struct boot_video Video;
    u32 machineType;
    u64 deviceTreeP;
    u32 deviceTreeLength;
    char CommandLine[BOOT_LINE_LENGTH];
    u64 bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

static void copy64(u64 dst, u64 src, u64 n)
{
    volatile u64 *d = (volatile u64 *)dst;
    volatile const u64 *s = (volatile const u64 *)src;
    for (u64 i = 0; i < (n + 7) / 8; i++)
        d[i] = s[i];
}

static void zero64(u64 dst, u64 n)
{
    volatile u64 *d = (volatile u64 *)dst;
    for (u64 i = 0; i < n / 8; i++)
        d[i] = 0;
}

typedef void (*xnu_entry_t)(u64 boot_args_pa);

u64 shim_main(u64 dtb, u64 load)
{
    (void)dtb;
    pick_fb();
    for (int i = 0; i < 6; i++)
        stage(i, GREY);

    // 1. UFS awake, recovery note cleared.
    u32 hcs, err;
    u32 uo = ufs_open(&hcs);
    u32 bcb = ufs_bcb_clear(1, &err);
    stage(0, (uo == 0 && (bcb == UFS_BCB_CLEARED || bcb == UFS_BCB_EMPTY)) ? GREEN : YELLOW);

    // 2. Payload present?
    const struct payload *p = (const struct payload *)(load + PAYLOAD_OFF);
    static const char want[8] = { 'S', 'W', 'E', 'E', 'T', 'X', 'N', 'U' };
    for (int i = 0; i < 8; i++)
        if (p->magic[i] != want[i]) {
            stage(1, RED);
            return 0;
        }
    if (p->version != 1 || (p->adt_off & 7)) {
        stage(1, RED);
        return 0;
    }
    u64 pa_of_va_base = p->phys_base - p->virt_base;
    u64 src_end = (u64)p + p->adt_off + p->adt_size;
    if (src_end > p->phys_base && (u64)p < p->phys_base + p->mem_size) {
        stage(1, RED);                      // payload overlaps kernel RAM
        return 0;
    }
    stage(1, GREEN);

    // 3. Kernel segments.
    for (u32 i = 0; i < p->nsegs; i++) {
        const struct payload_seg *s = &p->segs[i];
        u64 dst = s->vmaddr + pa_of_va_base;
        copy64(dst, (u64)p + s->file_off, s->file_size);
        if (s->vmsize > s->file_size) {
            u64 done = (s->file_size + 7) & ~7UL;
            zero64(dst + done, s->vmsize - done);
        }
    }
    stage(2, GREEN);

    // 4. ADT behind the kernel, with the live framebuffer.
    u64 adt_pa = p->kernel_end + pa_of_va_base;
    copy64(adt_pa, (u64)p + p->adt_off, p->adt_size);
    u64 off = 0;
    stage(3, adt_patch((u8 *)adt_pa, &off, p->adt_size, 0) ? GREEN : YELLOW);

    // 5. boot_args.
    u64 ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
    zero64(ba_pa, PAGE_16K);
    volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
    ba->Revision = 2;
    ba->Version = 2;
    ba->virtBase = p->virt_base;
    ba->physBase = p->phys_base;
    ba->memSize = p->mem_size;
    ba->topOfKernelData = ba_pa + PAGE_16K;
    ba->Video.v_baseAddr = fb_base;
    ba->Video.v_display = 1;
    ba->Video.v_rowBytes = fb_stride;
    ba->Video.v_width = FB_W;
    ba->Video.v_height = FB_H;
    ba->Video.v_depth = 32;
    ba->deviceTreeP = adt_pa - pa_of_va_base;   // XNU reads it with the MMU on
    ba->deviceTreeLength = (u32)p->adt_size;
    for (int i = 0; i < BOOT_LINE_LENGTH; i++)
        ba->CommandLine[i] = p->cmdline[i];
    ba->memSizeActual = p->mem_size;
    stage(4, GREEN);

    // 6. Jump. The square stays yellow if XNU never draws anything.
    stage(5, YELLOW);
    u64 entry_pa = p->entry_va + pa_of_va_base;
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
    ((xnu_entry_t)entry_pa)(ba_pa);
    stage(5, RED);
    return 0;
}
