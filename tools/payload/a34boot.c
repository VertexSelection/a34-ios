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
extern const u64 shim_payload_size, shim_payload2_size;

static inline u32 rd32(u64 a) { return *(volatile u32 *)a; }

// ---------------------------------------------------------------- display --
//
// At handoff LK scans two layers: OVL0_2L layer 0 (pitch 4352, the base) and,
// on some boots, OVL0 layer 1 (pitch 4320) on top of it. Which one is visible
// varies, so the loader draws its progress squares into every scanned buffer,
// then points the OVL0 layers at the base buffer so XNU's console, drawn
// there, is visible either way.

static inline void wr32(u64 a, u32 v) { *(volatile u32 *)a = v; }

#define MAX_FBS 6
static u64 fbs[MAX_FBS];
static u32 fb_pitches[MAX_FBS];
static int nfbs;
// What XNU gets. Fixed: the kernel's early console (xnu/a34-baseline.py) is
// built for LK buffer 0 with 4352-byte rows.
static const u64 fb_base = FB_BASE;
static const u32 fb_stride = 4352;

static int layer_ok(u64 addr, u32 pitch)
{
    return pitch >= FB_W * 4 && pitch <= 8192 && addr >= FB_BASE &&
           addr + (u64)pitch * FB_H <= FB_BASE + FB_SIZE;
}

static void find_fbs(void)
{
    static const u64 ovls[2] = { OVL0_2L, OVL0 };
    static const int nlayers[2] = { 2, 4 };
    int have_base = 0;
    for (int o = 0; o < 2; o++) {
        u32 en = rd32(ovls[o] + 0x2c);
        for (int n = 0; n < nlayers[o]; n++) {
            if (!(en & (1u << n)))
                continue;
            u64 a = rd32(ovls[o] + 0xf40 + 0x20 * n);
            u32 p = rd32(ovls[o] + 0x20 * n + 0x44) & 0xffff;
            if (!layer_ok(a, p) || nfbs == MAX_FBS)
                continue;
            fbs[nfbs] = a;
            fb_pitches[nfbs++] = p;
            if (a == fb_base && p == fb_stride)
                have_base = 1;
        }
    }
    if (!have_base && nfbs < MAX_FBS) {
        fbs[nfbs] = fb_base;
        fb_pitches[nfbs++] = fb_stride;
    }
}

// Make every enabled layer of OVL0_2L and OVL0 scan the base buffer with the
// base pitch, so whichever layer is on top shows what XNU draws.
static void unify_layers(void)
{
    static const u64 ovls[2] = { OVL0_2L, OVL0 };
    static const int nlayers[2] = { 2, 4 };
    for (int o = 0; o < 2; o++) {
        u32 en = rd32(ovls[o] + 0x2c);
        for (int n = 0; n < nlayers[o]; n++) {
            if (!(en & (1u << n)))
                continue;
            u64 l = ovls[o] + 0x20 * n;
            wr32(ovls[o] + 0xf40 + 0x20 * n, (u32)fb_base);
            wr32(l + 0x44, (rd32(l + 0x44) & 0xffff0000u) | fb_stride);
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

#define DSI0    0x14013000UL
#define MUTEX   0x14001000UL

// Command-mode panel: ask the display engine for one frame (display mutexes
// with modules attached re-enabled, then DSI0 started), after waiting for a
// frame in flight. Same sequence as xnu/patches/0900-a34-display-kick.patch.
static void kick(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
    if (rd32(DSI0 + 0x14) & 3)
        return;
    for (int i = 0; i < 2000000 && (rd32(DSI0 + 0x0c) & (1u << 31)); i++)
        ;
    for (u64 n = 0; n < 16; n++) {
        u64 m = MUTEX + 0x20 * n;
        if (!rd32(m + 0x30))
            continue;
        wr32(m + 0x20, 0);
        wr32(m + 0x20, 1);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    wr32(DSI0 + 0x00, 0);
    wr32(DSI0 + 0x00, 1);
    __asm__ volatile("dsb sy" ::: "memory");
}

static void clear_base(void)
{
    volatile u64 *p = (volatile u64 *)fb_base;
    for (u64 i = 0; i < (u64)fb_stride * FB_H / 8; i++)
        p[i] = 0xff000000ff000000ull;
}

static void rect(u64 x, u64 y, u64 w, u64 h, u32 color)
{
    for (int f = 0; f < nfbs; f++)
        for (u64 r = 0; r < h; r++) {
            volatile u32 *row = (volatile u32 *)(fbs[f] + (y + r) * fb_pitches[f] + x * 4);
            for (u64 c = 0; c < w; c++)
                row[c] = color;
        }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void stage(int n, u32 color)
{
    rect(40 + n * 120, 160, 100, 100, color);
    kick();
}

// -------------------------------------------------------- watchdog, GIC --

#define TOPRGU          0x10007000UL
#define GICD            0x0c000000UL
#define GICR            0x0c040000UL
#define GICR_SIZE       0x200000UL
#define GICR_PE_SIZE    0x20000UL
#define VTIMER_INTID    27
#define PTIMER_INTID    30
#define SGI_INTID       1

#define RSR(r) ({ u64 _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define WSR(r, v) __asm__ volatile("msr " r ", %0" :: "r"((u64)(v)))

// LK arms MediaTek's TOPRGU watchdog; XNU never feeds it, so it resets the
// phone ~25 s in. Turn it off as the ruby loader does (key 0x22, enable clear).
static void wdt_off(void)
{
    wr32(TOPRGU + 0x00, 0x22000000u);
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void wr8(u64 a, u8 v) { *(volatile u8 *)a = v; }
static inline u64 rd64(u64 a) { return *(volatile u64 *)a; }

// Boot CPU GIC setup before XNU, after ios-sweet/ios-topaz's test_gic() and
// gold_gicr_prepare(): without it XNU hangs after initCPU (no timer/SGI).
static int gic_prepare(void)
{
    u64 sre = RSR("S3_0_C12_C12_5");                 // ICC_SRE_EL1
    if (!(sre & 1)) {
        WSR("S3_0_C12_C12_5", sre | 1);
        __asm__ volatile("isb");
    }
    if (!(RSR("S3_0_C12_C12_5") & 1))
        return 0;

    wr32(GICD, rd32(GICD) | 0x13);                   // EnableGrp1(NS), Grp1A, ARE_NS
    for (int i = 0; i < 1000000 && (rd32(GICD) & (1u << 31)); i++)
        ;

    u64 mpidr = RSR("mpidr_el1");
    u32 aff = (u32)((mpidr & 0xffffff) | ((mpidr >> 32) & 0xff) << 24);
    u64 r = 0;
    for (u64 off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
        u64 typer = rd64(GICR + off + 0x08);
        if ((u32)(typer >> 32) == aff) {
            r = GICR + off;
            break;
        }
        if (typer & (1 << 4))                        // Last
            break;
    }
    if (!r)
        return 0;
    u32 waker = rd32(r + 0x14);
    if (waker & 2) {
        wr32(r + 0x14, waker & ~2u);
        for (int i = 0; i < 100000 && (rd32(r + 0x14) & 4); i++)
            ;
    }
    u64 sgi = r + 0x10000;
    wr32(sgi + 0x080, 0xffffffff);                   // IGROUPR0: all Group 1
    for (int id = 0; id < 32; id++)
        wr8(sgi + 0x400 + id, 0xa0);                 // IPRIORITYR
    wr32(sgi + 0x100, (1u << VTIMER_INTID) | (1u << PTIMER_INTID) | (1u << SGI_INTID));

    u64 ctlr = RSR("S3_0_C12_C12_4");                // ICC_CTLR_EL1
    WSR("S3_0_C12_C12_4", ctlr & ~2ull);             // EOImode 0
    WSR("S3_0_C4_C6_0", 0xf0);                       // ICC_PMR_EL1
    WSR("S3_0_C12_C12_3", 0);                        // ICC_BPR1_EL1
    WSR("S3_0_C12_C12_7", 1);                        // ICC_IGRPEN1_EL1
    __asm__ volatile("isb");
    return 1;
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

// ---------------------------------------------------------- TWRP escape --
//
// Holding Volume Up while the loader starts boots the TWRP kernel carried in
// the same image instead of XNU (root shell over adb, for logs and new test
// images once Android no longer boots). Volume Up is the only key on the
// MediaTek keypad (kp@10010000, hw-init-map index 1 = KEY_VOLUMEUP); KP_MEM1
// bit 1 reads 0 while it is pressed.

#define KPD_MEM1    0x10010004UL
#define TWO_MB      0x200000UL
#define RAM_LIMIT   0x46c00000UL        // secdbg-kernel-log starts here

static u64 now(void)
{
    u64 t;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
    return t;
}

static int volume_up_held(u64 ms)
{
    u64 end = now() + 13000ull * ms;
    while (now() < end)
        if (!(rd32(KPD_MEM1) & 2))
            return 1;
    return 0;
}

// Copy the second payload (an arm64 Image) to the next 2 MiB boundary after
// the blob; returns its entry point or 0.
static u64 place_image(u64 src, u64 len)
{
    if (!len || *(volatile u32 *)(src + 56) != 0x644d5241)
        return 0;
    u64 text_offset = *(volatile u64 *)(src + 8);
    u64 kimage_size = *(volatile u64 *)(src + 16);
    if (kimage_size < len)
        kimage_size = len;
    u64 dest = (src + len + TWO_MB - 1) & ~(TWO_MB - 1);
    if (dest + text_offset + kimage_size > RAM_LIMIT)
        return 0;
    copy64(dest + text_offset, src, len);
    __asm__ volatile("dsb sy" ::: "memory");
    return dest + text_offset;
}

u64 shim_main(u64 dtb, u64 load)
{
    (void)dtb;
    // 1. UFS awake and recovery note cleared first, then the watchdog off:
    //    from here on any hang or forced restart lands in Android.
    u32 hcs, err;
    u32 uo = ufs_open(&hcs);
    u32 bcb = ufs_bcb_clear(1, &err);
    wdt_off();

    // Display: every layer onto buffer 0, cleared to black, progress squares.
    find_fbs();
    unify_layers();
    clear_base();
    for (int i = 0; i < 8; i++)
        stage(i, GREY);
    stage(0, (uo == 0 && (bcb == UFS_BCB_CLEARED || bcb == UFS_BCB_EMPTY)) ? GREEN : YELLOW);
    stage(6, GREEN);
    stage(7, gic_prepare() ? GREEN : YELLOW);

    // TWRP escape: a white bar for two seconds; Volume Up held -> TWRP.
    rect(40, 300, FB_W - 80, 40, 0xffffffffu);
    kick();
    if (volume_up_held(2000)) {
        rect(40, 300, FB_W - 80, 40, 0xff0060ffu);
        kick();
        u64 twrp_src = load + PAYLOAD_OFF + ((shim_payload_size + 15) & ~15ull);
        u64 entry = place_image(twrp_src, shim_payload2_size);
        if (entry)
            return entry;               // shim-start.S enters it with x0 = DTB
        rect(40, 300, FB_W - 80, 40, RED);
        kick();
    }
    rect(40, 300, FB_W - 80, 40, 0xff000000u);
    kick();

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
    kick();
    u64 entry_pa = p->entry_va + pa_of_va_base;
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
    ((xnu_entry_t)entry_pa)(ba_pa);
    stage(5, RED);
    return 0;
}
