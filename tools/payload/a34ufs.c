// Minimal UFSHCI access for the A34 shim, adapted from ios-sweet's shim/ufs.c
// (MIT, third_party/ios-sweet). Runs with the MMU and caches off, so DMA buffers
// need no cache maintenance.
//
// One job: clear the "boot-recovery" note Samsung LK writes into the misc
// partition's bootloader message on `reboot recovery`, so a crashed test boots
// Android instead of looping back into recovery.
//
// Safety: the misc block is located by its fixed LBA (0xf680 on LUN 2, from the
// PIT and LK's own GPT log) and is only written if it currently starts with
// "boot-recovery"; the write changes nothing but the 32-byte command field, and
// is read back afterwards.

#include <stdint.h>
#include "a34ufs.h"

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;

#define UFS_BASE        0x11270000UL   // ufshci@11270000, mediatek,mt8183-ufshci
#define REG_CAP         0x00
#define REG_HCS         0x30
#define REG_HCE         0x34
#define REG_UTRLBA      0x50
#define REG_UTRLBAU     0x54
#define REG_UTRLDBR     0x58
#define REG_UTRLCLR     0x5c
#define REG_UTRLRSR     0x60
#define HCS_DP          (1u << 0)
#define HCS_UTRLRDY     (1u << 1)

#define BLOCK           4096u
#define USER_LUN        2              // all Android partitions live on LUN 2
#define DEVICE_WLUN     0xd0           // UFS Device well-known logical unit
#define MISC_LBA        0xf680u        // misc: PIT block 63104, 128 blocks

static inline u32 ufs_rd(u32 off) { return *(volatile u32 *)(UFS_BASE + off); }
static inline void ufs_wr(u32 off, u32 v) { *(volatile u32 *)(UFS_BASE + off) = v; }

struct utrd {
    u32 dw0, dw1, dw2, dw3;
    u32 ucdba, ucdbau;
    u32 resp;
    u32 prdt;
};

struct ucd {
    u8 cmd[512];
    u8 resp[512];
    struct { u32 addr_lo, addr_hi, rsvd, dbc; } prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static u8 blk[BLOCK] __attribute__((aligned(4096)));
static int ready;
struct ufs_diag ufs_diag;

static u64 ticks(void)
{
    u64 t;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
    return t;
}

static void zero(void *p, unsigned n)
{
    volatile u8 *b = p;
    for (unsigned i = 0; i < n; i++)
        b[i] = 0;
}

// One SCSI command in slot 0. dir: 0 none, 1 write (host->device), 2 read.
static u32 scsi(u8 lun, const u8 *cdb, unsigned cdb_len, void *buf, u32 len, int dir)
{
    static u8 tag;
    zero(&utrl[0], sizeof(utrl[0]));
    zero(&ucd, sizeof(ucd));

    ucd.cmd[0] = 0x01;                                  // COMMAND UPIU
    ucd.cmd[1] = dir == 2 ? 0x40 : dir == 1 ? 0x20 : 0; // R / W flag
    ucd.cmd[2] = lun;
    ucd.cmd[3] = ++tag;
    ucd.cmd[12] = len >> 24;
    ucd.cmd[13] = len >> 16;
    ucd.cmd[14] = len >> 8;
    ucd.cmd[15] = len;
    for (unsigned i = 0; i < cdb_len; i++)
        ucd.cmd[16 + i] = cdb[i];

    u64 ucd_pa = (u64)&ucd;
    utrl[0].dw0 = (1u << 28) | ((u32)(dir == 2 ? 2 : dir == 1 ? 1 : 0) << 25) | (1u << 24);
    utrl[0].dw2 = 0x0f;
    utrl[0].ucdba = (u32)ucd_pa;
    utrl[0].ucdbau = (u32)(ucd_pa >> 32);
    utrl[0].resp = (512u / 4) << 16 | (512u / 4);
    if (len) {
        u64 buf_pa = (u64)buf;
        ucd.prdt[0].addr_lo = (u32)buf_pa;
        ucd.prdt[0].addr_hi = (u32)(buf_pa >> 32);
        ucd.prdt[0].dbc = len - 1;
        utrl[0].prdt = (1024u / 4) << 16 | 1;
    }
    __asm__ volatile("dsb sy" ::: "memory");

    ufs_wr(REG_UTRLDBR, 1);
    u64 start = ticks();
    while (ufs_rd(REG_UTRLDBR) & 1) {
        if (ticks() - start > 13000000) {               // 1 s at 13 MHz
            ufs_wr(REG_UTRLCLR, ~1u);
            return UFS_E_TIMEOUT;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (utrl[0].dw2 & 0xff)
        return UFS_E_OCS | (utrl[0].dw2 & 0xff);
    if (ucd.resp[0] != 0x21)
        return UFS_E_RESP;
    if (ucd.resp[7]) {
        // Response UPIU data segment: 2-byte sense length, then fixed-format
        // sense data (key in byte 2, ASC/ASCQ in bytes 12/13).
        const u8 *s = &ucd.resp[34];
        ufs_diag.sense = (u32)(s[2] & 0xf) << 16 | (u32)s[12] << 8 | s[13];
        ufs_diag.resp6 = ucd.resp[6] | (u32)ucd.resp[10] << 16 | (u32)ucd.resp[11] << 8 |
                         (u32)ucd.resp[32] << 24;
        return UFS_E_STATUS | ucd.resp[7];
    }
    return 0;
}

static u32 rw_block(u32 lba, int write)
{
    u8 cdb[10] = { write ? 0x2a : 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, 0, 1, 0 };
    return scsi(USER_LUN, cdb, sizeof(cdb), blk, BLOCK, write ? 1 : 2);
}

u32 ufs_open(u32 *hcs_out)
{
    u32 hce = ufs_rd(REG_HCE), hcs = ufs_rd(REG_HCS);
    *hcs_out = hcs;
    if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY))
        return UFS_E_NOTREADY;
    if (ufs_rd(REG_UTRLDBR))
        return UFS_E_BUSY;
    // Take over the transfer request list; never ring a doorbell unless the
    // host really uses ours, or it would run whatever LK left in slot 0.
    ufs_wr(REG_UTRLRSR, 0);
    u64 list = (u64)utrl;
    ufs_wr(REG_UTRLBA, (u32)list);
    ufs_wr(REG_UTRLBAU, (u32)(list >> 32));
    ufs_wr(REG_UTRLRSR, 1);
    if (ufs_rd(REG_UTRLBA) != (u32)list || ufs_rd(REG_UTRLBAU) != (u32)(list >> 32) ||
        !(ufs_rd(REG_UTRLRSR) & 1))
        return UFS_E_LIST;
    static const u8 tur[6] = { 0 };
    // Samsung LK leaves the UFS device in a low-power state: data LUNs answer
    // NOT READY / "initializing command required" (sense 02/04/02). UFS only
    // accepts START STOP UNIT on the UFS Device well-known LUN (0xD0), with
    // POWER CONDITION = 1 (Active), as Linux's ufshcd_set_dev_pwr_mode() does.
    // It changes no data.
    static const u8 ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
    u32 rc = 0;
    for (int i = 0; i < 3; i++)                         // first command may see a unit attention
        if (!(rc = scsi(USER_LUN, tur, sizeof(tur), 0, 0, 0)))
            break;
    ufs_diag.tur1 = rc;
    ufs_diag.sense1 = ufs_diag.sense;
    if (rc) {
        for (int i = 0; i < 3; i++)                     // the W-LUN may report a unit attention first
            if (!(ufs_diag.ssu = scsi(DEVICE_WLUN, ssu, sizeof(ssu), 0, 0, 0)))
                break;
        for (int i = 0; i < 3; i++)
            if (!(rc = scsi(USER_LUN, tur, sizeof(tur), 0, 0, 0)))
                break;
        ufs_diag.tur2 = rc;
    }
    // Reads change nothing, so allow them even if TEST UNIT READY still
    // complains; writes stay gated on the content check in ufs_bcb_clear().
    ready = 1;
    return rc;
}

static int is_boot_recovery(const u8 *b)
{
    static const char want[] = "boot-recovery";
    for (int i = 0; i < 13; i++)
        if (b[i] != (u8)want[i])
            return 0;
    return b[13] == 0;
}

// Returns a UFS_BCB_* state; *err gets the failing command's code.
u32 ufs_bcb_clear(int do_write, u32 *err)
{
    *err = 0;
    if (!ready)
        return UFS_BCB_NOUFS;
    if ((*err = rw_block(MISC_LBA, 0)))
        return UFS_BCB_READFAIL;
    for (int i = 0; i < 4; i++)
        ufs_diag.head = ufs_diag.head << 8 | blk[i];
    if (!is_boot_recovery(blk))
        return blk[0] ? UFS_BCB_OTHER : UFS_BCB_EMPTY;
    if (!do_write)
        return UFS_BCB_FOUND;
    for (int i = 0; i < 32; i++)                        // command field only
        blk[i] = 0;
    if ((*err = rw_block(MISC_LBA, 1)))
        return UFS_BCB_WRITEFAIL;
    blk[0] = 0xa5;                                      // make sure the read-back is real
    if ((*err = rw_block(MISC_LBA, 0)))
        return UFS_BCB_READFAIL;
    for (int i = 0; i < 32; i++)
        if (blk[i])
            return UFS_BCB_VERIFYFAIL;
    return UFS_BCB_CLEARED;
}
