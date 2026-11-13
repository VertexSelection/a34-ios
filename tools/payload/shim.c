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
    static const u32 colors[3] = { 0xff00ff00, 0xffff00ff, 0xffffffff };
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
    paint();
    int have_out = find_placeholder(dtb);

    if (have_out) {
        put_str("v4");
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

    // UFS: open the host LK left running, then look at (and maybe clear) the
    // misc bootloader message.
    u32 hcs = 0, err = 0;
    u32 uo = ufs_open(&hcs);
    u32 bcb = ufs_bcb_clear(BCB_WRITE, &err);
    if (have_out) {
        put_str("-U"); put_hex(uo, 3); put('_'); put_hex(hcs, 2);
        put_str("-B"); put_hex(BCB_WRITE, 1); put_hex(bcb, 1); put('_'); put_hex(err, 3);
        put('_'); put_hex(ufs_diag.head, 8);
        put_str("-V"); put_hex(ufs_diag.tur1, 3); put('_'); put_hex(ufs_diag.sense1, 6);
        put('_'); put_hex(ufs_diag.ssu, 3); put('_'); put_hex(ufs_diag.tur2, 3);
        put('_'); put_hex(ufs_diag.sense, 6);
        put('_'); put_hex(ufs_diag.resp6, 8);
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

    delay_ms(5000);
    return dest + text_offset;
}
