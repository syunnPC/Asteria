// SPDX-License-Identifier: Apache-2.0

// The normal-context leaves through which the BSP's idle loops take device interrupts: the idle
// runs with PSR.i clear, so a device vector pends in the local SAPIC's IRR until the idle reads it
// out. Each leaf touches only scratch registers.

.text
.align 16
// UInt64 AsteriaIa64ReadPendingDeviceVectors: the pending vectors
// 0x40-0x4F (cr.irr1 bits 0-15) in bits 0-15.
.global AsteriaIa64ReadPendingDeviceVectors
.hidden AsteriaIa64ReadPendingDeviceVectors
.type AsteriaIa64ReadPendingDeviceVectors, @function
.proc AsteriaIa64ReadPendingDeviceVectors
AsteriaIa64ReadPendingDeviceVectors:
    mov r8 = cr.irr1
    ;;
    extr.u r8 = r8, 0, 16
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64ReadPendingDeviceVectors
.size AsteriaIa64ReadPendingDeviceVectors, . - AsteriaIa64ReadPendingDeviceVectors

.align 16
// UInt64 AsteriaIa64AcceptPendingInterrupt: reads cr.ivr, which puts the
// highest pending vector in service and returns it (15 when none).
.global AsteriaIa64AcceptPendingInterrupt
.hidden AsteriaIa64AcceptPendingInterrupt
.type AsteriaIa64AcceptPendingInterrupt, @function
.proc AsteriaIa64AcceptPendingInterrupt
AsteriaIa64AcceptPendingInterrupt:
    mov r8 = cr.ivr
    ;;
    srlz.d
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64AcceptPendingInterrupt
.size AsteriaIa64AcceptPendingInterrupt, . - AsteriaIa64AcceptPendingInterrupt

.align 16
// Void AsteriaIa64EndAcceptedInterrupt: writes cr.eoi for the vector in service.
.global AsteriaIa64EndAcceptedInterrupt
.hidden AsteriaIa64EndAcceptedInterrupt
.type AsteriaIa64EndAcceptedInterrupt, @function
.proc AsteriaIa64EndAcceptedInterrupt
AsteriaIa64EndAcceptedInterrupt:
    mov cr.eoi = r0
    ;;
    srlz.d
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64EndAcceptedInterrupt
.size AsteriaIa64EndAcceptedInterrupt, . - AsteriaIa64EndAcceptedInterrupt
