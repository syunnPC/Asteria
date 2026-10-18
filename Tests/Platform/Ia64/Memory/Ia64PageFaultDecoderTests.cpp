// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"
#include "Platform/Ia64/Memory/Ia64PageFaultDecoder.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::FaultAccess;
using Kernel::Memory::FaultCause;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::FaultRecord;
using Platform::Ia64::Memory::Ia64PageFaultDecodeError;
using Platform::Ia64::Memory::Ia64PageFaultDecoder;
using Platform::Ia64::Memory::Ia64PageFaultState;
using Platform::Ia64::Memory::Ia64PageFaultVector;

constexpr Foundation::UInt64 ExecuteBit{Foundation::UInt64{1U} << 32U};
constexpr Foundation::UInt64 WriteBit{Foundation::UInt64{1U} << 33U};
constexpr Foundation::UInt64 ReadBit{Foundation::UInt64{1U} << 34U};
constexpr Foundation::UInt64 InstructionSetBit{Foundation::UInt64{1U} << 34U};
constexpr Foundation::UInt64 PrivilegeShift{32U};

[[nodiscard]] constexpr FaultRecord Sentinel() noexcept
{
    return FaultRecord{
        .VirtualAddress = 0xA5A5'A5A5'A5A5'A5A5ULL,
        .InstructionPointer = 0x5A5A'5A5A'5A5A'5A5AULL,
        .ArchitectureSyndrome = 0x0123'4567'89AB'CDEFULL,
        .Access = FaultAccess::Execute,
        .Cause = FaultCause::InvalidMappingEncoding,
        .Privilege = FaultPrivilege::User,
        .Reserved = {1U, 2U, 3U, 4U, 5U},
    };
}

[[nodiscard]] constexpr bool RecordsEqual(const FaultRecord& left, const FaultRecord& right) noexcept
{
    if ((left.VirtualAddress != right.VirtualAddress) || (left.InstructionPointer != right.InstructionPointer) ||
        (left.ArchitectureSyndrome != right.ArchitectureSyndrome) || (left.Access != right.Access) ||
        (left.Cause != right.Cause) || (left.Privilege != right.Privilege))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < 5U; ++index)
    {
        if (left.Reserved[index] != right.Reserved[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool DecodesAs(const Ia64PageFaultVector vector, const Foundation::UInt64 interruptionStatus,
                             const Foundation::UInt64 cpl, const FaultAccess access, const FaultCause cause,
                             const FaultPrivilege privilege) noexcept
{
    constexpr Foundation::UInt64 FaultAddress{0xE000'0000'1234'5000ULL};
    constexpr Foundation::UInt64 InstructionPointer{0xA000'0000'0000'1230ULL};
    FaultRecord output{Sentinel()};
    const auto error = Ia64PageFaultDecoder::Decode(
        Ia64PageFaultState{
            .FaultAddress = FaultAddress,
            .InstructionPointer = InstructionPointer,
            .InterruptionStatus = interruptionStatus,
            .InterruptionProcessorStatus = cpl << PrivilegeShift,
            .Vector = vector,
        },
        output);
    const FaultRecord expected{
        .VirtualAddress = FaultAddress,
        .InstructionPointer = InstructionPointer,
        .ArchitectureSyndrome = interruptionStatus | (static_cast<Foundation::UInt64>(vector) << 48U),
        .Access = access,
        .Cause = cause,
        .Privilege = privilege,
        .Reserved = {},
    };
    return (error == Ia64PageFaultDecodeError::None) && RecordsEqual(output, expected);
}

void TestArchitectedFaults(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "an instruction page-not-present fault retains execute and slot data",
                   DecodesAs(Ia64PageFaultVector::PageNotPresent, ExecuteBit | (Foundation::UInt64{2U} << 41U), 3U,
                             FaultAccess::Execute, FaultCause::MissingMapping, FaultPrivilege::User));
    ASTERIA_EXPECT(runner, "a user data read of a missing page is normalized exactly",
                   DecodesAs(Ia64PageFaultVector::PageNotPresent, ReadBit, 3U, FaultAccess::Read,
                             FaultCause::MissingMapping, FaultPrivilege::User));
    ASTERIA_EXPECT(runner, "a read-modify-write fault is conservatively classified as write",
                   DecodesAs(Ia64PageFaultVector::PageNotPresent, ReadBit | WriteBit, 0U, FaultAccess::Write,
                             FaultCause::MissingMapping, FaultPrivilege::Supervisor));
    ASTERIA_EXPECT(runner, "a key-permission fault retains native ISR metadata",
                   DecodesAs(Ia64PageFaultVector::KeyPermission, WriteBit | (Foundation::UInt64{0x5U} << 36U) | 5U, 3U,
                             FaultAccess::Write, FaultCause::ProtectionViolation, FaultPrivilege::User));
    ASTERIA_EXPECT(runner, "instruction access-rights faults are execute protection failures",
                   DecodesAs(Ia64PageFaultVector::InstructionAccessRights, ExecuteBit, 1U, FaultAccess::Execute,
                             FaultCause::ProtectionViolation, FaultPrivilege::Supervisor));
    ASTERIA_EXPECT(runner, "an alternate data-TLB ledger miss is a missing mapping of the data access",
                   DecodesAs(Ia64PageFaultVector::AlternateDataTlb, WriteBit | (Foundation::UInt64{1U} << 41U), 3U,
                             FaultAccess::Write, FaultCause::MissingMapping, FaultPrivilege::User),
                   DecodesAs(Ia64PageFaultVector::AlternateDataTlb, ReadBit, 3U, FaultAccess::Read,
                             FaultCause::MissingMapping, FaultPrivilege::User));
    ASTERIA_EXPECT(runner, "CPL1 and CPL2 remain supervisor origins",
                   DecodesAs(Ia64PageFaultVector::DataAccessRights, ReadBit, 2U, FaultAccess::Read,
                             FaultCause::ProtectionViolation, FaultPrivilege::Supervisor));
}

void CheckRejected(TestContext& runner, const Ia64PageFaultState& state, const Ia64PageFaultDecodeError expected,
                   const char* const description) noexcept
{
    const FaultRecord sentinel{Sentinel()};
    FaultRecord output{sentinel};
    ASTERIA_EXPECT(runner, description, Ia64PageFaultDecoder::Decode(state, output) == expected,
                   RecordsEqual(output, sentinel));
}

void TestRejectedStates(TestContext& runner) noexcept
{
    const Ia64PageFaultState valid{
        .FaultAddress = 0x4000U,
        .InstructionPointer = 0x8000U,
        .InterruptionStatus = ReadBit,
        .InterruptionProcessorStatus = Foundation::UInt64{3U} << PrivilegeShift,
        .Vector = Ia64PageFaultVector::PageNotPresent,
    };

    Ia64PageFaultState state{valid};
    state.Vector = static_cast<Ia64PageFaultVector>(0x2400U);
    CheckRejected(runner, state, Ia64PageFaultDecodeError::UnsupportedInterruptionVector,
                  "access-bit faults stay on the kernel-internal path");

    state = valid;
    state.InterruptionProcessorStatus |= InstructionSetBit;
    CheckRejected(runner, state, Ia64PageFaultDecodeError::UnsupportedInstructionSet,
                  "the native decoder rejects IA-32 interruption state");

    state = valid;
    state.InstructionPointer |= 1U;
    CheckRejected(runner, state, Ia64PageFaultDecodeError::InvalidInstructionPointer,
                  "native IIP must identify an aligned bundle");

    constexpr Foundation::UInt64 InvalidInterruptionStatusBits[]{
        Foundation::UInt64{1U} << 24U,
        Foundation::UInt64{1U} << 39U,
        Foundation::UInt64{1U} << 40U,
        Foundation::UInt64{1U} << 63U,
    };
    for (const Foundation::UInt64 invalidBit : InvalidInterruptionStatusBits)
    {
        state = valid;
        state.InterruptionStatus |= invalidBit;
        CheckRejected(runner, state, Ia64PageFaultDecodeError::InvalidInterruptionStatus,
                      "reserved, nested, or IA-32-only ISR fields are rejected");
    }

    state = valid;
    state.InterruptionStatus = 0U;
    CheckRejected(runner, state, Ia64PageFaultDecodeError::InvalidAccessEncoding,
                  "a pager fault must identify one memory access kind");

    state = valid;
    state.InterruptionStatus = ExecuteBit | WriteBit;
    CheckRejected(runner, state, Ia64PageFaultDecodeError::InvalidAccessEncoding,
                  "instruction and data access flags cannot be combined");

    state = valid;
    state.Vector = Ia64PageFaultVector::InstructionAccessRights;
    state.InterruptionStatus = ReadBit;
    CheckRejected(runner, state, Ia64PageFaultDecodeError::InvalidAccessEncoding,
                  "the instruction-rights vector requires an execute access");

    state = valid;
    state.Vector = Ia64PageFaultVector::DataAccessRights;
    state.InterruptionStatus = ExecuteBit;
    CheckRejected(runner, state, Ia64PageFaultDecodeError::InvalidAccessEncoding,
                  "the data-rights vector rejects an execute access");

    state = valid;
    state.Vector = Ia64PageFaultVector::AlternateDataTlb;
    state.InterruptionStatus = ExecuteBit;
    CheckRejected(runner, state, Ia64PageFaultDecodeError::InvalidAccessEncoding,
                  "the alternate data-TLB vector rejects an execute access");
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64PageFaultDecoderTests"};
    Asteria::Test::TestArchitectedFaults(runner);
    Asteria::Test::TestRejectedStates(runner);
    return runner.Finish();
}
