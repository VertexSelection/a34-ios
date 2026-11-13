// Bare-metal touch probe for the Galaxy A34 (MT6877): runs right after LK.
//
// Goodix GT9895 (Berlin-D) on I2C0 (0x11e00000), address 0x5d, AVDD from the
// MT6359P LDO vio28 (3.0 V). Steps, each recorded in struct a34touch:
//   1. read the PMIC (pwrap), clock gates, I2C mux and pin state as LK left them
//   2. switch on vio28, the I2C0/APDMA clocks and the SCL0/SDA0 pin mode
//   3. initialise the I2C controller (FIFO mode, polled, 100 kHz) and read the
//      chip's version block, IC-info length, ESD byte and touch-event header
//
// Register facts are from Samsung's A34 kernel (6.6, kernel_device_modules):
// drivers/i2c/busses/i2c-mt65xx.c (mt6877_compat, mt_i2c_regs_v2),
// clk-mt6877*.c, pinctrl-mt6877, mt6359p registers.h, and the sec_input
// Goodix Berlin driver (goodix_brl_i2c.c / goodix_brl_hw.c).

#include <stdint.h>
#include "a34touch.h"

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

static inline u32 rd32(u64 a) { return *(volatile u32 *)a; }
static inline void wr32(u64 a, u32 v) { *(volatile u32 *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }
static inline u16 rd16(u64 a) { return *(volatile u16 *)a; }
static inline void wr16(u64 a, u16 v) { *(volatile u16 *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }
// The data port is byte-wide: the vendor driver uses writeb/readb on it.
static inline u8 rd8(u64 a) { return *(volatile u8 *)a; }
static inline void wr8(u64 a, u8 v) { *(volatile u8 *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }

#define SYSREG(name) ({ u64 _v; __asm__ volatile("mrs %0, " #name : "=r"(_v)); _v; })

static u64 now_ticks(void) { __asm__ volatile("isb"); return SYSREG(cntvct_el0); }
static u64 ticks_per_ms(void) { u64 f = SYSREG(cntfrq_el0); return (f ? f : 13000000) / 1000; }

static void udelay(u64 us)
{
    u64 end = now_ticks() + ticks_per_ms() * us / 1000 + 1;
    while (now_ticks() < end)
        ;
}

// ---- PMIC wrapper (PMIF v3 software interface) ----
#define PWRAP          0x10026000UL
#define PW_ACC         0x880
#define PW_WDATA       0x884
#define PW_RDATA       0x894
#define PW_VLDCLR      0x8a4
#define PW_STA         0x8a8
#define PW_FSM(v)      (((v) >> 1) & 7u)
#define PW_IDLE        0u
#define PW_WFVLDCLR    6u
#define PW_INIT        (1u << 15)

static int pw_wait(u32 fsm)
{
    for (int i = 0; i < 100000; i++)
        if (PW_FSM(rd32(PWRAP + PW_STA)) == fsm)
            return 0;
    return -1;
}

static int pmic_read(u16 reg, u16 *val)
{
    if (!(rd32(PWRAP + PW_STA) & PW_INIT))
        return -2;
    if (pw_wait(PW_IDLE))
        return -1;
    wr32(PWRAP + PW_ACC, reg);
    if (pw_wait(PW_WFVLDCLR))
        return -3;
    *val = (u16)rd32(PWRAP + PW_RDATA);
    wr32(PWRAP + PW_VLDCLR, 1);
    return 0;
}

static int pmic_write(u16 reg, u16 val)
{
    if (!(rd32(PWRAP + PW_STA) & PW_INIT))
        return -2;
    if (pw_wait(PW_IDLE))
        return -1;
    wr32(PWRAP + PW_WDATA, val);
    wr32(PWRAP + PW_ACC, (1u << 29) | reg);
    return pw_wait(PW_IDLE) ? -3 : 0;
}

#define MT6359_TOPSTATUS   0x002a
#define VIO28_CON0         0x1d64      // RG_LDO_VIO28_EN bit 0
#define VIO28_MON          0x1d68      // DA_VIO28_B_EN bit 0
#define VIO28_ANA_CON0     0x1faa      // VOSEL [11:8], 0xb = 3.0 V

// ---- clocks ----
#define IMPW_STA       0x11e01e00UL    // CLK_IMPW_AP_CLOCK_I2C0, bit 0 (1 = gated)
#define IMPW_CLR       0x11e01e04UL
#define IFRAO_STA0     0x10001090UL    // ifrao_i2c_pseudo bit 11
#define IFRAO_CLR0     0x10001084UL
#define IFRAO_STA1     0x100010acUL    // ifrao_apdma bit 18
#define IFRAO_CLR1     0x100010a8UL
#define TOP_CFG10      0x100000b0UL    // i2c_sel: [1:0] parent, bit 7 gate
#define TOP_CFG10_CLR  0x100000b8UL
#define TOP_UPDATE1    0x10000008UL    // bit 9 updates i2c_sel
#define MAINPLL_CON1   0x1000c354UL
#define UNIVPLL_CON1   0x1000c30cUL

// ---- pins ----
#define GPIO_MODE8     0x10005380UL    // GPIO64 [3:0], GPIO65 [7:4]
#define GPIO_MODE8_MOD 0x1000538cUL    // race-free: (0x8 | mode) << shift
#define GPIO_MODE1     0x10005310UL    // GPIO14 [27:24]
#define GPIO_DIR0      0x10005000UL
#define GPIO_DIN0      0x10005200UL

// ---- I2C0 (mt_i2c_regs_v2) ----
#define I2C0           0x11e00000UL
#define APDMA_I2C0     0x10220200UL
#define I_DATA         0x00
#define I_INTR_MASK    0x08
#define I_INTR_STAT    0x0c
#define I_CONTROL      0x10
#define I_TRANSFER_LEN 0x14
#define I_TRANSAC_LEN  0x18
#define I_DELAY_LEN    0x1c
#define I_TIMING       0x20
#define I_START        0x24
#define I_EXT_CONF     0x28
#define I_LTIMING      0x2c
#define I_HS           0x30
#define I_IO_CONFIG    0x34
#define I_FIFO_ADDR_CLR 0x38
#define I_SDA_TIMING   0x3c
#define I_LEN_AUX      0x44
#define I_CLOCK_DIV    0x48
#define I_SOFTRESET    0x50
#define I_SCL_MIS_COMP 0x90
#define I_SLAVE_ADDR1  0x94
#define I_FIFO_STAT    0xf4

#define ST_COMP        (1u << 0)
#define ST_ACKERR      (1u << 1)
#define ST_HS_NACKERR  (1u << 2)
#define ST_ARB_LOST    (1u << 3)
#define ST_ERRORS      (ST_ACKERR | ST_HS_NACKERR | ST_ARB_LOST)

#define GT_ADDR7       0x5d

// 100 kHz settings from i2c-mt65xx.c's mtk_i2c_set_speed for each i2c_sel
// parent: 26 MHz, mainpll/32 (68.25 MHz), univpll/20 (124.8 MHz).
static const u16 tim_timing[3]  = { 0x228, 0x236, 0x428 };
static const u16 tim_ltiming[3] = { 0x0ac, 0x0ba, 0x12a };
static const u16 tim_ext[3]     = { 0x8501, 0xae01, 0xd201 };
static const u16 tim_div[3]     = { 0x000, 0x101, 0x202 };
static const u16 tim_sda[3]     = { 0x100d, 0x1011, 0x100c };

static void i2c_init(u32 parent)
{
    if (parent > 2)
        parent = 0;
    wr16(I2C0 + I_START, 0);
    wr16(I2C0 + I_INTR_STAT, rd16(I2C0 + I_INTR_STAT));
    wr32(APDMA_I2C0 + 0x0c, 1);
    udelay(2);
    wr32(APDMA_I2C0 + 0x0c, 0);
    udelay(2);
    wr32(APDMA_I2C0 + 0x0c, 0x6);
    wr16(I2C0 + I_SOFTRESET, 0x21);
    udelay(2);
    wr32(APDMA_I2C0 + 0x0c, 0);
    wr16(I2C0 + I_SOFTRESET, 0);
    wr32(APDMA_I2C0 + 0x18, 0x4);
    wr16(I2C0 + I_IO_CONFIG, 0x3);
    wr16(I2C0 + I_TIMING, tim_timing[parent]);
    wr16(I2C0 + I_HS, 0);
    wr16(I2C0 + I_LTIMING, tim_ltiming[parent]);
    wr16(I2C0 + I_EXT_CONF, tim_ext[parent]);
    wr16(I2C0 + I_CLOCK_DIV, tim_div[parent]);
    wr16(I2C0 + I_SCL_MIS_COMP, 0);
    wr16(I2C0 + I_SDA_TIMING, tim_sda[parent]);
    wr16(I2C0 + I_CONTROL, 0x32c);
    wr16(I2C0 + I_DELAY_LEN, 2);
}

// One FIFO transaction: write wlen (<= 8) bytes, then, if rlen, a repeated
// start and rlen (<= 8) bytes read. Returns the final INTR_STAT, with bit 31
// set on timeout.
static u32 i2c_xfer(const u8 *w, u32 wlen, u8 *r, u32 rlen, u32 parent)
{
    wr16(I2C0 + I_CONTROL, rlen ? 0x3a : 0x28);
    wr16(I2C0 + I_SLAVE_ADDR1, GT_ADDR7 << 1);
    wr16(I2C0 + I_INTR_STAT, 0x20f);
    wr16(I2C0 + I_FIFO_ADDR_CLR, 1);
    wr16(I2C0 + I_INTR_MASK, 0);
    wr16(I2C0 + I_TRANSFER_LEN, wlen);
    wr16(I2C0 + I_LEN_AUX, rlen);
    wr16(I2C0 + I_TRANSAC_LEN, rlen ? 2 : 1);
    for (u32 i = 0; i < wlen; i++)
        wr8(I2C0 + I_DATA, w[i]);
    wr16(I2C0 + I_START, 1);
    u64 end = now_ticks() + ticks_per_ms() * 20;
    u32 st;
    for (;;) {
        st = rd16(I2C0 + I_INTR_STAT);
        if (st & (ST_COMP | ST_ERRORS))
            break;
        if (now_ticks() > end) {
            st |= 1u << 31;
            break;
        }
    }
    wr16(I2C0 + I_INTR_STAT, st & 0xffff);
    if ((st & (ST_ERRORS | (1u << 31))) || !(st & ST_COMP)) {
        i2c_init(parent);
        return st | (1u << 30);
    }
    for (u32 i = 0; i < rlen; i++)
        r[i] = rd8(I2C0 + I_DATA);
    return st;
}

// Berlin register read: 4-byte big-endian address, chunked to the 8-byte FIFO.
static u32 gt_read(u32 reg, u8 *buf, u32 len, u32 parent)
{
    while (len) {
        u32 n = len > 8 ? 8 : len;
        u8 a[4] = { (u8)(reg >> 24), (u8)(reg >> 16), (u8)(reg >> 8), (u8)reg };
        u32 st = i2c_xfer(a, 4, buf, n, parent);
        if (st & (3u << 30))
            return st;
        reg += n;
        buf += n;
        len -= n;
    }
    return 1;
}

void a34touch_probe(struct a34touch *t)
{
    u16 v;

    // 1. state as LK left it
    t->pw_sta = rd32(PWRAP + PW_STA);
    t->pm_err = 0;
    v = 0xffff;
    if (pmic_read(VIO28_CON0, &v))
        t->pm_err |= 1;
    t->vio_en = v;
    v = 0xffff;
    if (pmic_read(VIO28_MON, &v))
        t->pm_err |= 2;
    t->vio_mon = v;
    v = 0xffff;
    if (pmic_read(VIO28_ANA_CON0, &v))
        t->pm_err |= 4;
    t->vio_vosel = v;
    v = 0xffff;
    if (pmic_read(MT6359_TOPSTATUS, &v))
        t->pm_err |= 8;
    t->top = v;
    t->impw = rd32(IMPW_STA);
    t->ifrao0 = rd32(IFRAO_STA0);
    t->ifrao1 = rd32(IFRAO_STA1);
    t->cfg10 = rd32(TOP_CFG10);
    t->mainpll = rd32(MAINPLL_CON1);
    t->univpll = rd32(UNIVPLL_CON1);
    t->mode8 = rd32(GPIO_MODE8);
    t->mode1 = rd32(GPIO_MODE1);
    t->dir0 = rd32(GPIO_DIR0);
    t->din0_before = rd32(GPIO_DIN0);
    t->stage = 1;
    if (t->pm_err & 3)
        return;

    // 2. power, clocks, pins
    if (!(t->vio_en & 1)) {
        if (((t->vio_vosel >> 8) & 0xf) != 0xb &&
            pmic_write(VIO28_ANA_CON0, (u16)((t->vio_vosel & ~0x0f00) | 0x0b00)))
            t->pm_err |= 0x10;
        if (pmic_write(VIO28_CON0, (u16)(t->vio_en | 1)))
            t->pm_err |= 0x20;
        udelay(5000);
    }
    v = 0xffff;
    if (pmic_read(VIO28_CON0, &v))
        t->pm_err |= 0x40;
    t->vio_en2 = v;
    v = 0xffff;
    if (pmic_read(VIO28_MON, &v))
        t->pm_err |= 0x80;
    t->vio_mon2 = v;
    if (t->cfg10 & 0x80) {
        wr32(TOP_CFG10_CLR, 0x80);
        wr32(TOP_UPDATE1, 1u << 9);
    }
    wr32(IFRAO_CLR1, 1u << 18);         // APDMA (the driver enables dma first)
    wr32(IFRAO_CLR0, 1u << 11);         // i2c pseudo
    wr32(IMPW_CLR, 1u << 0);            // I2C0
    udelay(10);
    t->impw2 = rd32(IMPW_STA);
    t->ifrao02 = rd32(IFRAO_STA0);
    t->ifrao12 = rd32(IFRAO_STA1);
    t->cfg10_2 = rd32(TOP_CFG10);
    if ((t->mode8 & 0xff) != 0x11)
        wr32(GPIO_MODE8_MOD, 0x99);
    t->mode8_2 = rd32(GPIO_MODE8);
    t->stage = 2;
    if ((t->impw2 & 1) || (t->ifrao12 & (1u << 18)))
        return;

    // 3. talk to the chip (firmware needs ~200 ms after power-on)
    udelay(250000);
    u32 parent = t->cfg10_2 & 3;
    i2c_init(parent);
    t->i2c_ctrl = rd16(I2C0 + I_CONTROL);
    t->i2c_timing = rd16(I2C0 + I_TIMING);
    t->stage = 3;
    t->st_ver = gt_read(0x10014, t->ver, sizeof(t->ver), parent);
    t->st_info = gt_read(0x10070, t->info_len, sizeof(t->info_len), parent);
    t->st_esd = gt_read(0x10170, &t->esd, 1, parent);
    t->st_hdr = gt_read(0x10308, t->hdr, sizeof(t->hdr), parent);
    t->din0_after = rd32(GPIO_DIN0);
    t->stage = 4;
}
