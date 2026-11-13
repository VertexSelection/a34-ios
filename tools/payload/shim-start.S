// Entry for the A34 test shim. LK loads the gzip-decompressed kernel blob at a
// random 64 KiB-aligned address around 0x40100000 and jumps here with x0 = DTB.
//
// Blob layout (built by tools/build-shim.py):
//   0x00000  this header + shim code/data (must stay below 0xC000)
//   0x0C000  16 KiB stack (grows down from 0x10000)
//   0x10000  payload: an arm64 Linux Image (TWRP's kernel), copied elsewhere by
//            shim_main() and entered with the same DTB.

    .section .text.start, "ax"
    .global _start
    .global shim_payload_size
_start:
    b       entry               // code0
    .long   0                   // code1
    .quad   0x80000             // text_offset
    .quad   0                   // image_size: patched by the builder
    .quad   0xa                 // flags: little endian, 4K pages, place anywhere
    .quad   0, 0, 0             // reserved
    .ascii  "ARM\x64"           // magic
    .long   0                   // reserved
shim_payload_size:
    .quad   0                   // offset 64: patched by the builder

entry:
    mov     x19, x0             // DTB from LK
    adr     x20, _start         // where LK put us
    add     x1, x20, #0x10000
    mov     sp, x1
    mov     x0, x19
    mov     x1, x20
    bl      shim_main           // returns the payload entry, or 0
    cbz     x0, hang
    mov     x21, x0
    ic      iallu
    dsb     sy
    isb
    mov     x0, x19
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    br      x21
hang:
    wfe
    b       hang
