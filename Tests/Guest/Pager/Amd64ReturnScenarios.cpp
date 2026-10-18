// SPDX-License-Identifier: Apache-2.0
#include "Guest/Pager/TestRootService.hpp"

#include "Abi/PagerProtocol.hpp"
#include "Inventory/ReportText.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Pager/PagerWords.hpp"
#include "User/System/DeviceAccess.hpp"

namespace Asteria::User::Pager
{
#if defined(ASTERIA_TARGET_AMD64)
namespace
{
constexpr Foundation::UInt64 UserEnd{Foundation::UInt64{1U} << 47U};
constexpr Foundation::UInt64 LastPage{UserEnd - 4096U};
constexpr Foundation::UInt64 BadRip{0xFFFF'8000'0000'0000ULL};
constexpr Foundation::UInt64 GeneralProtectionVector{13U};
constexpr Foundation::UInt64 UndefinedOpcodeVector{6U};
constinit char g_rejectionLine[192U]{};
[[nodiscard]] bool ReturnFaultMatches(const Foundation::UInt8* const record, const Foundation::UInt64 instruction,
                                      const Foundation::UInt64 vector, const bool xsetbv) noexcept
{
    // Intel SDM 325462-093US, Vol. 2D, XSETBV, Protected Mode and 64-Bit
    // Mode Exceptions (6-70/6-71): CPL3 and the zero XCR0 value forbid the
    // instruction with #GP(0); missing XSAVE/OSXSAVE also permits #UD.
    const auto code = LoadWord(record + 16U);
    const bool expected = code == (vector << 32U) || (xsetbv && code == (GeneralProtectionVector << 32U));
    return LoadWord(record) == instruction && LoadWord(record + 8U) == instruction && expected && record[25U] == 4U;
}
void ReportReturnFault(ScenarioContext& context, const Foundation::UInt8* const record,
                       const Foundation::UInt64 instruction, const Foundation::UInt64 vector) noexcept
{
    Inventory::ReportText text{g_rejectionLine, sizeof(g_rejectionLine)};
    text.Text("# amd64 entry probe: unexpected fault ");
    text.Hex(LoadWord(record), 16U);
    text.Character(' ');
    text.Hex(LoadWord(record + 8U), 16U);
    text.Character(' ');
    text.Hex(LoadWord(record + 16U), 16U);
    text.Character(' ');
    text.Decimal(record[25U]);
    text.Text(" expected ");
    text.Hex(instruction, 16U);
    text.Character(' ');
    text.Decimal(vector);
    text.Character('\n');
    WriteLine(context, g_rejectionLine, text.Bytes());
}
} // namespace
#endif

bool TestRootService::RunAmd64BusyReturn(Abi::SystemCallError& error) noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    System::CapabilityHandle threads[2U]{};
    m_calls.ClearRequest();
    StoreWord(m_calls.GetRequest() + Abi::ObjectCreateThreadEntryOffset, LastPage);
    StoreWord(m_calls.GetRequest() + Abi::ObjectCreateThreadStackOffset, m_launch->StackTop);
    if (!m_calls.Create(m_bootstrap->Management, Abi::ObjectTypeThread, Abi::ObjectCreateThreadRequestSizeBytes,
                        threads[0U], error))
    {
        return false;
    }
    Foundation::Size delivered{0U};
    for (Foundation::Size attempt{0U}; attempt < 2000U && delivered != 2U; ++attempt)
    {
        const auto waited = m_context.Client->Wait(m_bootstrap->WaitSet, 10'000U);
        if (waited.Error == Abi::SystemCallError::TimedOut)
        {
            continue;
        }
        if (!Check(waited, error))
        {
            return false;
        }
        const auto received =
            m_context.Client->Receive(m_bootstrap->Endpoint, AddressOf(m_context.Delivery), sizeof(m_context.Delivery));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            continue;
        }
        constexpr Foundation::Size TokenOffset{Component::EchoDeliveryHeaderBytes + Component::EchoHeaderBytes};
        constexpr Foundation::Size RecordOffset{TokenOffset + Component::EchoAttachmentBytes};
        if (!Check(received, error) || received.Value1 != RecordOffset + Abi::PagerFaultPayloadBytes)
        {
            return false;
        }
        const auto* const record = m_context.Delivery + RecordOffset;
        const Foundation::UInt64 instruction = delivered == 0U ? LastPage + 6U : BadRip;
        const Foundation::UInt64 vector = delivered == 0U ? UndefinedOpcodeVector : GeneralProtectionVector;
        if (!ReturnFaultMatches(record, instruction, vector, delivered == 0U))
        {
            ReportReturnFault(m_context, record, instruction, vector);
            return false;
        }
        const auto token = Component::ReadEchoHandle(m_context.Delivery, 0U);
        if (delivered == 0U)
        {
            m_calls.ClearRequest();
            StoreWord(m_calls.GetRequest() + Abi::ObjectCreateThreadEntryOffset, LastPage + 128U);
            StoreWord(m_calls.GetRequest() + Abi::ObjectCreateThreadStackOffset, m_launch->StackTop);
            if (!m_calls.Create(m_bootstrap->Management, Abi::ObjectTypeThread, Abi::ObjectCreateThreadRequestSizeBytes,
                                threads[1U], error))
            {
                return false;
            }
            // Force the invalid-RIP page fault to yield behind this token.
            // Its interrupt continuation must supervise #GP instead of stopping.
            const auto held = m_context.Client->Wait(m_bootstrap->WaitSet, 10'000U);
            if (held.Error != Abi::SystemCallError::TimedOut)
            {
                error = held.Error;
                return false;
            }
        }
        if (!Check(m_context.Client->FaultTerminate(token), error) || !m_calls.Close(token, error))
        {
            return false;
        }
        ++delivered;
    }
    if (delivered != 2U)
    {
        return false;
    }
    for (const auto thread : threads)
    {
        bool exited{false};
        for (Foundation::Size attempt{0U}; attempt < 2000U && !exited; ++attempt)
        {
            const auto destroyed = m_context.Client->DestroyObject(thread, Abi::ObjectTypeThread);
            if (destroyed.Error == Abi::SystemCallError::None)
            {
                if (destroyed.Value0 != Abi::FaultTerminatedExitCode)
                {
                    return false;
                }
                exited = true;
            }
            else if (destroyed.Error == Abi::SystemCallError::WouldBlock)
            {
                const auto waited = m_context.Client->Wait(m_bootstrap->WaitSet, 10'000U);
                if (waited.Error != Abi::SystemCallError::TimedOut && !Check(waited, error))
                {
                    return false;
                }
            }
            else
            {
                error = destroyed.Error;
                return false;
            }
        }
        if (!exited || !m_calls.Close(thread, error))
        {
            return false;
        }
    }
    static constexpr char Pending[] = "# amd64 entry state: pending XSETBV supervised, terminated, reclaimed\n";
    static constexpr char Busy[] =
        "# amd64 entry state: invalid interrupt RIP supervised after busy delivery, terminated, reclaimed\n";
    return Check(m_context.Client->DebugWrite(m_bootstrap->Diagnostic, AddressOf(Pending), sizeof(Pending) - 1U),
                 error) &&
           Check(m_context.Client->DebugWrite(m_bootstrap->Diagnostic, AddressOf(Busy), sizeof(Busy) - 1U), error);
#else
    static_cast<void>(error);
    return true;
#endif
}

bool TestRootService::RunAmd64ReturnBoundary(Abi::SystemCallError& error) noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    // Intel SDM 325462-093US, Vol. 2B, SYSCALL (Operation): RCX receives
    // the next instruction, even at the four-level canonical boundary.
    constexpr Foundation::UInt8 ReadExecuteUser{0x0DU};
    ComponentMapping mapping{};
    mapping.Pages = 1U;
    if (!m_calls.CreateMemory(m_bootstrap->Management, Abi::ObjectTypeFrame, 1U, 0U, mapping.Frame, error) ||
        !m_calls.CreateMemory(m_bootstrap->Management, Abi::ObjectTypeMemoryObject, 1U, System::ReadWriteUser,
                              mapping.Memory, error) ||
        !m_calls.Map(m_bootstrap->OwnSpace, mapping, m_bootstrap->WindowStart, System::ReadWriteUser, error))
    {
        return false;
    }
    auto* const page =
        reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(m_bootstrap->WindowStart));
    // XCR0 is privileged and OSXSAVE is disabled in this profile. A user
    // attempt must reach the supervisor as one of XSETBV's listed exceptions
    // (Intel SDM 325462-093US, Vol. 2D, Protected Mode / 64-Bit Exceptions).
    static constexpr Foundation::UInt8 rejected[]{0x31U, 0xC9U, 0x31U, 0xC0U, 0x31U, 0xD2U, 0x0FU, 0x01U, 0xD1U};
    for (Foundation::Size index{0U}; index < sizeof(rejected); ++index)
    {
        page[index] = rejected[index];
    }
    // POPFQ sets TF/DF/NT/AC/ID, then SYSCALL must mask the kernel flags
    // and return safely with TF clear. UD2 proves the next user instruction
    // ran and produces the supervised completion fault (Intel SDM
    // 325462-093US Vol. 2B SYSCALL; Vol. 3B §20.3.1.4).
    static constexpr Foundation::UInt8 flags[]{0xB8U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x9CU, 0x48U, 0x81U, 0x0CU, 0x24U,
                                               0x00U, 0x45U, 0x24U, 0x00U, 0x9DU, 0x0FU, 0x05U, 0x0FU, 0x0BU};
    for (Foundation::Size index{0U}; index < sizeof(flags); ++index)
    {
        page[64U + index] = flags[index];
    }
    // An unmapped canonical upper-half target faults with that user RIP.
    static constexpr Foundation::UInt8 branch[]{0x48U, 0xB8U, 0x00U, 0x00U, 0x00U, 0x00U,
                                                0x00U, 0x00U, 0x00U, 0x00U, 0xFFU, 0xE0U};
    for (Foundation::Size index{0U}; index < sizeof(branch); ++index)
    {
        page[128U + index] = branch[index];
    }
    StoreWord(page + 130U, BadRip);
    page[4094U] = 0x0FU;
    page[4095U] = 0x05U;
    if (!Check(m_context.Client->UnmapMemory(m_bootstrap->OwnSpace, m_bootstrap->WindowStart, 1U), error) ||
        !m_calls.Destroy(mapping.Memory, Abi::ObjectTypeMemoryObject, error) ||
        !m_calls.CreateMemory(m_bootstrap->Management, Abi::ObjectTypeMemoryObject, 1U, ReadExecuteUser, mapping.Memory,
                              error) ||
        !m_calls.Map(m_bootstrap->OwnSpace, mapping, LastPage, ReadExecuteUser, error))
    {
        return false;
    }
    static constexpr Foundation::UInt64 entries[]{UserEnd - 2U, LastPage, LastPage + 64U};
    static constexpr Foundation::UInt64 instructions[]{UserEnd, LastPage + 6U, LastPage + 81U};
    static constexpr Foundation::UInt64 vectors[]{GeneralProtectionVector, UndefinedOpcodeVector,
                                                  UndefinedOpcodeVector};
    static_assert(sizeof(entries) == sizeof(instructions) && sizeof(entries) == sizeof(vectors));
    for (Foundation::Size probe{0U}; probe < (sizeof(entries) / sizeof(entries[0U])); ++probe)
    {
        m_calls.ClearRequest();
        StoreWord(m_calls.GetRequest() + Abi::ObjectCreateThreadEntryOffset, entries[probe]);
        StoreWord(m_calls.GetRequest() + Abi::ObjectCreateThreadStackOffset, m_launch->StackTop);
        System::CapabilityHandle thread{};
        if (!m_calls.Create(m_bootstrap->Management, Abi::ObjectTypeThread, Abi::ObjectCreateThreadRequestSizeBytes,
                            thread, error))
        {
            return false;
        }
        bool faulted{false};
        bool exited{false};
        for (Foundation::Size attempt{0U}; attempt < 2000U && !exited; ++attempt)
        {
            const auto destroyed = m_context.Client->DestroyObject(thread, Abi::ObjectTypeThread);
            if (destroyed.Error == Abi::SystemCallError::None)
            {
                if (!faulted || destroyed.Value0 != Abi::FaultTerminatedExitCode)
                {
                    return false;
                }
                exited = true;
                break;
            }
            if (destroyed.Error != Abi::SystemCallError::WouldBlock)
            {
                error = destroyed.Error;
                return false;
            }
            const auto waited = m_context.Client->Wait(m_bootstrap->WaitSet, 10'000U);
            if (waited.Error == Abi::SystemCallError::TimedOut)
            {
                continue;
            }
            if (!Check(waited, error))
            {
                return false;
            }
            const auto received = m_context.Client->Receive(m_bootstrap->Endpoint, AddressOf(m_context.Delivery),
                                                            sizeof(m_context.Delivery));
            if (received.Error == Abi::SystemCallError::WouldBlock)
            {
                continue;
            }
            constexpr Foundation::Size TokenOffset{Component::EchoDeliveryHeaderBytes + Component::EchoHeaderBytes};
            constexpr Foundation::Size RecordOffset{TokenOffset + Component::EchoAttachmentBytes};
            if (!Check(received, error) || received.Value1 != RecordOffset + Abi::PagerFaultPayloadBytes)
            {
                return false;
            }
            const auto* const record = m_context.Delivery + RecordOffset;
            if (faulted || !ReturnFaultMatches(record, instructions[probe], vectors[probe], probe == 1U))
            {
                ReportReturnFault(m_context, record, instructions[probe], vectors[probe]);
                return false;
            }
            const auto token = Component::ReadEchoHandle(m_context.Delivery, 0U);
            if (!Check(m_context.Client->FaultTerminate(token), error) || !m_calls.Close(token, error))
            {
                return false;
            }
            faulted = true;
        }
        if (!exited || !m_calls.Close(thread, error))
        {
            return false;
        }
    }
    if (!RunAmd64BusyReturn(error) ||
        !Check(m_context.Client->UnmapMemory(m_bootstrap->OwnSpace, LastPage, 1U), error) ||
        !m_calls.Destroy(mapping.Memory, Abi::ObjectTypeMemoryObject, error) ||
        !m_calls.Destroy(mapping.Frame, Abi::ObjectTypeFrame, error))
    {
        return false;
    }
    static constexpr char Passed[] =
        "# amd64 return boundary: SYSCALL at last two bytes supervised, terminated, reclaimed\n";
    static constexpr char Rejected[] = "# amd64 entry state: XSETBV supervised, terminated, reclaimed\n";
    static constexpr char Flags[] = "# amd64 entry state: user flags and SYSCALL supervised, terminated, reclaimed\n";
    return Check(m_context.Client->DebugWrite(m_bootstrap->Diagnostic, AddressOf(Passed), sizeof(Passed) - 1U),
                 error) &&
           Check(m_context.Client->DebugWrite(m_bootstrap->Diagnostic, AddressOf(Rejected), sizeof(Rejected) - 1U),
                 error) &&
           Check(m_context.Client->DebugWrite(m_bootstrap->Diagnostic, AddressOf(Flags), sizeof(Flags) - 1U), error);
#else
    static_cast<void>(error);
    return true;
#endif
}
} // namespace Asteria::User::Pager
