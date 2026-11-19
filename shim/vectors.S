.section .text.el1vectors, "ax"
.balign 2048

.global el1_vectors
.type el1_vectors, %function

/*
 * Each AArch64 vector slot is 128 bytes. Save x0-x30 before using
 * registers to pass the exception frame and vector number to C.
 * The 256-byte frame keeps SP 16-byte aligned and matches
 * el1_exception_frame_t in exception.h.
 */
.macro EXCEPTION_SLOT number
    .balign 128
.Lvector_start_\@:
    msr daifset, #0xf
    sub sp, sp, #256

    stp x0,  x1,  [sp, #0]
    stp x2,  x3,  [sp, #16]
    stp x4,  x5,  [sp, #32]
    stp x6,  x7,  [sp, #48]
    stp x8,  x9,  [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]

    mov x0, sp
    mov x1, #\number
    b el1_exception_dispatch_entry

    .space 128 - (. - .Lvector_start_\@)
.endm

el1_vectors:
    /* Current EL using SP_EL0: synchronous, IRQ, FIQ, SError. */
    EXCEPTION_SLOT 0
    EXCEPTION_SLOT 1
    EXCEPTION_SLOT 2
    EXCEPTION_SLOT 3

    /* Current EL using SP_EL1: synchronous, IRQ, FIQ, SError. */
    EXCEPTION_SLOT 4
    EXCEPTION_SLOT 5
    EXCEPTION_SLOT 6
    EXCEPTION_SLOT 7

    /* Lower EL using AArch64: synchronous, IRQ, FIQ, SError. */
    EXCEPTION_SLOT 8
    EXCEPTION_SLOT 9
    EXCEPTION_SLOT 10
    EXCEPTION_SLOT 11

    /* Lower EL using AArch32: synchronous, IRQ, FIQ, SError. */
    EXCEPTION_SLOT 12
    EXCEPTION_SLOT 13
    EXCEPTION_SLOT 14
    EXCEPTION_SLOT 15

.size el1_vectors, . - el1_vectors

.global el1_exception_dispatch_entry
.type el1_exception_dispatch_entry, %function
el1_exception_dispatch_entry:
    /* x0 = saved frame, x1 = vector slot number. */
    bl el1_exception_dispatch

1:
    wfe
    b 1b

.size el1_exception_dispatch_entry, . - el1_exception_dispatch_entry
