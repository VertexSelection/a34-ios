// First test payload for the Galaxy A34 (MT6877), loaded by Samsung's LK from the
// recovery partition in place of a Linux kernel.
//
// What it does, in order:
//   1. Clears the "boot into recovery" mode the kernel left in the TOPRGU
//      non-reset register, so the next start goes to Android, not back here.
//   2. Paints the bootloader's framebuffer reservation: buffer 0 green,
//      buffer 1 magenta, buffer 2 white, the rest grey. These colours look the
//      same whether the panel reads RGBA or BGRA.
//   3. Waits 3 s. If the colour shows now, the display refreshes by itself.
//   4. If DSI0 is in command mode, asks the display engine to send a frame
//      (display mutex enable + DSI start), three times.
//   5. Waits 5 s, then resets the phone (PSCI SYSTEM_RESET, then watchdog).
//
// All addresses come from the phone's own device tree (recon/android-DYG1/live.dts):
//   toprgu@10007000, reboot-mode offset 0x24 mask 0xf
//   mblock-23-framebuffer 0x74180000 size 0x1d80000
//   disp_mutex@14001000, dsi@14013000, arch timer 13 MHz, PSCI via smc.

    .equ TOPRGU,        0x10007000
    .equ WDT_SWRST,     0x14
    .equ WDT_NONRST2,   0x24
    .equ FB_BASE,       0x74180000
    .equ FB_SIZE,       0x01d80000
    .equ FRAME_BYTES,   4352 * 2340            // 1088-pixel stride, 32 bpp
    .equ REST_BYTES,    FB_SIZE - 3 * FRAME_BYTES
    .equ DSI0,          0x14013000
    .equ DSI_START,     0x00
    .equ DSI_MODE_CTRL, 0x14
    .equ MUTEX,         0x14001000
    .equ MUTEX_EN0,     0x20                   // + 0x20 * n
    .equ MUTEX_MOD0,    0x30                   // + 0x20 * n
    .equ PSCI_RESET,    0x84000009

    .text
    .global _start
_start:
    // arm64 Linux Image header, so the bootloader treats us like a kernel
    b       main                // code0
    .long   0                   // code1
    .quad   0x80000             // text_offset
    .quad   image_end - _start  // image_size
    .quad   0xa                 // flags: little endian, 4K pages, place anywhere
    .quad   0, 0, 0             // reserved
    .ascii  "ARM\x64"           // magic
    .long   0                   // reserved

main:
    bl      clear_boot_mode

    ldr     x0, =FB_BASE
    ldr     x2, =FRAME_BYTES
    ldr     x3, =0xff00ff00ff00ff00     // green
    bl      fill
    ldr     x2, =FRAME_BYTES
    ldr     x3, =0xffff00ffffff00ff     // magenta
    bl      fill
    ldr     x2, =FRAME_BYTES
    mov     x3, #-1                     // white
    bl      fill
    ldr     x2, =REST_BYTES
    ldr     x3, =0xff808080ff808080     // grey
    bl      fill
    bl      clean_fb
    dsb     sy

    mov     x0, #3000
    bl      delay_ms

    bl      trigger
    mov     x0, #100
    bl      delay_ms
    bl      trigger
    mov     x0, #100
    bl      delay_ms
    bl      trigger

    mov     x0, #5000
    bl      delay_ms

    bl      clear_boot_mode
    ldr     x0, =PSCI_RESET
    smc     #0
    // PSCI returned: fall back to a watchdog software reset
    ldr     x0, =TOPRGU
    mov     w1, #0x1209
    str     w1, [x0, #WDT_SWRST]
hang:
    wfe
    b       hang

// Clear the low nibble of WDT_NONRST2 (the boot mode that "adb reboot recovery" set).
clear_boot_mode:
    ldr     x0, =TOPRGU
    ldr     w1, [x0, #WDT_NONRST2]
    and     w1, w1, #0xfffffff0
    str     w1, [x0, #WDT_NONRST2]
    dsb     sy
    ret

// x0 = destination (advanced), x2 = byte count (multiple of 16), x3 = 64-bit pattern
fill:
    stp     x3, x3, [x0], #16
    subs    x2, x2, #16
    b.gt    fill
    ret

// Clean the framebuffer from the data cache in case the bootloader left it on.
clean_fb:
    ldr     x0, =FB_BASE
    ldr     x1, =FB_BASE + FB_SIZE
1:  dc      cvac, x0
    add     x0, x0, #64
    cmp     x0, x1
    b.lo    1b
    ret

// x0 = milliseconds. Uses the virtual counter; 13 MHz if CNTFRQ is unset.
delay_ms:
    mrs     x1, cntfrq_el0
    cbnz    x1, 1f
    ldr     x1, =13000000
1:  mul     x1, x1, x0
    mov     x2, #1000
    udiv    x1, x1, x2
    isb
    mrs     x2, cntvct_el0
    add     x2, x2, x1
2:  isb
    mrs     x3, cntvct_el0
    cmp     x3, x2
    b.lo    2b
    ret

// In command mode the panel only updates when the engine sends a frame:
// re-enable every display mutex that has modules attached, then start DSI0.
trigger:
    ldr     x0, =DSI0
    ldr     w1, [x0, #DSI_MODE_CTRL]
    tst     w1, #3
    b.ne    9f                  // video mode: memory writes show by themselves
    ldr     x4, =MUTEX
    mov     x5, #0
1:  add     x6, x4, x5, lsl #5
    ldr     w7, [x6, #MUTEX_MOD0]
    cbz     w7, 2f
    str     wzr, [x6, #MUTEX_EN0]
    mov     w8, #1
    str     w8, [x6, #MUTEX_EN0]
2:  add     x5, x5, #1
    cmp     x5, #16
    b.lo    1b
    dsb     sy
    str     wzr, [x0, #DSI_START]
    mov     w8, #1
    str     w8, [x0, #DSI_START]
    dsb     sy
9:  ret

    .ltorg
    .balign 16
image_end:
