// Test shim v2 for the Galaxy A34 (MT6877, Samsung LK on One UI 7 / DYG1).
//
// 1. Paints LK's framebuffer with the corrected buffer spacing so the result can
//    be checked by eye: buffer 0 green, 1 magenta, 2 white, each with black
//    lines at the top, middle and bottom rows and a black square near the
//    top-left corner.
// 2. Records CPU and display facts into a placeholder ("a34t=xxxx...") that the
//    boot image puts on the kernel command line, so they show up in TWRP's
//    /proc/cmdline.
// 3. Copies the embedded arm64 Image (TWRP's kernel) to the next 2 MiB boundary
//    after the blob and returns its entry point; shim-start.S jumps there with
//    the original DTB. TWRP then clears the "boot-recovery" note, which ends the
//    reboot loop the first test caused.
//
// Addresses: recon/android-DYG1/live.dts and LK's own log in expdb.

#include <stdint.h>
#include "a34ufs.h"
#include "a34touch.h"

#ifndef BCB_WRITE
#define BCB_WRITE 0     // 0: only read and report the misc bootloader message
#endif

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;

#define FB_BASE       0x74180000UL
#define FB_SIZE       0x01d80000UL
#define STRIDE        4352UL                 // 1088 px * 4 bytes
#define FRAME         (STRIDE * 2368UL)      // 0x9d4000: LK aligns both sides to 32
#define WIDTH         1080
#define HEIGHT        2340
#define DSI0          0x14013000UL
#define MUTEX         0x14001000UL
#define OVL0          0x14005000UL
#define OVL0_2L       0x14006000UL
#define OVL1_2L       0x1401f000UL
#define RDMA0         0x14007000UL
#define PAYLOAD_OFF   0x10000UL
#define TWO_MB        0x200000UL
#define RAM_LIMIT     0x46c00000UL           // secdbg-kernel-log starts here
#define PLACEHOLDER   'x'

extern u8 _start[];
extern const u64 shim_payload_size;

static inline u32 rd32(u64 a) { return *(volatile u32 *)a; }
static inline void wr32(u64 a, u32 v) { *(volatile u32 *)a = v; }

#define SYSREG(name) ({ u64 _v; __asm__ volatile("mrs %0, " #name : "=r"(_v)); _v; })

static void delay_ms(u64 ms)
{
    u64 f = SYSREG(cntfrq_el0);
    if (!f)
        f = 13000000;
    __asm__ volatile("isb");
    u64 end = SYSREG(cntvct_el0) + f * ms / 1000;
    for (;;) {
        __asm__ volatile("isb");
        if (SYSREG(cntvct_el0) >= end)
            break;
    }
}

static void fill(u64 addr, u64 bytes, u32 color)
{
    volatile u64 *p = (volatile u64 *)addr;
    u64 v = ((u64)color << 32) | color;
    for (u64 i = 0; i < bytes / 8; i++)
        p[i] = v;
}

static void rect(u64 buf, u64 x, u64 y, u64 w, u64 h, u32 color)
{
    for (u64 r = 0; r < h; r++) {
        volatile u32 *row = (volatile u32 *)(buf + (y + r) * STRIDE + x * 4);
        for (u64 c = 0; c < w; c++)
            row[c] = color;
    }
}

static void paint(void)
{
#ifdef TRIGGER_TEST
    static const u32 colors[3] = { 0xff00ff00, 0xff00ff00, 0xff00ff00 };   // all green
#else
    static const u32 colors[3] = { 0xff00ff00, 0xffff00ff, 0xffffffff };
#endif
    for (u64 k = 0; k < 3; k++) {
        u64 buf = FB_BASE + k * FRAME;
        fill(buf, FRAME, colors[k]);
        rect(buf, 0, 0, WIDTH, 4, 0xff000000);                    // top edge
        rect(buf, 0, HEIGHT / 2 - 2, WIDTH, 4, 0xff000000);       // middle
        rect(buf, 0, HEIGHT - 4, WIDTH, 4, 0xff000000);           // bottom edge
        rect(buf, 100, 100, 200, 200, 0xff000000);                // origin marker
    }
    fill(FB_BASE + 3 * FRAME, FB_SIZE - 3 * FRAME, 0xff808080);
    for (u64 a = FB_BASE; a < FB_BASE + FB_SIZE; a += 64)
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// ---- results written into the command-line placeholder ----

static char *out, *out_end;

static void put(char c)
{
    if (out < out_end)
        *out++ = c;
}

static void put_str(const char *s)
{
    while (*s)
        put(*s++);
}

static void put_hex(u64 v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        put("0123456789abcdef"[(v >> (i * 4)) & 0xf]);
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

// Point out/out_end at the run of placeholder characters after "a34t=".
static int find_placeholder(u64 dtb)
{
    const u8 *d = (const u8 *)dtb;
    if (be32(d) != 0xd00dfeed)
        return 0;
    u32 size = be32(d + 4);
    for (u32 i = 0; i + 6 < size; i++) {
        if (d[i] == 'a' && d[i + 1] == '3' && d[i + 2] == '4' && d[i + 3] == 't' &&
            d[i + 4] == '=' && d[i + 5] == PLACEHOLDER) {
            char *p = (char *)(d + i + 5);
            char *e = p;
            while (e < (char *)(d + size) && *e == PLACEHOLDER)
                e++;
            out = p;
            out_end = e;
            return 1;
        }
    }
    return 0;
}

// Command-mode panels only show memory changes when the engine sends a frame:
// re-enable every display mutex that has modules attached, then start DSI0
// (MTK LK's ddp_mutex_enable + DSI_Start).
static void __attribute__((unused)) display_trigger(void)
{
    if (rd32(DSI0 + 0x14) & 3)          // video mode refreshes by itself
        return;
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

// OVL register layout as in Linux's mtk_disp_ovl.c: SRC_CON 0x2c (layer enable
// bits), per layer n: CON 0x30, SRC_SIZE 0x38, PITCH 0x44, ADDR 0xf40 (+0x20 n).
static void dump_ovl(char tag, u64 base, int layers)
{
    put('-'); put(tag);
    put_hex(rd32(base + 0x0c), 2);                  // EN
    put_hex(rd32(base + 0x2c), 2);                  // SRC_CON
    for (int n = 0; n < layers; n++) {
        u64 l = base + 0x20 * n;
        put('_'); put_hex(rd32(base + 0xf40 + 0x20 * n), 8);
        put('/'); put_hex(rd32(l + 0x44) & 0xffff, 4);
        put('/'); put_hex(rd32(l + 0x38), 8);
        put('/'); put_hex(rd32(l + 0x30), 8);
    }
}

// Put a white square in the middle of whichever LK buffer an enabled OVL0
// layer scans, so the screen shows which buffer the register points at.
static void mark_scanned_buffer(void)
{
    u32 en = rd32(OVL0 + 0x2c);
    for (int n = 0; n < 4; n++) {
        if (!(en & (1u << n)))
            continue;
        u64 a = rd32(OVL0 + 0xf40 + 0x20 * n);
        if (a >= FB_BASE && a + STRIDE * HEIGHT <= FB_BASE + FB_SIZE) {
            rect(a, 390, 1020, 300, 300, 0xffffffff);
            for (u64 p = a; p < a + STRIDE * HEIGHT; p += 64)
                __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
            __asm__ volatile("dsb sy" ::: "memory");
            return;
        }
    }
}

u64 shim_main(u64 dtb, u64 load)
{
    // UFS first: clear the recovery note before anything that could hang, so a
    // forced restart from here on boots Android.
    u32 hcs = 0, err = 0;
    u32 uo = ufs_open(&hcs);
    u32 bcb = ufs_bcb_clear(BCB_WRITE, &err);

    paint();
    int have_out = find_placeholder(dtb);

#ifdef DSI_PROBE
    // v8d: the panel link as LK leaves it. DSI0 registers, then 200 ms of
    // sampling DSI_INTSTA bit 31 ("busy"): busy fraction, number of busy->idle
    // edges, longest/shortest idle run and longest busy run (13 MHz ticks);
    // then one frame trigger: ticks waited for idle before it, and ticks until
    // the link is idle again after it.
    if (have_out) {
        static const uint16_t regs[] = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
                                    0x104, 0x108, 0x110, 0x114, 0x118, 0x11c,
                                    0x160, 0x164, 0x168, 0x16c, 0x170, 0x174 };
        put_str("v8d-R");
        for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
            put_hex(rd32(DSI0 + regs[i]), 8);
            put(i % 7 == 6 ? '/' : '.');
        }
        u64 f = SYSREG(cntfrq_el0), t, end, run0;
        u64 samples = 0, busy = 0, edges = 0, idle_max = 0, idle_min = ~0ull, busy_max = 0;
        __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
        end = t + f / 5;
        run0 = t;
        u32 prev = rd32(DSI0 + 0x0c) >> 31;
        for (;;) {
            u32 b = rd32(DSI0 + 0x0c) >> 31;
            __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
            samples++;
            busy += b;
            if (b != prev) {
                u64 run = t - run0;
                if (prev) {
                    edges++;
                    if (run > busy_max) busy_max = run;
                } else {
                    if (run > idle_max) idle_max = run;
                    if (run < idle_min) idle_min = run;
                }
                prev = b;
                run0 = t;
            }
            if (t >= end)
                break;
        }
        put_str("-S"); put_hex(samples, 7); put('_'); put_hex(busy, 7); put('_');
        put_hex(edges, 5); put('_'); put_hex(idle_max, 6); put('_');
        put_hex(idle_min == ~0ull ? 0 : idle_min, 6); put('_'); put_hex(busy_max, 7);
        put('_'); put_hex(prev, 1);
        // one trigger
        u64 t0, t1, t2, start_wait;
        __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t0));
        start_wait = t0;
        end = t0 + f / 2;
        while ((rd32(DSI0 + 0x0c) >> 31) && t0 < end)
            __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t0));
        __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t1));
        display_trigger();
        u32 after = rd32(DSI0 + 0x0c);
        end = t1 + f;
        do {
            __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t2));
        } while ((rd32(DSI0 + 0x0c) >> 31) && t2 < end);
        // -T <ticks waited for idle before the trigger> _ <INTSTA right after
        //    the trigger> _ <ticks until idle again> _ <INTSTA at the end>
        put_str("-T"); put_hex(t1 - start_wait, 7); put('_');
        put_hex(after, 8); put('_'); put_hex(t2 - t1, 7);
        put('_'); put_hex(rd32(DSI0 + 0x0c), 8);
    }
    if (0) {
#elif defined(CPU_PROBE)
    // v7c: CPU clock of the boot core as LK leaves it, measured with a chain
    // of dependent adds (1 cycle each on Cortex-A55) against the 13 MHz
    // counter, and the first words of the CPU DVFS shared SRAM ("dvfsp",
    // 0x0011bc00) that MediaTek's cpufreq driver talks to the MCUPM through.
    if (have_out) {
        u64 best = 0;
        for (int run = 0; run < 3; run++) {
            u64 iters = 1000000, t0, t1;
            __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t0));
            __asm__ volatile(
                "mov x9, %0\n"
                "1:\n"
                ".rept 32\n add x10, x10, #1\n .endr\n"
                "subs x9, x9, #1\n"
                "b.ne 1b\n"
                :: "r"(iters) : "x9", "x10", "cc");
            __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t1));
            u64 f = SYSREG(cntfrq_el0);
            u64 mhz = (32 * iters) * (f ? f : 13000000) / ((t1 - t0) ? (t1 - t0) : 1) / 1000000;
            if (mhz > best)
                best = mhz;
        }
        put_str("v7c-MHz"); put_hex(best, 4);
        put_str("-R");
        for (int i = 0; i < 40; i++) {
            put_hex(rd32(0x0011bc00UL + 4 * i), 8);
            put(i % 8 == 7 ? '/' : '.');
        }
    }
    if (0) {
#elif defined(TOUCH_PROBE)
    // v6t: touch bring-up facts instead of the display dump (see a34touch.c).
    if (have_out) {
        static struct a34touch t;
        a34touch_probe(&t);
        put_str("v6t-s"); put_hex(t.stage, 1);
        put_str("-P"); put_hex(t.pw_sta, 8); put('_'); put_hex(t.pm_err, 2);
        put('_'); put_hex(t.vio_en, 4); put_hex(t.vio_mon, 4); put_hex(t.vio_vosel, 4);
        put_hex(t.top, 4); put('_'); put_hex(t.vio_en2, 4); put_hex(t.vio_mon2, 4);
        put_str("-C"); put_hex(t.impw, 8); put('_'); put_hex(t.ifrao0, 8); put('_');
        put_hex(t.ifrao1, 8); put('_'); put_hex(t.cfg10, 8);
        put_str("-c"); put_hex(t.impw2, 8); put('_'); put_hex(t.ifrao02, 8); put('_');
        put_hex(t.ifrao12, 8); put('_'); put_hex(t.cfg10_2, 8);
        put_str("-L"); put_hex(t.mainpll, 8); put('_'); put_hex(t.univpll, 8);
        put_str("-G"); put_hex(t.mode8, 8); put('_'); put_hex(t.mode8_2, 8); put('_');
        put_hex(t.mode1, 8); put('_'); put_hex(t.dir0, 8); put('_');
        put_hex(t.din0_before, 8); put('_'); put_hex(t.din0_after, 8);
        put_str("-I"); put_hex(t.i2c_ctrl, 4); put('_'); put_hex(t.i2c_timing, 4);
        put('_'); put_hex(t.st_ver, 8); put('_'); put_hex(t.st_info, 8);
        put('_'); put_hex(t.st_esd, 8); put('_'); put_hex(t.st_hdr, 8);
        put_str("-V"); for (unsigned i = 0; i < sizeof(t.ver); i++) put_hex(t.ver[i], 2);
        put_str("-N"); put_hex(t.info_len[0], 2); put_hex(t.info_len[1], 2);
        put_str("-E"); put_hex(t.esd, 2);
        put_str("-H"); for (unsigned i = 0; i < sizeof(t.hdr); i++) put_hex(t.hdr[i], 2);
    }
    if (0) {
#else
    if (have_out) {
#endif
        put_str("v5");
        put_str("-E"); put_hex(SYSREG(CurrentEL) >> 2, 1);
        put_str("-S"); put_hex(SYSREG(sctlr_el1), 8);
        put_str("-L"); put_hex(load, 8);
        put_str("-D"); put_hex(dtb, 8);
        put_str("-F"); put_hex(SYSREG(cntfrq_el0), 8);
        put_str("-M"); put_hex(rd32(DSI0 + 0x14), 8);
        put_str("-X"); put_hex(rd32(MUTEX + 0x30), 8);   // mutex 0 modules
        // Mutex 0 lists bits 0-2 (OVL0, OVL0_2L, RDMA0 on this SoC family), so
        // these blocks are clocked and safe to read.
        dump_ovl('O', OVL0, 4);
        dump_ovl('T', OVL0_2L, 2);
        put_str("-R");
        put_hex(rd32(RDMA0 + 0x10), 8); put('_');     // GLOBAL_CON
        put_hex(rd32(RDMA0 + 0x14), 8); put('_');     // SIZE_CON_0
        put_hex(rd32(RDMA0 + 0x2c), 8); put('_');     // MEM_SRC_PITCH
        put_hex(rd32(RDMA0 + 0xf00), 8);              // MEM_START_ADDR
    }
    mark_scanned_buffer();

    if (have_out) {
        put_str("-U"); put_hex(uo, 3); put('_'); put_hex(hcs, 2);
        put_str("-B"); put_hex(BCB_WRITE, 1); put_hex(bcb, 1); put('_'); put_hex(err, 3);
#if !defined(TOUCH_PROBE) && !defined(CPU_PROBE) && !defined(DSI_PROBE)
        // OVL1_2L last: mutex 0 bit 16 may be it; if reading it hangs, the
        // results above are lost but the recovery note is already cleared.
        dump_ovl('Q', OVL1_2L, 2);
#endif
    }

    // Copy the payload Image to the next 2 MiB boundary after the blob.
    u64 src = load + PAYLOAD_OFF;
    u64 len = shim_payload_size;
    u64 kimage_size = *(volatile u64 *)(src + 16);
    if (kimage_size < len)
        kimage_size = len;
    u64 dest = (src + len + TWO_MB - 1) & ~(TWO_MB - 1);
    u64 text_offset = *(volatile u64 *)(src + 8);

    if (len == 0 || *(volatile u32 *)(src + 56) != 0x644d5241 ||
        dest + text_offset + kimage_size > RAM_LIMIT) {
        if (have_out) put_str("-FAIL");
        fill(FB_BASE, 3 * FRAME, 0xffff0000);       // every buffer red
        return 0;
    }

    volatile u64 *s = (volatile u64 *)src;
    volatile u64 *d = (volatile u64 *)(dest + text_offset);
    for (u64 i = 0; i < (len + 7) / 8; i++)
        d[i] = s[i];
    __asm__ volatile("dsb sy" ::: "memory");

    if (have_out) {
        put_str("-K"); put_hex(dest + text_offset, 8);
        put_str("-ok");
        while (out < out_end)
            put('.');
    }

#ifdef TRIGGER_TEST
    // Refresh test: the buffers were painted green at the start without any
    // trigger. Wait, then ask the display engine for a frame and wait again.
    u32 dsta0 = rd32(DSI0 + 0x04), cnt0 = rd32(DSI0 + 0x168);
    delay_ms(15000);
    u32 dsta1 = rd32(DSI0 + 0x04), cnt1 = rd32(DSI0 + 0x168);
    display_trigger();
    delay_ms(100);
    u32 dsta2 = rd32(DSI0 + 0x04), cnt2 = rd32(DSI0 + 0x168);
    delay_ms(15000);
    if (have_out) {
        // overwrite the trailing dots: DSI status and debug counter before/after
        while (out[-1] == '.')
            out--;
        put_str("-G"); put_hex(dsta0, 8); put('_'); put_hex(cnt0, 8);
        put('_'); put_hex(dsta1, 8); put('_'); put_hex(cnt1, 8);
        put('_'); put_hex(dsta2, 8); put('_'); put_hex(cnt2, 8);
    }
#else
    delay_ms(5000);
#endif
    return dest + text_offset;
}
