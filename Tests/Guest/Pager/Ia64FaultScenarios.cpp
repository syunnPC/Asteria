// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/Ia64FaultScenarios.hpp"

#include "Abi/PagerProtocol.hpp"
#include "User/Pager/PagerWords.hpp"
#include "User/Loader/ExecutableFormats.hpp"

#if defined(__ia64__)
extern "C" void AsteriaIa64FaultBreak() noexcept;
extern "C" void AsteriaIa64FaultEager() noexcept;
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultEagerSite[];
extern "C" void AsteriaIa64FaultBigEndian() noexcept;
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultBigEndianSite[];
extern "C" void AsteriaIa64FaultLoadDistance() noexcept;
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultLoadDistanceSite[];
extern "C" void AsteriaIa64FaultShortFrame() noexcept;
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultShortFrameSite[];
extern "C" void AsteriaIa64FaultLocals() noexcept;
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultLocalsSite[];
extern "C" void AsteriaIa64FaultRotating() noexcept;
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultRotatingSite[];
extern "C" void AsteriaIa64FaultEagerBreak() noexcept;
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultEagerBreakSite[];
extern "C" void AsteriaIa64FaultIsaTransition() noexcept;
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultIsaTransitionSite[];
extern "C" void AsteriaIa64FaultFrameMarker() noexcept;
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultFrameMarkerSite[];
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultFrameMarkerWrite[];
extern "C" void AsteriaIa64FaultEagerSpin() noexcept;
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultEagerSpinSite[];
extern "C" void AsteriaIa64FaultPrivileged() noexcept;
extern "C" void AsteriaIa64FaultNatRegister() noexcept;
extern "C" void AsteriaIa64FaultUnaligned() noexcept;
extern "C" void AsteriaIa64FaultDataRights() noexcept;
extern "C" void AsteriaIa64FaultInstruction() noexcept;
extern "C" void AsteriaIa64FaultSpeculation() noexcept;
extern "C" void AsteriaIa64FaultRse() noexcept;
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultBreakSite[];
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultPrivilegedSite[];
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultNatRegisterSite[];
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultUnalignedSite[];
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultDataRightsSite[];
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultLazyCode[];
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64FaultLazyCodeEnd[];
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64SpinnerImageBytes[];
extern "C" const Asteria::Foundation::UInt8 AsteriaIa64SpinnerImageEnd[];

namespace Asteria::User::Pager
{
namespace
{
// Endpoint delivery header + AIPC header + one capability attachment.
constexpr Foundation::Size TokenOffset{48U + 88U};
constexpr Foundation::Size FaultOffset{TokenOffset + 32U};
constexpr Foundation::Size DeliveryBytes{FaultOffset + Abi::PagerFaultPayloadBytes};
// The fault record: address at 0, then the instruction, syndrome, access, cause, origin and, at 32,
// the supervision tag, zero for this boot domain's threads, and the kernel-assigned domain identity.
constexpr Foundation::Size RecordInstructionOffset{8U};
constexpr Foundation::Size RecordSyndromeOffset{16U};
constexpr Foundation::Size RecordAccessOffset{24U};
constexpr Foundation::Size RecordCauseOffset{25U};
constexpr Foundation::Size RecordOriginOffset{26U};
constexpr Foundation::Size RecordTagOffset{Abi::PagerFaultTagOffset};
constexpr Foundation::Size RecordIdentityOffset{Abi::PagerFaultIdentityOffset};
constexpr Foundation::UInt8 ExecuteAccess{3U};
constexpr Foundation::UInt8 MissingMappingCause{1U};
constexpr Foundation::UInt8 ProtectionViolationCause{2U};
constexpr Foundation::UInt8 ExceptionCause{4U};
constexpr Foundation::UInt8 UserOrigin{1U};
// The IA-64 syndrome: the IVT offset in bits 48-63 over the ISR, whose code is bits 0-15 (a break's
// IIM in their place) and whose ei, the instruction slot, bits 41-42.
constexpr Foundation::UInt64 SyndromeVectorShift{48U};
constexpr Foundation::UInt64 SyndromeCodeMask{0xFFFFU};
constexpr Foundation::UInt64 SyndromeSlotShift{41U};
constexpr Foundation::UInt64 SyndromeSlotMask{3U};
constexpr Foundation::UInt64 RseBit{Foundation::UInt64{1U} << 37U};
constexpr Foundation::UInt64 IncompleteBit{Foundation::UInt64{1U} << 38U};
// IVT offsets (Intel SDM Vol. 2, table 8-4) and the ISR codes the probes
// raise: a privileged operation and a NaT register consumption both have
// code{7:4} 1.
constexpr Foundation::UInt64 AlternateInstructionTlbVector{0x0C00U};
constexpr Foundation::UInt64 AlternateDataTlbVector{0x1000U};
constexpr Foundation::UInt64 BreakVector{0x2C00U};
// External Interrupt (SDM rev. 1.1 Vol. 2, 0x3000); a device vector carries its number in the low
// byte.
constexpr Foundation::UInt64 ExternalInterruptVector{0x3000U};
constexpr Foundation::UInt64 FirstDeviceVector{0x40U};
constexpr Foundation::UInt64 LastDeviceVector{0x4FU};
constexpr Foundation::UInt64 InstructionAccessRightsVector{0x5200U};
constexpr Foundation::UInt64 DataAccessRightsVector{0x5300U};
constexpr Foundation::UInt64 GeneralExceptionVector{0x5400U};
constexpr Foundation::UInt64 NatConsumptionVector{0x5600U};
constexpr Foundation::UInt64 UnalignedReferenceVector{0x5A00U};
constexpr Foundation::UInt64 PrivilegedOperationCode{0x10U};
constexpr Foundation::UInt64 NatRegisterCode{0x10U};
// General Exception ISR.code{7:4} 3 (SDM rev. 1.1 Vol. 2, General Exception
// vector): the emulator refuses the malformed PFS write itself.
constexpr Foundation::UInt64 ReservedRegisterFieldCode{0x30U};
// FaultResolve permissions (Read 1, Write 2, Execute 4, User 8).
constexpr Foundation::UInt8 ReadWriteUser{0x0BU};
constexpr Foundation::UInt8 ReadExecuteUser{0x0DU};
// The spinner image's contract segment (Ia64Spinner.S): its code bytes, the
// success break's bundle offset, the count's ITC ticks and the success
// break's immediate.
constexpr Foundation::Size SpinnerContractCodeBytesOffset{0U};
constexpr Foundation::Size SpinnerContractSuccessOffset{8U};
constexpr Foundation::Size SpinnerContractImmediateOffset{24U};
constexpr Foundation::Size SpinnerContractBytes{32U};
constexpr Foundation::UInt64 SpinnerSuccessSlot{1U};
constexpr Foundation::UInt64 BundleMask{15U};
constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::UInt64 RsePages{4U};
constexpr Foundation::UInt64 ProbeExitCode{77U};
constexpr Foundation::UInt64 MaximumWaits{2000U};
constexpr Foundation::UInt64 WaitMicroseconds{10'000U};
// One scenario at a time, on the BSP, and only the active test thread calls
// the exit/unmap bridges. The pager owns all these objects until it reaps it.
constinit ScenarioContext* g_active{nullptr};
alignas(16U) constinit Foundation::UInt64 g_operand[2U]{};
constinit bool g_rseMapped[RsePages]{};
constinit Loader::ExecutableImage g_spinnerLayout{};

void ReportMismatch(ScenarioContext& context, const Foundation::UInt64 address, const Foundation::UInt64 instruction,
                    const Foundation::UInt64 syndrome) noexcept
{
    char line[80U]{'#', ' ', 'f', 'a', 'u', 'l', 't', ' ', 'm', 'i', 's', 'm', 'a', 't', 'c', 'h', ':'};
    Foundation::Size length{17U};
    const Foundation::UInt64 words[]{address, instruction, syndrome};
    for (const Foundation::UInt64 word : words)
    {
        line[length++] = ' ';
        for (Foundation::UInt64 digit{16U}; digit != 0U; --digit)
        {
            const Foundation::UInt64 value = (word >> ((digit - 1U) * 4U)) & 15U;
            line[length++] = static_cast<char>(value < 10U ? '0' + value : 'a' + value - 10U);
        }
    }
    line[length++] = '\n';
    static_cast<void>(context.Client->DebugWrite(context.Diagnostic, AddressOf(line), length));
}

// What the pager does with a probe's faults: none may arrive (the thread
// Must exit by itself), terminate it, or map its lazy code or backing store.
enum class Resolution : Foundation::UInt8
{
    NoFault,
    Terminate,
    Execute,
    Rse,
};

struct Probe
{
    Foundation::UInt64 Entry;
    Foundation::UInt64 Operand;
    Foundation::UInt64 Vector;
    Foundation::UInt64 Address;
    Foundation::UInt64 Instruction;
    Foundation::UInt64 Code;
    Foundation::UInt64 Slot;
    Foundation::UInt8 Cause;
    bool Execute;
    Resolution Resolve;
    const char* Line;
    Foundation::Size LineBytes;
    // Any tick or device interrupt at Address, in any slot (ISR.code zero).
    bool External{false};
    // A second acceptable fault shape, resolved the same way.
    const Probe* Alternative{nullptr};
};

// The fields of a delivered record every non-RSE probe and the RSE probes
// share: vector, cause, origin, access, tag and supervision identity.
[[nodiscard]] bool MatchesRecord(const Probe& probe, const Foundation::UInt8* const fault,
                                 const Foundation::UInt64 syndrome) noexcept
{
    const Foundation::UInt64 vector = syndrome >> SyndromeVectorShift;
    const bool externalVector = (vector == ExternalInterruptVector) ||
                                (((vector & ~Foundation::UInt64{0xFFU}) == ExternalInterruptVector) &&
                                 ((vector & 0xFFU) >= FirstDeviceVector) && ((vector & 0xFFU) <= LastDeviceVector));
    return (probe.External ? externalVector : (vector == probe.Vector)) && (fault[RecordCauseOffset] == probe.Cause) &&
           (fault[RecordOriginOffset] == UserOrigin) &&
           ((fault[RecordAccessOffset] == ExecuteAccess) == probe.Execute) &&
           (LoadWord(fault + RecordTagOffset) == 0U) && (LoadWord(fault + RecordIdentityOffset) != 0U);
}

// The faulting site: addresses, ISR.code and slot (any slot for External).
[[nodiscard]] bool MatchesSite(const Probe& probe, const Foundation::UInt64 address,
                               const Foundation::UInt64 instruction, const Foundation::UInt64 syndrome) noexcept
{
    const Foundation::UInt64 slot = (syndrome >> SyndromeSlotShift) & SyndromeSlotMask;
    return (address == probe.Address) && (instruction == probe.Instruction) &&
           ((syndrome & SyndromeCodeMask) == probe.Code) && (probe.External ? (slot <= 2U) : (slot == probe.Slot));
}

template <typename T> [[nodiscard]] Foundation::UInt64 Descriptor(T entry) noexcept
{
    // P64 function pointers name the linker-produced {IP, GP} descriptor.
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(entry));
}

[[nodiscard]] bool CreateProbe(ScenarioContext& context, const Probe& probe, System::CapabilityHandle& thread,
                               Abi::SystemCallError& error) noexcept
{
    context.Calls->ClearRequest();
    Foundation::UInt8* const request = context.Calls->GetRequest();
    StoreWord(request + Abi::ObjectCreateThreadEntryOffset, probe.Entry);
    StoreWord(request + Abi::ObjectCreateThreadStackOffset, context.Launch->StackTop);
    StoreWord(request + Abi::ObjectCreateThreadArgumentOffset, probe.Operand);
    return context.Calls->Create(context.Management, Abi::ObjectTypeThread, Abi::ObjectCreateThreadRequestSizeBytes,
                                 thread, error);
}

[[nodiscard]] bool ResolveProbe(ScenarioContext& context, const Probe& probe, const System::CapabilityHandle token,
                                const Foundation::UInt64 address, const ComponentMapping& mapping,
                                Abi::SystemCallError& error) noexcept
{
    if ((probe.Resolve == Resolution::Terminate) || (probe.Resolve == Resolution::NoFault))
    {
        return Check(context.Client->FaultTerminate(token), error) && context.Calls->Close(token, error);
    }
    context.Calls->ClearRequest();
    Foundation::UInt8* const request = context.Calls->GetRequest();
    const Foundation::UInt64 page = address & ~(PageBytes - 1U);
    const Foundation::UInt64 offset = (page - context.WindowStart) / PageBytes;
    StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, context.OwnSpace);
    StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, mapping.Memory);
    StoreHandle(request + Abi::FaultResolveFrameOffset, mapping.Frame);
    StoreWord(request + Abi::FaultResolveVirtualStartOffset, page);
    StoreWord(request + Abi::FaultResolvePageCountOffset, 1U);
    StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, offset);
    StoreWord(request + Abi::FaultResolveFramePageOffset, offset);
    request[Abi::FaultResolvePermissionsOffset] =
        probe.Resolve == Resolution::Execute ? ReadExecuteUser : ReadWriteUser;
    // Publish while the RSE thread is still blocked. Once FaultResolve wakes
    // it, a timer may run its leaf before this pager closes the old token.
    if (probe.Resolve == Resolution::Rse)
    {
        g_rseMapped[offset] = true;
    }
    if (!Check(context.Client->FaultResolve(token, Abi::FaultResolveKind::Map, AddressOf(request)), error) ||
        !context.Calls->Close(token, error))
    {
        return false;
    }
    return true;
}

[[nodiscard]] bool UnmapRse(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    for (Foundation::UInt64 page{0U}; page < RsePages; ++page)
    {
        if (g_rseMapped[page])
        {
            if (!Check(context.Client->UnmapMemory(context.OwnSpace, context.WindowStart + page * PageBytes, 1U),
                       error))
            {
                return false;
            }
            g_rseMapped[page] = false;
        }
    }
    return true;
}

[[nodiscard, gnu::noinline]] bool RunBusyProbe(ScenarioContext& context, const Probe& probe,
                                               Abi::SystemCallError& error) noexcept
{
    System::CapabilityHandle threads[2U]{};
    if (!CreateProbe(context, probe, threads[0U], error) || !CreateProbe(context, probe, threads[1U], error))
    {
        return false;
    }
    Foundation::UInt64 delivered{0U};
    for (Foundation::UInt64 waits{0U}; (waits < MaximumWaits) && (delivered != 2U); ++waits)
    {
        const Abi::SystemCallResponse waited = context.Client->Wait(context.Bootstrap->WaitSet, WaitMicroseconds);
        if (waited.Error == Abi::SystemCallError::TimedOut)
        {
            continue;
        }
        if (!Check(waited, error))
        {
            return false;
        }
        const Abi::SystemCallResponse received =
            context.Client->Receive(context.Bootstrap->Endpoint, AddressOf(context.Delivery), sizeof(context.Delivery));
        const Foundation::UInt8* const fault = context.Delivery + FaultOffset;
        const Foundation::UInt64 syndrome = LoadWord(fault + RecordSyndromeOffset);
        if (!Check(received, error) || (received.Value1 != DeliveryBytes) || (LoadWord(fault) != probe.Address) ||
            (LoadWord(fault + RecordInstructionOffset) != probe.Instruction) ||
            ((syndrome >> SyndromeVectorShift) != probe.Vector) || ((syndrome & SyndromeCodeMask) != probe.Code) ||
            (((syndrome >> SyndromeSlotShift) & SyndromeSlotMask) != probe.Slot) ||
            (fault[RecordCauseOffset] != probe.Cause) || (LoadWord(fault + RecordIdentityOffset) == 0U))
        {
            return false;
        }
        if (delivered == 0U)
        {
            // Keep the first token pending while the second thread is run.
            // It must yield behind this token and retry after resolution.
            const Abi::SystemCallResponse held = context.Client->Wait(context.Bootstrap->WaitSet, 1000U);
            if (held.Error != Abi::SystemCallError::TimedOut)
            {
                error = held.Error;
                return false;
            }
        }
        const System::CapabilityHandle token{LoadWord(context.Delivery + TokenOffset),
                                             LoadWord(context.Delivery + TokenOffset + 8U)};
        if (!ResolveProbe(context, probe, token, probe.Address, ComponentMapping{}, error))
        {
            return false;
        }
        ++delivered;
    }
    if (delivered != 2U)
    {
        return false;
    }
    for (const System::CapabilityHandle thread : threads)
    {
        const Abi::SystemCallResponse destroyed = context.Client->DestroyObject(thread, Abi::ObjectTypeThread);
        if (!Check(destroyed, error) || (destroyed.Value0 != Abi::FaultTerminatedExitCode) ||
            !context.Calls->Close(thread, error))
        {
            return false;
        }
    }
    static constexpr char line[]{"# ia64 fault: busy pager yielded and retried, both delivered\n"};
    return Check(context.Client->DebugWrite(context.Diagnostic, AddressOf(line), sizeof(line) - 1U), error);
}

[[nodiscard, gnu::noinline]] bool RunProbe(ScenarioContext& context, const Probe& probe,
                                           const ComponentMapping& mapping, Abi::SystemCallError& error) noexcept
{
    System::CapabilityHandle thread{};
    if (!CreateProbe(context, probe, thread, error))
    {
        return false;
    }
    Foundation::UInt64 faults{0U};
    bool sawRse{false};
    bool sawIncomplete{false};
    for (Foundation::UInt64 waits{0U}; waits < MaximumWaits; ++waits)
    {
        const Abi::SystemCallResponse destroyed = context.Client->DestroyObject(thread, Abi::ObjectTypeThread);
        if (destroyed.Error == Abi::SystemCallError::None)
        {
            const Foundation::UInt64 exitCode =
                probe.Resolve == Resolution::Terminate ? Abi::FaultTerminatedExitCode : ProbeExitCode;
            if ((destroyed.Value0 != exitCode) || ((faults == 0U) != (probe.Resolve == Resolution::NoFault)) ||
                ((probe.Resolve == Resolution::Rse) && (!sawRse || !sawIncomplete)) ||
                !context.Calls->Close(thread, error))
            {
                return false;
            }
            return Check(context.Client->DebugWrite(context.Diagnostic, AddressOf(probe.Line), probe.LineBytes), error);
        }
        if (destroyed.Error != Abi::SystemCallError::WouldBlock)
        {
            error = destroyed.Error;
            return false;
        }
        const Abi::SystemCallResponse waited = context.Client->Wait(context.Bootstrap->WaitSet, WaitMicroseconds);
        if (waited.Error == Abi::SystemCallError::TimedOut)
        {
            continue;
        }
        if (!Check(waited, error))
        {
            return false;
        }
        const Abi::SystemCallResponse received =
            context.Client->Receive(context.Bootstrap->Endpoint, AddressOf(context.Delivery), sizeof(context.Delivery));
        if (!Check(received, error) || (received.Value1 != DeliveryBytes))
        {
            return false;
        }
        const Foundation::UInt8* const fault = context.Delivery + FaultOffset;
        const Foundation::UInt64 address = LoadWord(fault);
        const Foundation::UInt64 instruction = LoadWord(fault + RecordInstructionOffset);
        const Foundation::UInt64 syndrome = LoadWord(fault + RecordSyndromeOffset);
        bool matches = (probe.Resolve != Resolution::NoFault) && MatchesRecord(probe, fault, syndrome);
        if (probe.Resolve == Resolution::Rse)
        {
            matches = matches && (address >= context.WindowStart) &&
                      ((address - context.WindowStart) < (RsePages * PageBytes)) && ((syndrome & RseBit) != 0U);
            sawRse = sawRse || ((syndrome & RseBit) != 0U);
            sawIncomplete = sawIncomplete || ((syndrome & IncompleteBit) != 0U);
        }
        else
        {
            matches = (matches && MatchesSite(probe, address, instruction, syndrome)) ||
                      ((probe.Resolve != Resolution::NoFault) && (probe.Alternative != nullptr) &&
                       MatchesRecord(*probe.Alternative, fault, syndrome) &&
                       MatchesSite(*probe.Alternative, address, instruction, syndrome));
        }
        if (!matches)
        {
            ReportMismatch(context, address, instruction, syndrome);
            return false;
        }
        ++faults;
        const System::CapabilityHandle token{LoadWord(context.Delivery + TokenOffset),
                                             LoadWord(context.Delivery + TokenOffset + 8U)};
        if (!ResolveProbe(context, probe, token, address, mapping, error))
        {
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool RunLazyProbe(ScenarioContext& context, const bool execute, Abi::SystemCallError& error) noexcept
{
    ComponentMapping mapping{};
    mapping.Pages = execute ? 1U : RsePages;
    if (!context.Calls->CreateMemory(context.Management, Abi::ObjectTypeFrame, mapping.Pages, 0U, mapping.Frame,
                                     error) ||
        !context.Calls->CreateMemory(context.Management, Abi::ObjectTypeMemoryObject, mapping.Pages, ReadWriteUser,
                                     mapping.Memory, error))
    {
        return false;
    }
    if (execute)
    {
        if (!context.Calls->Map(context.OwnSpace, mapping, context.WindowStart, ReadWriteUser, error))
        {
            return false;
        }
        const Foundation::UInt64 bytes = AddressOf(AsteriaIa64FaultLazyCodeEnd) - AddressOf(AsteriaIa64FaultLazyCode);
        auto* const destination =
            reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(context.WindowStart));
        for (Foundation::UInt64 index{0U}; index < bytes; ++index)
        {
            destination[index] = AsteriaIa64FaultLazyCode[index];
        }
        context.Launch->Component.FlushCode(context.WindowStart, bytes);
        if (!Check(context.Client->UnmapMemory(context.OwnSpace, context.WindowStart, 1U), error) ||
            !context.Calls->Destroy(mapping.Memory, Abi::ObjectTypeMemoryObject, error) ||
            !context.Calls->CreateMemory(context.Management, Abi::ObjectTypeMemoryObject, 1U, ReadExecuteUser,
                                         mapping.Memory, error))
        {
            return false;
        }
    }
    static constexpr char codeLine[]{"# ia64 fault: lazy instruction resolved and retried\n"};
    static constexpr char rseLine[]{"# ia64 fault: lazy RSE spills and incomplete-frame fills resolved and resumed\n"};
    const Probe probe{
        .Entry = execute ? Descriptor(AsteriaIa64FaultInstruction) : Descriptor(AsteriaIa64FaultRse),
        .Operand = context.WindowStart,
        .Vector = execute ? AlternateInstructionTlbVector : AlternateDataTlbVector,
        .Address = context.WindowStart,
        .Instruction = context.WindowStart,
        .Code = 0U,
        .Slot = 0U,
        .Cause = MissingMappingCause,
        .Execute = execute,
        .Resolve = execute ? Resolution::Execute : Resolution::Rse,
        .Line = execute ? codeLine : rseLine,
        .LineBytes = execute ? sizeof(codeLine) - 1U : sizeof(rseLine) - 1U,
    };
    if (!RunProbe(context, probe, mapping, error))
    {
        return false;
    }
    if (execute)
    {
        if (!Check(context.Client->UnmapMemory(context.OwnSpace, context.WindowStart, 1U), error))
        {
            return false;
        }
    }
    else if (!UnmapRse(context, error))
    {
        return false;
    }
    return context.Calls->Destroy(mapping.Memory, Abi::ObjectTypeMemoryObject, error) &&
           context.Calls->Destroy(mapping.Frame, Abi::ObjectTypeFrame, error);
}

[[nodiscard, gnu::noinline]] bool RunSpinnerProbe(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    const Foundation::UInt64 imageBytes =
        AddressOf(AsteriaIa64SpinnerImageEnd) - AddressOf(AsteriaIa64SpinnerImageBytes);
    if (Loader::DescribeExecutable(AsteriaIa64SpinnerImageBytes, imageBytes, context.Launch->Component.Image,
                                   g_spinnerLayout) != Loader::LoaderError::None ||
        (g_spinnerLayout.SegmentCount < 2U))
    {
        return false;
    }
    const Loader::ExecutableSegment& code = g_spinnerLayout.Segments[0U];
    const Loader::ExecutableSegment& contract = g_spinnerLayout.Segments[1U];
    if (!code.Executable || code.Writable || (code.PageOffset != 0U) || (code.Pages > context.Bootstrap->WindowPages) ||
        (contract.FileBytes < SpinnerContractBytes))
    {
        return false;
    }
    const Foundation::UInt8* const metadata = AsteriaIa64SpinnerImageBytes + contract.FileOffset;
    const Foundation::UInt64 success = LoadWord(metadata + SpinnerContractSuccessOffset);
    if ((LoadWord(metadata + SpinnerContractCodeBytesOffset) != code.FileBytes) || (success >= code.FileBytes) ||
        ((success & BundleMask) != 0U))
    {
        return false;
    }
    ComponentMapping mapping{};
    mapping.Pages = code.Pages;
    if (!context.Calls->CreateMemory(context.Management, Abi::ObjectTypeFrame, mapping.Pages, 0U, mapping.Frame,
                                     error) ||
        !context.Calls->CreateMemory(context.Management, Abi::ObjectTypeMemoryObject, mapping.Pages, ReadWriteUser,
                                     mapping.Memory, error) ||
        !context.Calls->Map(context.OwnSpace, mapping, context.WindowStart, ReadWriteUser, error))
    {
        return false;
    }
    auto* const destination =
        reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(context.WindowStart));
    for (Foundation::UInt64 index{0U}; index < code.FileBytes; ++index)
    {
        destination[index] = AsteriaIa64SpinnerImageBytes[code.FileOffset + index];
    }
    context.Launch->Component.FlushCode(context.WindowStart, code.FileBytes);
    if (!Check(context.Client->UnmapMemory(context.OwnSpace, context.WindowStart, mapping.Pages), error) ||
        !context.Calls->Destroy(mapping.Memory, Abi::ObjectTypeMemoryObject, error) ||
        !context.Calls->CreateMemory(context.Management, Abi::ObjectTypeMemoryObject, mapping.Pages, ReadExecuteUser,
                                     mapping.Memory, error) ||
        !context.Calls->Map(context.OwnSpace, mapping, context.WindowStart, ReadExecuteUser, error))
    {
        return false;
    }
    static constexpr char start[]{"# spinner: counting\n"};
    static constexpr char done[]{"# spinner: done, complete IA64 state preserved\n"};
    const Probe probe{
        .Entry = Descriptor(AsteriaIa64FaultInstruction),
        .Operand = context.WindowStart,
        .Vector = BreakVector,
        .Address = context.WindowStart + success,
        .Instruction = context.WindowStart + success,
        .Code = LoadWord(metadata + SpinnerContractImmediateOffset),
        .Slot = SpinnerSuccessSlot,
        .Cause = ExceptionCause,
        .Execute = true,
        .Resolve = Resolution::Terminate,
        .Line = done,
        .LineBytes = sizeof(done) - 1U,
    };
    return Check(context.Client->DebugWrite(context.Diagnostic, AddressOf(start), sizeof(start) - 1U), error) &&
           RunProbe(context, probe, mapping, error) &&
           Check(context.Client->UnmapMemory(context.OwnSpace, context.WindowStart, mapping.Pages), error) &&
           context.Calls->Destroy(mapping.Memory, Abi::ObjectTypeMemoryObject, error) &&
           context.Calls->Destroy(mapping.Frame, Abi::ObjectTypeFrame, error);
}
} // Namespace

bool RunIa64FaultScenarios(ScenarioContext& context, Abi::SystemCallError& error) noexcept
{
    g_active = &context;
    static constexpr char breakLine[]{"# ia64 fault: break immediate and slot delivered, terminated\n"};
    static constexpr char privilegedLine[]{"# ia64 fault: privileged operation delivered, terminated\n"};
    static constexpr char natLine[]{"# ia64 fault: NaT register consumption delivered, terminated\n"};
    static constexpr char unalignedLine[]{"# ia64 fault: unaligned IFA delivered, terminated\n"};
    static constexpr char dataLine[]{"# ia64 fault: data access rights IFA delivered, terminated\n"};
    static constexpr char instructionLine[]{"# ia64 fault: instruction access rights IFA delivered, terminated\n"};
    static constexpr char speculationLine[]{"# ia64 fault: chk.s branched to recovery, no fault\n"};
    // Static storage keeps the probes out of this function's frame.
    static constinit Probe probes[7U]{};
    const Foundation::UInt64 breakSite = AddressOf(AsteriaIa64FaultBreakSite);
    const Foundation::UInt64 privilegedSite = AddressOf(AsteriaIa64FaultPrivilegedSite);
    const Foundation::UInt64 natSite = AddressOf(AsteriaIa64FaultNatRegisterSite);
    const Foundation::UInt64 dataSite = AddressOf(AsteriaIa64FaultDataRightsSite);
    const Foundation::UInt64 operand = AddressOf(g_operand);
    probes[0U] = Probe{.Entry = Descriptor(AsteriaIa64FaultBreak),
                       .Operand = 0U,
                       .Vector = BreakVector,
                       .Address = breakSite,
                       .Instruction = breakSite,
                       .Code = 0U,
                       .Slot = 1U,
                       .Cause = ExceptionCause,
                       .Execute = true,
                       .Resolve = Resolution::Terminate,
                       .Line = breakLine,
                       .LineBytes = sizeof(breakLine) - 1U};
    probes[1U] = Probe{.Entry = Descriptor(AsteriaIa64FaultPrivileged),
                       .Operand = 0U,
                       .Vector = GeneralExceptionVector,
                       .Address = privilegedSite,
                       .Instruction = privilegedSite,
                       .Code = PrivilegedOperationCode,
                       .Slot = 0U,
                       .Cause = ExceptionCause,
                       .Execute = true,
                       .Resolve = Resolution::Terminate,
                       .Line = privilegedLine,
                       .LineBytes = sizeof(privilegedLine) - 1U};
    probes[2U] = Probe{.Entry = Descriptor(AsteriaIa64FaultNatRegister),
                       .Operand = operand,
                       .Vector = NatConsumptionVector,
                       .Address = natSite,
                       .Instruction = natSite,
                       .Code = NatRegisterCode,
                       .Slot = 0U,
                       .Cause = ExceptionCause,
                       .Execute = true,
                       .Resolve = Resolution::Terminate,
                       .Line = natLine,
                       .LineBytes = sizeof(natLine) - 1U};
    probes[3U] = Probe{.Entry = Descriptor(AsteriaIa64FaultUnaligned),
                       .Operand = operand,
                       .Vector = UnalignedReferenceVector,
                       .Address = operand + 1U,
                       .Instruction = AddressOf(AsteriaIa64FaultUnalignedSite),
                       .Code = 0U,
                       .Slot = 0U,
                       .Cause = ExceptionCause,
                       .Execute = false,
                       .Resolve = Resolution::Terminate,
                       .Line = unalignedLine,
                       .LineBytes = sizeof(unalignedLine) - 1U};
    probes[4U] = Probe{.Entry = Descriptor(AsteriaIa64FaultDataRights),
                       .Operand = dataSite,
                       .Vector = DataAccessRightsVector,
                       .Address = dataSite,
                       .Instruction = dataSite,
                       .Code = 0U,
                       .Slot = 0U,
                       .Cause = ProtectionViolationCause,
                       .Execute = false,
                       .Resolve = Resolution::Terminate,
                       .Line = dataLine,
                       .LineBytes = sizeof(dataLine) - 1U};
    // A branch into this pager's read/write data: the ledger's translation
    // has no execute right, so the fetch faults at the branch target.
    probes[5U] = Probe{.Entry = Descriptor(AsteriaIa64FaultInstruction),
                       .Operand = operand,
                       .Vector = InstructionAccessRightsVector,
                       .Address = operand,
                       .Instruction = operand,
                       .Code = 0U,
                       .Slot = 0U,
                       .Cause = ProtectionViolationCause,
                       .Execute = true,
                       .Resolve = Resolution::Terminate,
                       .Line = instructionLine,
                       .LineBytes = sizeof(instructionLine) - 1U};
    probes[6U] = Probe{.Entry = Descriptor(AsteriaIa64FaultSpeculation),
                       .Operand = operand,
                       .Vector = 0U,
                       .Address = 0U,
                       .Instruction = 0U,
                       .Code = 0U,
                       .Slot = 0U,
                       .Cause = 0U,
                       .Execute = false,
                       .Resolve = Resolution::NoFault,
                       .Line = speculationLine,
                       .LineBytes = sizeof(speculationLine) - 1U};
    for (const Probe& probe : probes)
    {
        if (!RunProbe(context, probe, ComponentMapping{}, error))
        {
            return false;
        }
    }
    static constinit Probe entryProbes[10U]{};
    static constexpr char entryEagerLine[]{"# entry state fault: Eager, supervised, terminated, reclaimed\n"};
    entryProbes[0U] = Probe{.Entry = Descriptor(AsteriaIa64FaultEager),
                            .Operand = 0U,
                            .Vector = BreakVector,
                            .Address = AddressOf(AsteriaIa64FaultEagerSite),
                            .Instruction = AddressOf(AsteriaIa64FaultEagerSite),
                            .Code = 0U,
                            .Slot = 1U,
                            .Cause = ExceptionCause,
                            .Execute = true,
                            .Resolve = Resolution::Terminate,
                            .Line = entryEagerLine,
                            .LineBytes = sizeof(entryEagerLine) - 1U};
    static constexpr char entryBigEndianLine[]{"# entry state fault: BigEndian, supervised, terminated, reclaimed\n"};
    entryProbes[1U] = Probe{.Entry = Descriptor(AsteriaIa64FaultBigEndian),
                            .Operand = 0U,
                            .Vector = BreakVector,
                            .Address = AddressOf(AsteriaIa64FaultBigEndianSite),
                            .Instruction = AddressOf(AsteriaIa64FaultBigEndianSite),
                            .Code = 0U,
                            .Slot = 1U,
                            .Cause = ExceptionCause,
                            .Execute = true,
                            .Resolve = Resolution::Terminate,
                            .Line = entryBigEndianLine,
                            .LineBytes = sizeof(entryBigEndianLine) - 1U};
    static constexpr char entryLoadDistanceLine[]{
        "# entry state fault: LoadDistance, supervised, terminated, reclaimed\n"};
    entryProbes[2U] = Probe{.Entry = Descriptor(AsteriaIa64FaultLoadDistance),
                            .Operand = 0U,
                            .Vector = BreakVector,
                            .Address = AddressOf(AsteriaIa64FaultLoadDistanceSite),
                            .Instruction = AddressOf(AsteriaIa64FaultLoadDistanceSite),
                            .Code = 0U,
                            .Slot = 1U,
                            .Cause = ExceptionCause,
                            .Execute = true,
                            .Resolve = Resolution::Terminate,
                            .Line = entryLoadDistanceLine,
                            .LineBytes = sizeof(entryLoadDistanceLine) - 1U};
    static constexpr char entryShortFrameLine[]{"# entry state fault: ShortFrame, supervised, terminated, reclaimed\n"};
    entryProbes[3U] = Probe{.Entry = Descriptor(AsteriaIa64FaultShortFrame),
                            .Operand = 0U,
                            .Vector = BreakVector,
                            .Address = AddressOf(AsteriaIa64FaultShortFrameSite),
                            .Instruction = AddressOf(AsteriaIa64FaultShortFrameSite),
                            .Code = 0U,
                            .Slot = 1U,
                            .Cause = ExceptionCause,
                            .Execute = true,
                            .Resolve = Resolution::Terminate,
                            .Line = entryShortFrameLine,
                            .LineBytes = sizeof(entryShortFrameLine) - 1U};
    static constexpr char entryLocalsLine[]{"# entry state fault: Locals, supervised, terminated, reclaimed\n"};
    entryProbes[4U] = Probe{.Entry = Descriptor(AsteriaIa64FaultLocals),
                            .Operand = 0U,
                            .Vector = BreakVector,
                            .Address = AddressOf(AsteriaIa64FaultLocalsSite),
                            .Instruction = AddressOf(AsteriaIa64FaultLocalsSite),
                            .Code = 0U,
                            .Slot = 1U,
                            .Cause = ExceptionCause,
                            .Execute = true,
                            .Resolve = Resolution::Terminate,
                            .Line = entryLocalsLine,
                            .LineBytes = sizeof(entryLocalsLine) - 1U};
    static constexpr char entryRotatingLine[]{"# entry state fault: Rotating, supervised, terminated, reclaimed\n"};
    entryProbes[5U] = Probe{.Entry = Descriptor(AsteriaIa64FaultRotating),
                            .Operand = 0U,
                            .Vector = BreakVector,
                            .Address = AddressOf(AsteriaIa64FaultRotatingSite),
                            .Instruction = AddressOf(AsteriaIa64FaultRotatingSite),
                            .Code = 0U,
                            .Slot = 1U,
                            .Cause = ExceptionCause,
                            .Execute = true,
                            .Resolve = Resolution::Terminate,
                            .Line = entryRotatingLine,
                            .LineBytes = sizeof(entryRotatingLine) - 1U};
    static constexpr char entryEagerBreakLine[]{"# entry state fault: EagerBreak, supervised, terminated, reclaimed\n"};
    entryProbes[6U] = Probe{.Entry = Descriptor(AsteriaIa64FaultEagerBreak),
                            .Operand = 0U,
                            .Vector = BreakVector,
                            .Address = AddressOf(AsteriaIa64FaultEagerBreakSite),
                            .Instruction = AddressOf(AsteriaIa64FaultEagerBreakSite),
                            .Code = 0U,
                            .Slot = 1U,
                            .Cause = ExceptionCause,
                            .Execute = true,
                            .Resolve = Resolution::Terminate,
                            .Line = entryEagerBreakLine,
                            .LineBytes = sizeof(entryEagerBreakLine) - 1U};
    static constexpr char entryIsaLine[]{"# entry state fault: IA-32 transition, supervised, terminated, reclaimed\n"};
    entryProbes[7U] = Probe{.Entry = Descriptor(AsteriaIa64FaultIsaTransition),
                            .Operand = 0U,
                            .Vector = GeneralExceptionVector,
                            .Address = AddressOf(AsteriaIa64FaultIsaTransitionSite),
                            .Instruction = AddressOf(AsteriaIa64FaultIsaTransitionSite),
                            .Code = 0x40U,
                            .Slot = 2U,
                            .Cause = ExceptionCause,
                            .Execute = true,
                            .Resolve = Resolution::Terminate,
                            .Line = entryIsaLine,
                            .LineBytes = sizeof(entryIsaLine) - 1U};
    static constexpr char entryFrameLine[]{
        "# entry state fault: PFS frame marker, supervised, terminated, reclaimed\n"};
    // SDM rev. 1.1 reserves only PFS bits 51:38 and 61:58, so br.ret may
    // restore the marker and the break then finds it in CFM; a stricter
    // implementation refuses the write instead, a supervised Reserved
    // Register/Field fault. Either way only the thread ends.
    static constinit Probe frameWrite{};
    frameWrite = Probe{.Entry = Descriptor(AsteriaIa64FaultFrameMarker),
                       .Operand = 0U,
                       .Vector = GeneralExceptionVector,
                       .Address = AddressOf(AsteriaIa64FaultFrameMarkerWrite),
                       .Instruction = AddressOf(AsteriaIa64FaultFrameMarkerWrite),
                       .Code = ReservedRegisterFieldCode,
                       .Slot = 1U,
                       .Cause = ExceptionCause,
                       .Execute = true,
                       .Resolve = Resolution::Terminate,
                       .Line = entryFrameLine,
                       .LineBytes = sizeof(entryFrameLine) - 1U};
    entryProbes[8U] = Probe{.Entry = Descriptor(AsteriaIa64FaultFrameMarker),
                            .Operand = 0U,
                            .Vector = BreakVector,
                            .Address = AddressOf(AsteriaIa64FaultFrameMarkerSite),
                            .Instruction = AddressOf(AsteriaIa64FaultFrameMarkerSite),
                            .Code = 0U,
                            .Slot = 1U,
                            .Cause = ExceptionCause,
                            .Execute = true,
                            .Resolve = Resolution::Terminate,
                            .Line = entryFrameLine,
                            .LineBytes = sizeof(entryFrameLine) - 1U,
                            .Alternative = &frameWrite};
    static constexpr char entrySpinLine[]{
        "# entry state fault: interrupt with eager RSC serviced, supervised, terminated, reclaimed\n"};
    entryProbes[9U] = Probe{.Entry = Descriptor(AsteriaIa64FaultEagerSpin),
                            .Operand = 0U,
                            .Vector = ExternalInterruptVector,
                            .Address = AddressOf(AsteriaIa64FaultEagerSpinSite),
                            .Instruction = AddressOf(AsteriaIa64FaultEagerSpinSite),
                            .Code = 0U,
                            .Slot = 0U,
                            .Cause = ExceptionCause,
                            .Execute = true,
                            .Resolve = Resolution::Terminate,
                            .Line = entrySpinLine,
                            .LineBytes = sizeof(entrySpinLine) - 1U,
                            .External = true};
    for (const Probe& probe : entryProbes)
    {
        if (!RunProbe(context, probe, ComponentMapping{}, error))
        {
            return false;
        }
    }
    return RunLazyProbe(context, true, error) && RunLazyProbe(context, false, error) &&
           RunBusyProbe(context, probes[0U], error) && RunSpinnerProbe(context, error);
}
} // Namespace Asteria::User::Pager

extern "C" [[noreturn]] void AsteriaIa64FaultScenarioExit(const Asteria::Foundation::UInt64 result) noexcept
{
    for (;;)
    {
        static_cast<void>(Asteria::User::Pager::g_active->Client->ExitThread(result));
    }
}

extern "C" void AsteriaIa64FaultScenarioUnmapRse() noexcept
{
    Asteria::User::Pager::ScenarioContext& context = *Asteria::User::Pager::g_active;
    Asteria::Abi::SystemCallError error{Asteria::Abi::SystemCallError::None};
    static_cast<void>(Asteria::User::Pager::UnmapRse(context, error));
}
#else
namespace Asteria::User::Pager
{
bool RunIa64FaultScenarios(ScenarioContext&, Abi::SystemCallError&) noexcept
{
    return true;
}
} // Namespace Asteria::User::Pager
#endif
