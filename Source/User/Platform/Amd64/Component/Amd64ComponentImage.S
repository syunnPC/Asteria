// SPDX-License-Identifier: Apache-2.0

// Entry of the AMD64 component image: the root service starts its thread here with the start
// block's address in RDI and the stack it built.

    .text
    .global AsteriaAmd64ComponentImageEntry
    .hidden AsteriaAmd64ComponentImageEntry
    .type AsteriaAmd64ComponentImageEntry,@function
AsteriaAmd64ComponentImageEntry:
    xor %ebp,%ebp
    call AsteriaAmd64ComponentImageMain
1:
    pause
    jmp 1b
    .size AsteriaAmd64ComponentImageEntry,.-AsteriaAmd64ComponentImageEntry

// The first incarnation's crash, by the kind in RDI: an invalid instruction, or
// (ComponentCrashFloatingPoint) an x87 division by zero with FCW.ZM clear, which the next waiting
// instruction reports (#MF, CR0.NE set); the kernel hands either to the component's supervisor. x87
// rather than SSE: QEMU's TCG raises no #XM for an unmasked SSE exception. Returns only if neither
// trapped.
    .global AsteriaAmd64ComponentCrash
    .hidden AsteriaAmd64ComponentCrash
    .type AsteriaAmd64ComponentCrash,@function
AsteriaAmd64ComponentCrash:
    cmp $1,%rdi
    jne 1f
    sub $8,%rsp
    fnstcw (%rsp)
    andw $0xFFFB,(%rsp)
    fldcw (%rsp)
    movl $0,(%rsp)
    fld1
    fidivl (%rsp)
    fwait
    add $8,%rsp
    ret
1:
    ud2
    ret
    .size AsteriaAmd64ComponentCrash,.-AsteriaAmd64ComponentCrash

// Returns the time-stamp counter, the component's clock.
    .global AsteriaAmd64ComponentClock
    .hidden AsteriaAmd64ComponentClock
    .type AsteriaAmd64ComponentClock,@function
AsteriaAmd64ComponentClock:
    rdtsc
    shl $32,%rdx
    or %rdx,%rax
    ret
    .size AsteriaAmd64ComponentClock,.-AsteriaAmd64ComponentClock
